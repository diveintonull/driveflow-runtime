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
  if (config.max_sensor_sources == 0U) {
    throw std::invalid_argument(
        "runtime sensor source capacity must be greater than zero");
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
                            SensorSampleHandler sample_handler) {
  if (!stop_requested) {
    throw std::invalid_argument("runtime stop predicate must not be empty");
  }
  if (!sample_handler) {
    throw std::invalid_argument("runtime sample handler must not be empty");
  }

  RuntimeSummary summary;
  const auto limit_reached = [&] {
    return config_.packet_count.has_value() &&
           summary.packets_received >= *config_.packet_count;
  };
  SensorStreamTracker stream_tracker(
      {.max_sources = config_.max_sensor_sources});
  SensorSampleProcessor sample_processor(std::move(sample_handler));
  WorkerPipeline pipeline(
      config_.pipeline, [&sample_processor](const ReceivedPacket& packet) {
        (void)sample_processor.process(packet);
      });

  while (!stop_requested() && !limit_reached()) {
    auto packets = receiver_.poll(config_.poll_timeout);
    for (auto& packet : packets) {
      if (stop_requested() || limit_reached()) {
        break;
      }
      (void)stream_tracker.observe(packet);
      (void)pipeline.try_submit(std::move(packet));
      ++summary.packets_received;
    }
  }

  summary.pipeline_metrics = pipeline.stop();
  summary.stream_metrics = stream_tracker.metrics();
  summary.sample_metrics = sample_processor.metrics();
  summary.receiver_metrics = receiver_.metrics();
  return summary;
}

}  // namespace driveflow::runtime
