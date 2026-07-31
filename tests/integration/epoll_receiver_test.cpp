#include "driveflow/net/udp_socket.hpp"
#include "driveflow/protocol/packet.hpp"
#include "driveflow/runtime/epoll_receiver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace driveflow::runtime {
namespace {

[[nodiscard]] const ReceivedPacket* find_by_sequence(
    std::span<const ReceivedPacket> packets, std::uint64_t sequence_number) {
  const auto position = std::find_if(
      packets.begin(), packets.end(), [sequence_number](const ReceivedPacket& packet) {
        return packet.packet.header.sequence_number == sequence_number;
      });
  return position == packets.end() ? nullptr : &*position;
}

TEST(EpollReceiverIntegrationTest,
     ReceivesValidatedPacketsFromMultipleReadyListeners) {
  EpollReceiver receiver(EpollReceiverConfig{
      .listen_endpoints =
          {
              {.address = "127.0.0.1", .port = 0U},
              {.address = "127.0.0.1", .port = 0U},
          },
  });
  const auto listeners = receiver.local_endpoints();
  ASSERT_EQ(listeners.size(), 2U);
  ASSERT_NE(listeners[0].port, 0U);
  ASSERT_NE(listeners[1].port, 0U);
  ASSERT_NE(listeners[0].port, listeners[1].port);

  constexpr std::array<std::uint8_t, 3> payload{0x10U, 0x20U, 0x30U};
  auto sender = net::UdpSocket::open();
  sender.send_to(
      protocol::encode_packet(protocol::MessageType::kImu, 11U, 101U, payload),
      listeners[0]);
  sender.send_to(
      protocol::encode_packet(protocol::MessageType::kGnss, 22U, 202U, payload),
      listeners[1]);

  const auto packets = receiver.poll(std::chrono::milliseconds{100});

  ASSERT_EQ(packets.size(), 2U);
  const auto* first = find_by_sequence(packets, 11U);
  const auto* second = find_by_sequence(packets, 22U);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->listener, listeners[0]);
  EXPECT_EQ(second->listener, listeners[1]);
  EXPECT_EQ(first->source.address, "127.0.0.1");
  EXPECT_NE(first->source.port, 0U);
  EXPECT_EQ(first->packet.payload,
            (std::vector<std::uint8_t>{0x10U, 0x20U, 0x30U}));
  EXPECT_GT(first->receive_timestamp_ns, 0U);
  EXPECT_GT(second->receive_timestamp_ns, 0U);
}

TEST(EpollReceiverIntegrationTest,
     RejectsMalformedDatagramAndContinuesReceiving) {
  EpollReceiver receiver(EpollReceiverConfig{
      .listen_endpoints = {{.address = "127.0.0.1", .port = 0U}},
  });
  const auto listener = receiver.local_endpoints().front();
  auto sender = net::UdpSocket::open();

  constexpr std::array<std::uint8_t, 4> malformed{
      0xdeU, 0xadU, 0xbeU, 0xefU};
  sender.send_to(malformed, listener);
  constexpr std::array<std::uint8_t, 1> payload{0x42U};
  sender.send_to(
      protocol::encode_packet(protocol::MessageType::kCameraMeta, 33U, 303U,
                              payload),
      listener);

  const auto packets = receiver.poll(std::chrono::milliseconds{100});

  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets.front().packet.header.sequence_number, 33U);
  const auto metrics = receiver.metrics();
  EXPECT_EQ(metrics.epoll_wakeups, 1U);
  EXPECT_EQ(metrics.datagrams_received, 2U);
  EXPECT_EQ(metrics.packets_accepted, 1U);
  EXPECT_EQ(metrics.packets_rejected, 1U);
}

}  // namespace
}  // namespace driveflow::runtime
