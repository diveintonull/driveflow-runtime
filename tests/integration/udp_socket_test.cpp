#include "driveflow/net/udp_socket.hpp"
#include "driveflow/protocol/packet.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace driveflow::net {
namespace {

TEST(UdpSocketIntegrationTest, TransfersEncodedPacketOverIpv4Loopback) {
  auto receiver = UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const Ipv4Endpoint destination = receiver.local_endpoint();
  ASSERT_EQ(destination.address, "127.0.0.1");
  ASSERT_NE(destination.port, 0U);

  constexpr std::array<std::uint8_t, 4> payload{1U, 2U, 3U, 4U};
  const auto encoded = protocol::encode_packet(protocol::MessageType::kGnss, 42U, 123'456U,
                                               payload);
  auto sender = UdpSocket::open();
  sender.send_to(encoded, destination);

  const ReceivedDatagram datagram = receiver.receive();
  EXPECT_EQ(datagram.source.address, "127.0.0.1");
  EXPECT_NE(datagram.source.port, 0U);

  const auto decoded = protocol::decode_packet(datagram.bytes);
  ASSERT_TRUE(decoded);
  ASSERT_TRUE(decoded.packet.has_value());
  EXPECT_EQ(decoded.packet->header.message_type, protocol::MessageType::kGnss);
  EXPECT_EQ(decoded.packet->header.sequence_number, 42U);
  EXPECT_EQ(decoded.packet->payload,
            std::vector<std::uint8_t>(payload.begin(), payload.end()));
}

TEST(UdpSocketIntegrationTest, RejectsInvalidIpv4Address) {
  EXPECT_THROW((void)UdpSocket::bind_to({.address = "not-an-address", .port = 0U}),
               std::invalid_argument);
}

TEST(UdpSocketIntegrationTest, RejectsPayloadLargerThanIpv4UdpLimit) {
  auto sender = UdpSocket::open();
  const std::vector<std::uint8_t> oversized(kMaxUdpPayloadSize + 1U, 0U);

  EXPECT_THROW(sender.send_to(oversized, {.address = "127.0.0.1", .port = 9U}),
               std::length_error);
}

TEST(UdpSocketIntegrationTest, ReportsDatagramTruncation) {
  auto receiver = UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  auto sender = UdpSocket::open();
  constexpr std::array<std::uint8_t, 8> bytes{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
  sender.send_to(bytes, receiver.local_endpoint());

  EXPECT_THROW((void)receiver.receive(4U), std::length_error);
}

}  // namespace
}  // namespace driveflow::net
