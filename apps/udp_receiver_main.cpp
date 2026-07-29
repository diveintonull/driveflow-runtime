#include "driveflow/net/udp_socket.hpp"
#include "driveflow/protocol/packet.hpp"

#include <charconv>
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
    const auto decoded = driveflow::protocol::decode_packet(datagram.bytes);
    if (!decoded) {
      std::cerr << "rejected packet from " << datagram.source.address << ':'
                << datagram.source.port << ": "
                << driveflow::protocol::to_string(decoded.error) << '\n';
      return 2;
    }

    const auto& header = decoded.packet->header;
    std::cout << "received packet from " << datagram.source.address << ':'
              << datagram.source.port << " type="
              << static_cast<std::uint16_t>(header.message_type)
              << " sequence=" << header.sequence_number
              << " payload_bytes=" << decoded.packet->payload.size() << '\n';
  } catch (const std::exception& error) {
    std::cerr << "receive failed: " << error.what() << '\n';
    return 3;
  }
  return 0;
}
