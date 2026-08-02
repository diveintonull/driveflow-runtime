#pragma once

#include "driveflow/runtime/sensor_stream_tracker.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace driveflow::runtime {

enum class SourceHealthStatus {
  kUnknown,
  kHealthy,
  kDegraded,
  kOffline,
};

[[nodiscard]] const char* to_string(SourceHealthStatus status) noexcept;

enum class SourceHealthIssue {
  kPayloadRejected,
  kQueueDropped,
};

struct SourceHealthMonitorConfig {
  std::size_t max_sources{256U};
  std::uint64_t healthy_after_packets{2U};
  std::chrono::nanoseconds degraded_after{std::chrono::milliseconds{500}};
  std::chrono::nanoseconds offline_after{std::chrono::seconds{2}};
  std::chrono::nanoseconds recovery_after{std::chrono::seconds{1}};
};

struct SourceHealthSnapshot {
  SensorSource source;
  SourceHealthStatus status{SourceHealthStatus::kUnknown};
  std::uint64_t packets_received{};
  std::uint64_t first_receive_timestamp_ns{};
  std::uint64_t last_receive_timestamp_ns{};
  std::uint64_t inactivity_ns{};
  double current_rate_hz{};
  std::uint64_t latest_latency_ns{};
  std::uint64_t maximum_latency_ns{};
  std::uint64_t timestamp_anomalies{};
  std::uint64_t gap_observations{};
  std::uint64_t duplicate_observations{};
  std::uint64_t reordered_observations{};
  std::uint64_t missing_samples_inferred{};
  std::uint64_t payloads_rejected{};
  std::uint64_t packets_dropped_queue_full{};
};

struct SourceHealthReport {
  SensorStreamMetrics stream_metrics;
  std::vector<SourceHealthSnapshot> sources;
};

// Combines receive-order sequence tracking, bounded per-source measurements,
// and health-state evaluation behind one thread-safe interface. Packet arrival
// must still be observed in receive order. Issues may be reported concurrently
// by worker threads after typed payload processing.
class SourceHealthMonitor {
 public:
  explicit SourceHealthMonitor(
      SourceHealthMonitorConfig config = SourceHealthMonitorConfig{});
  ~SourceHealthMonitor();

  SourceHealthMonitor(const SourceHealthMonitor&) = delete;
  SourceHealthMonitor& operator=(const SourceHealthMonitor&) = delete;
  SourceHealthMonitor(SourceHealthMonitor&&) = delete;
  SourceHealthMonitor& operator=(SourceHealthMonitor&&) = delete;

  // Returns kUntracked when the configured source capacity is exhausted. An
  // untracked packet has no per-source snapshot but remains in stream metrics.
  [[nodiscard]] SequenceObservation observe_packet(
      const ReceivedPacket& packet);

  // Records a post-receive problem for an already admitted source. Calls for
  // unknown or untracked sources are ignored because no bounded state exists.
  void observe_issue(const ReceivedPacket& packet, SourceHealthIssue issue);

  // Evaluates liveness at now_timestamp_ns and returns a stable copy sorted by
  // source identity. Passing a time before the latest observation clamps
  // inactivity to zero.
  [[nodiscard]] SourceHealthReport report(
      std::uint64_t now_timestamp_ns) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace driveflow::runtime
