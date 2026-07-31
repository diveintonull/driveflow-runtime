#include "driveflow/net/udp_socket.hpp"
#include "driveflow/protocol/packet.hpp"
#include "driveflow/runtime/runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace driveflow::runtime {
namespace {

TEST(RuntimeIntegrationTest, DispatchesPacketsToWorkersAndDrainsAtCount) {
  RuntimeConfig config;
  config.receiver.listen_endpoints = {
      {.address = "127.0.0.1", .port = 0U},
  };
  config.packet_count = 2U;
  config.poll_timeout = std::chrono::milliseconds{50};
  config.pipeline = {
      .worker_count = 2U,
      .queue_capacity = 8U,
  };
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
  std::vector<std::thread::id> handler_threads;
  std::mutex delivered_mutex;
  const auto runtime_thread = std::this_thread::get_id();
  const auto summary = runtime.run(
      [] { return false; },
      [&](const ReceivedPacket& packet) {
        const std::scoped_lock lock(delivered_mutex);
        delivered_sequences.push_back(packet.packet.header.sequence_number);
        handler_threads.push_back(std::this_thread::get_id());
      });

  std::ranges::sort(delivered_sequences);
  EXPECT_EQ(delivered_sequences,
            (std::vector<std::uint64_t>{41U, 42U}));
  ASSERT_EQ(handler_threads.size(), 2U);
  EXPECT_NE(handler_threads[0], runtime_thread);
  EXPECT_NE(handler_threads[1], runtime_thread);
  EXPECT_EQ(summary.packets_received, 2U);
  EXPECT_EQ(summary.pipeline_metrics.packets_submitted, 2U);
  EXPECT_EQ(summary.pipeline_metrics.packets_processed, 2U);
  EXPECT_EQ(summary.pipeline_metrics.packets_dropped_queue_full, 0U);
  EXPECT_EQ(summary.pipeline_metrics.handler_failures, 0U);
  EXPECT_EQ(summary.receiver_metrics.datagrams_received, 2U);
  EXPECT_EQ(summary.receiver_metrics.packets_accepted, 2U);
  EXPECT_EQ(summary.receiver_metrics.packets_rejected, 0U);
}

}  // namespace
}  // namespace driveflow::runtime
