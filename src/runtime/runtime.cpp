#include "driveflow/runtime/runtime.hpp"

#include <chrono>
#include <cstdint>
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
  if (config.health.max_sources == 0U) {
    throw std::invalid_argument(
        "runtime sensor source capacity must be greater than zero");
  }
  if (config.health.healthy_after_packets == 0U ||
      config.health.degraded_after.count() <= 0 ||
      config.health.recovery_after.count() <= 0) {
    throw std::invalid_argument(
        "runtime source health counts and durations must be positive");
  }
  if (config.health.offline_after <= config.health.degraded_after) {
    throw std::invalid_argument(
        "runtime offline timeout must exceed degraded timeout");
  }
  return config;
}

[[nodiscard]] std::uint64_t current_monotonic_timestamp_ns() noexcept {
  const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
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
  SourceHealthMonitor health_monitor(config_.health);
  SensorSampleProcessor sample_processor(std::move(sample_handler));
  WorkerPipeline pipeline(
      config_.pipeline,
      [&sample_processor, &health_monitor](const ReceivedPacket& packet) {
        const auto error = sample_processor.process(packet);
        if (error != protocol::SensorPayloadError::kNone) {
          health_monitor.observe_issue(
              packet, SourceHealthIssue::kPayloadRejected);
        }
      });

  while (!stop_requested() && !limit_reached()) {
    auto packets = receiver_.poll(config_.poll_timeout);
    for (auto& packet : packets) {
      if (stop_requested() || limit_reached()) {
        break;
      }
      (void)health_monitor.observe_packet(packet);
      const auto submit_result = pipeline.try_submit(std::move(packet));
      if (submit_result == SubmitResult::kQueueFull) {
        health_monitor.observe_issue(packet, SourceHealthIssue::kQueueDropped);
      }
      ++summary.packets_received;
    }
  }

  const auto report_timestamp_ns = current_monotonic_timestamp_ns();
  summary.pipeline_metrics = pipeline.stop();
  summary.sample_metrics = sample_processor.metrics();
  summary.receiver_metrics = receiver_.metrics();
  auto health_report = health_monitor.report(report_timestamp_ns);
  summary.stream_metrics = health_report.stream_metrics;
  summary.source_health = std::move(health_report.sources);
  return summary;
}

}  // namespace driveflow::runtime
