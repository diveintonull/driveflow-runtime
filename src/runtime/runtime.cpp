#include "driveflow/runtime/runtime.hpp"

#include <stdexcept>
#include <utility>

namespace driveflow::runtime {
namespace {

[[nodiscard]] RuntimeConfig validate_config(const RuntimeConfig& config) {
  if (config.packet_count.has_value() && *config.packet_count == 0U) {
    throw std::invalid_argument("runtime packet count must be greater than zero");
  }
  if (config.poll_timeout.count() <= 0) {
    throw std::invalid_argument("runtime poll timeout must be greater than zero");
  }
  if (config.pipeline.worker_count == 0U) {
    throw std::invalid_argument("runtime worker count must be greater than zero");
  }
  if (config.pipeline.queue_capacity == 0U) {
    throw std::invalid_argument(
        "runtime worker queue capacity must be greater than zero");
  }
  return config;
}

}  // namespace

Runtime::Runtime(const RuntimeConfig& config)
    : config_(validate_config(config)), receiver_(config_.receiver) {}

std::span<const net::Ipv4Endpoint> Runtime::local_endpoints() const noexcept {
  return receiver_.local_endpoints();
}

RuntimeSummary Runtime::run(StopPredicate stop_requested,
                            PacketHandler packet_handler) {
  if (!stop_requested) {
    throw std::invalid_argument("runtime stop predicate must not be empty");
  }
  if (!packet_handler) {
    throw std::invalid_argument("runtime packet handler must not be empty");
  }

  RuntimeSummary summary;
  const auto limit_reached = [&] {
    return config_.packet_count.has_value() &&
           summary.packets_received >= *config_.packet_count;
  };
  WorkerPipeline pipeline(config_.pipeline, std::move(packet_handler));

  while (!stop_requested() && !limit_reached()) {
    auto packets = receiver_.poll(config_.poll_timeout);
    for (auto& packet : packets) {
      if (stop_requested() || limit_reached()) {
        break;
      }
      (void)pipeline.try_submit(std::move(packet));
      ++summary.packets_received;
    }
  }

  summary.pipeline_metrics = pipeline.stop();
  summary.receiver_metrics = receiver_.metrics();
  return summary;
}

}  // namespace driveflow::runtime
