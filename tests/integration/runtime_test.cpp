#include "driveflow/net/udp_socket.hpp"
#include "driveflow/protocol/packet.hpp"
#include "driveflow/runtime/runtime.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace driveflow::runtime {
namespace {

TEST(RuntimeIntegrationTest, StopsAtConfiguredPacketCount) {
  RuntimeConfig config;
  config.receiver.listen_endpoints = {
      {.address = "127.0.0.1", .port = 0U},
  };
  config.packet_count = 2U;
  config.poll_timeout = std::chrono::milliseconds{50};
  Runtime runtime(config);
  const auto listener = runtime.local_endpoints().front();

  constexpr std::array<std::uint8_t, 1> payload{0x5aU};
  auto sender = net::UdpSocket::open();
  sender.send_to(
      protocol::encode_packet(protocol::MessageType::kImu, 41U, 401U, payload),
      listener);
  sender.send_to(
      protocol::encode_packet(protocol::MessageType::kGnss, 42U, 402U, payload),
      listener);

  std::vector<std::uint64_t> delivered_sequences;
  const auto summary = runtime.run(
      [] { return false; },
      [&](const ReceivedPacket& packet) {
        delivered_sequences.push_back(packet.packet.header.sequence_number);
      });

  EXPECT_EQ(delivered_sequences,
            (std::vector<std::uint64_t>{41U, 42U}));
  EXPECT_EQ(summary.packets_delivered, 2U);
  EXPECT_EQ(summary.receiver_metrics.datagrams_received, 2U);
  EXPECT_EQ(summary.receiver_metrics.packets_accepted, 2U);
  EXPECT_EQ(summary.receiver_metrics.packets_rejected, 0U);
}

}  // namespace
}  // namespace driveflow::runtime
