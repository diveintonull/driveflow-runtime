#include "driveflow/net/udp_socket.hpp"
#include "driveflow/protocol/packet.hpp"
#include "driveflow/protocol/sensor_payload.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

namespace {

[[nodiscard]] std::uint16_t parse_port(std::string_view text) {
  unsigned int value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value > 65'535U) {
    throw std::invalid_argument("port must be an integer between 0 and 65535");
  }
  return static_cast<std::uint16_t>(value);
}

void print_sensor_payload(const driveflow::protocol::SensorPayload& payload) {
  std::visit(
      [](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, driveflow::protocol::ImuSample>) {
          std::cout << " acceleration=(" << value.linear_acceleration_mps2.x << ','
                    << value.linear_acceleration_mps2.y << ','
                    << value.linear_acceleration_mps2.z << ") angular_velocity=("
                    << value.angular_velocity_rps.x << ',' << value.angular_velocity_rps.y << ','
                    << value.angular_velocity_rps.z << ')';
        } else if constexpr (std::is_same_v<Value, driveflow::protocol::GnssFix>) {
          std::cout << " latitude=" << value.latitude_deg
                    << " longitude=" << value.longitude_deg
                    << " altitude_m=" << value.altitude_m;
        } else {
          std::cout << " frame_id=" << value.frame_id << " dimensions=" << value.width << 'x'
                    << value.height << " extra_bytes=" << value.extra_data.size();
        }
      },
      payload);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::cerr << "usage: driveflow_udp_receiver <bind-IPv4-address> <port>\n";
    return 1;
  }

  try {
    const driveflow::net::Ipv4Endpoint requested{.address = argv[1],
                                                 .port = parse_port(argv[2])};
    auto socket = driveflow::net::UdpSocket::bind_to(requested);
    const auto actual = socket.local_endpoint();
    std::cout << "listening on " << actual.address << ':' << actual.port << '\n';

    const auto datagram = socket.receive();
    const auto decoded_packet = driveflow::protocol::decode_packet(datagram.bytes);
    if (!decoded_packet) {
      std::cerr << "rejected packet from " << datagram.source.address << ':'
                << datagram.source.port << ": "
                << driveflow::protocol::to_string(decoded_packet.error) << '\n';
      return 2;
    }

    const auto& packet = *decoded_packet.packet;
    const auto decoded_payload =
        driveflow::protocol::decode_sensor_payload(packet.header.message_type, packet.payload);
    if (!decoded_payload) {
      std::cerr << "rejected sensor payload from " << datagram.source.address << ':'
                << datagram.source.port << ": "
                << driveflow::protocol::to_string(decoded_payload.error) << '\n';
      return 3;
    }

    std::cout << "received packet from " << datagram.source.address << ':'
              << datagram.source.port << " type="
              << static_cast<std::uint16_t>(packet.header.message_type)
              << " sequence=" << packet.header.sequence_number
              << " payload_bytes=" << packet.payload.size();
    print_sensor_payload(*decoded_payload.payload);
    std::cout << '\n';
  } catch (const std::exception& error) {
    std::cerr << "receive failed: " << error.what() << '\n';
    return 4;
  }
  return 0;
}
