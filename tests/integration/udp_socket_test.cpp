#include "driveflow/net/udp_socket.hpp"
#include "driveflow/protocol/packet.hpp"
#include "driveflow/protocol/sensor_payload.hpp"

#include <cstdint>
#include <stdexcept>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace driveflow::net {
namespace {

TEST(UdpSocketIntegrationTest, TransfersEncodedPacketOverIpv4Loopback) {
  auto receiver = UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const Ipv4Endpoint destination = receiver.local_endpoint();
  ASSERT_EQ(destination.address, "127.0.0.1");
  ASSERT_NE(destination.port, 0U);

  const protocol::GnssFix fix{
      .latitude_deg = 1.3521,
      .longitude_deg = 103.8198,
      .altitude_m = 15.75,
      .horizontal_accuracy_m = 0.8F,
      .vertical_accuracy_m = 1.2F,
  };
  const auto payload = protocol::encode_sensor_payload(protocol::SensorPayload{fix});
  const auto encoded = protocol::encode_packet(protocol::MessageType::kGnss, 42U, 123'456U,
                                               payload);
  auto sender = UdpSocket::open();
  sender.send_to(encoded, destination);

  const ReceivedDatagram datagram = receiver.receive();
  EXPECT_EQ(datagram.source.address, "127.0.0.1");
  EXPECT_NE(datagram.source.port, 0U);

  const auto decoded_packet = protocol::decode_packet(datagram.bytes);
  ASSERT_TRUE(decoded_packet);
  ASSERT_TRUE(decoded_packet.packet.has_value());
  EXPECT_EQ(decoded_packet.packet->header.message_type, protocol::MessageType::kGnss);
  EXPECT_EQ(decoded_packet.packet->header.sequence_number, 42U);

  const auto decoded_payload = protocol::decode_sensor_payload(
      decoded_packet.packet->header.message_type, decoded_packet.packet->payload);
  ASSERT_TRUE(decoded_payload);
  ASSERT_TRUE(decoded_payload.payload.has_value());
  EXPECT_EQ(std::get<protocol::GnssFix>(*decoded_payload.payload), fix);
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

TEST(UdpSocketIntegrationTest, NonBlockingReceiveReportsNoData) {
  auto receiver = UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  receiver.set_non_blocking();

  EXPECT_FALSE(receiver.try_receive().has_value());
}

}  // namespace
}  // namespace driveflow::net
