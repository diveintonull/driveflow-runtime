#include "driveflow/net/udp_socket.hpp"
#include "driveflow/protocol/packet.hpp"
#include "driveflow/protocol/sensor_payload.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

[[nodiscard]] std::uint16_t parse_port(std::string_view text) {
  unsigned int value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value > 65'535U) {
    throw std::invalid_argument("port must be an integer between 0 and 65535");
  }
  return static_cast<std::uint16_t>(value);
}

[[nodiscard]] std::uint64_t current_timestamp_ns() {
  const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  return static_cast<std::uint64_t>(nanoseconds);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::cerr << "usage: driveflow_udp_sender <IPv4-address> <port>\n";
    return 1;
  }

  try {
    const driveflow::net::Ipv4Endpoint destination{.address = argv[1],
                                                   .port = parse_port(argv[2])};
    const driveflow::protocol::SensorPayload sample{driveflow::protocol::ImuSample{
        .linear_acceleration_mps2 = {.x = 0.0F, .y = 0.0F, .z = 9.81F},
        .angular_velocity_rps = {.x = 0.01F, .y = -0.02F, .z = 0.03F},
    }};
    const auto payload = driveflow::protocol::encode_sensor_payload(sample);
    const auto packet = driveflow::protocol::encode_packet(
        driveflow::protocol::message_type(sample), 1U, current_timestamp_ns(), payload);

    auto socket = driveflow::net::UdpSocket::open();
    socket.send_to(packet, destination);
    std::cout << "sent " << packet.size() << " bytes to " << destination.address << ':'
              << destination.port << '\n';
  } catch (const std::exception& error) {
    std::cerr << "send failed: " << error.what() << '\n';
    return 2;
  }
  return 0;
}
