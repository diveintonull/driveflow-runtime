#pragma once

#include "driveflow/net/udp_socket.hpp"
#include "driveflow/protocol/packet.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace driveflow::runtime {

struct EpollReceiverConfig {
  std::vector<net::Ipv4Endpoint> listen_endpoints;
  std::size_t max_datagrams_per_listener_per_poll{64U};
};
struct ReceiverMetrics {
  std::uint64_t epoll_wakeups{};
  std::uint64_t datagrams_received{};
  std::uint64_t packets_accepted{};
  std::uint64_t packets_rejected{};
};


struct ReceivedPacket {
  protocol::Packet packet;
  net::Ipv4Endpoint source;
  net::Ipv4Endpoint listener;
  std::uint64_t receive_timestamp_ns{};
};

class EpollReceiver {
 public:
  explicit EpollReceiver(const EpollReceiverConfig& config);

  EpollReceiver(const EpollReceiver&) = delete;
  EpollReceiver& operator=(const EpollReceiver&) = delete;

  ~EpollReceiver();

  [[nodiscard]] std::span<const net::Ipv4Endpoint> local_endpoints() const noexcept;
  [[nodiscard]] std::vector<ReceivedPacket> poll(std::chrono::milliseconds timeout);
  [[nodiscard]] ReceiverMetrics metrics() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace driveflow::runtime
