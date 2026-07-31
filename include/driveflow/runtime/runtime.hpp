#pragma once

#include "driveflow/runtime/epoll_receiver.hpp"
#include "driveflow/runtime/runtime_config.hpp"
#include "driveflow/runtime/worker_pipeline.hpp"

#include <cstdint>
#include <functional>
#include <span>

namespace driveflow::runtime {

using StopPredicate = std::function<bool()>;
using PacketHandler = PacketProcessor;

struct RuntimeSummary {
  std::uint64_t packets_received{};
  ReceiverMetrics receiver_metrics;
  WorkerPipelineMetrics pipeline_metrics;
};

class Runtime {
 public:
  explicit Runtime(const RuntimeConfig& config);

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  [[nodiscard]] std::span<const net::Ipv4Endpoint> local_endpoints() const noexcept;
  [[nodiscard]] RuntimeSummary run(StopPredicate stop_requested,
                                   PacketHandler packet_handler);

 private:
  RuntimeConfig config_;
  EpollReceiver receiver_;
};

}  // namespace driveflow::runtime
