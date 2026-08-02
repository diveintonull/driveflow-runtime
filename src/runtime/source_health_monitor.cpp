#include "driveflow/runtime/source_health_monitor.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace driveflow::runtime {
namespace {

constexpr std::size_t kRecentTimestampCapacity = 64U;
constexpr double kNanosecondsPerSecond = 1'000'000'000.0;

void saturating_increment(std::uint64_t& value) noexcept {
  if (value != std::numeric_limits<std::uint64_t>::max()) {
    ++value;
  }
}

void saturating_add(std::uint64_t& value, std::uint64_t increment) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (increment > maximum - value) {
    value = maximum;
    return;
  }
  value += increment;
}

[[nodiscard]] std::uint64_t duration_ns(
    std::chrono::nanoseconds duration) noexcept {
  return static_cast<std::uint64_t>(duration.count());
}

[[nodiscard]] std::uint64_t elapsed_or_zero(std::uint64_t now,
                                            std::uint64_t then) noexcept {
  return now >= then ? now - then : 0U;
}

[[nodiscard]] SourceHealthMonitorConfig validate_config(
    SourceHealthMonitorConfig config) {
  if (config.max_sources == 0U) {
    throw std::invalid_argument(
        "source health monitor capacity must be greater than zero");
  }
  if (config.healthy_after_packets == 0U) {
    throw std::invalid_argument(
        "source health healthy packet count must be greater than zero");
  }
  if (config.degraded_after.count() <= 0) {
    throw std::invalid_argument(
        "source health degraded timeout must be greater than zero");
  }
  if (config.offline_after <= config.degraded_after) {
    throw std::invalid_argument(
        "source health offline timeout must exceed degraded timeout");
  }
  if (config.recovery_after.count() <= 0) {
    throw std::invalid_argument(
        "source health recovery duration must be greater than zero");
  }
  return config;
}

}  // namespace

class SourceHealthMonitor::Impl {
 public:
  explicit Impl(SourceHealthMonitorConfig config)
      : config_(validate_config(std::move(config))),
        stream_tracker_({.max_sources = config_.max_sources}) {}

  [[nodiscard]] SequenceObservation observe_packet(
      const ReceivedPacket& packet) {
    const std::scoped_lock lock(mutex_);
    const auto observation = stream_tracker_.observe(packet);
    if (observation.status == SequenceStatus::kUntracked) {
      return observation;
    }

    const auto source = identify_sensor_source(packet);
    auto state = find_source(source);
    if (state == sources_.end()) {
      sources_.push_back(SourceState{.source = source});
      state = std::prev(sources_.end());
    }

    record_packet(*state, packet, observation);
    return observation;
  }

  void observe_issue(const ReceivedPacket& packet, SourceHealthIssue issue) {
    const std::scoped_lock lock(mutex_);
    const auto state = find_source(identify_sensor_source(packet));
    if (state == sources_.end()) {
      return;
    }

    switch (issue) {
      case SourceHealthIssue::kPayloadRejected:
        saturating_increment(state->payloads_rejected);
        break;
      case SourceHealthIssue::kQueueDropped:
        saturating_increment(state->packets_dropped_queue_full);
        break;
    }
    mark_issue(*state, packet.receive_timestamp_ns);
  }

  [[nodiscard]] SourceHealthReport report(
      std::uint64_t now_timestamp_ns) const {
    const std::scoped_lock lock(mutex_);
    SourceHealthReport result{
        .stream_metrics = stream_tracker_.metrics(),
        .sources = {},
    };
    result.sources.reserve(sources_.size());
    for (const auto& state : sources_) {
      result.sources.push_back(make_snapshot(state, now_timestamp_ns));
    }
    std::ranges::sort(result.sources, {}, [](const SourceHealthSnapshot& value) {
      return std::tuple{
          value.source.remote_endpoint.address,
          value.source.remote_endpoint.port,
          value.source.listener_endpoint.address,
          value.source.listener_endpoint.port,
          static_cast<std::uint16_t>(value.source.message_type),
      };
    });
    return result;
  }

 private:
  struct SourceState {
    SensorSource source;
    std::array<std::uint64_t, kRecentTimestampCapacity> recent_timestamps{};
    std::size_t recent_timestamp_count{};
    std::size_t next_timestamp_index{};
    std::uint64_t packets_received{};
    std::uint64_t first_receive_timestamp_ns{};
    std::uint64_t last_receive_timestamp_ns{};
    std::uint64_t latest_latency_ns{};
    std::uint64_t maximum_latency_ns{};
    std::uint64_t timestamp_anomalies{};
    std::uint64_t gap_observations{};
    std::uint64_t duplicate_observations{};
    std::uint64_t reordered_observations{};
    std::uint64_t missing_samples_inferred{};
    std::uint64_t payloads_rejected{};
    std::uint64_t packets_dropped_queue_full{};
    std::uint64_t healthy_packets_since_issue{};
    std::uint64_t last_issue_timestamp_ns{};
    bool issue_seen{false};
  };

  using SourceIterator = std::vector<SourceState>::iterator;

  [[nodiscard]] SourceIterator find_source(const SensorSource& source) {
    return std::find_if(
        sources_.begin(), sources_.end(),
        [&](const SourceState& candidate) { return candidate.source == source; });
  }

  void record_packet(SourceState& state, const ReceivedPacket& packet,
                     const SequenceObservation& observation) {
    const auto receive_timestamp =
        std::max(packet.receive_timestamp_ns, state.last_receive_timestamp_ns);
    if (state.packets_received == 0U) {
      state.first_receive_timestamp_ns = receive_timestamp;
    }
    state.last_receive_timestamp_ns = receive_timestamp;
    saturating_increment(state.packets_received);
    record_recent_timestamp(state, receive_timestamp);

    bool packet_has_issue = record_latency(state, packet);
    switch (observation.status) {
      case SequenceStatus::kFirst:
      case SequenceStatus::kInOrder:
        break;
      case SequenceStatus::kGap:
        saturating_increment(state.gap_observations);
        saturating_add(state.missing_samples_inferred,
                       observation.missing_samples);
        packet_has_issue = true;
        break;
      case SequenceStatus::kDuplicate:
        saturating_increment(state.duplicate_observations);
        packet_has_issue = true;
        break;
      case SequenceStatus::kReordered:
        saturating_increment(state.reordered_observations);
        packet_has_issue = true;
        break;
      case SequenceStatus::kUntracked:
        return;
    }

    if (packet_has_issue) {
      mark_issue(state, receive_timestamp);
    } else {
      saturating_increment(state.healthy_packets_since_issue);
    }
  }

  static void record_recent_timestamp(SourceState& state,
                                      std::uint64_t timestamp) noexcept {
    state.recent_timestamps[state.next_timestamp_index] = timestamp;
    state.next_timestamp_index =
        (state.next_timestamp_index + 1U) % kRecentTimestampCapacity;
    state.recent_timestamp_count =
        std::min(state.recent_timestamp_count + 1U, kRecentTimestampCapacity);
  }

  [[nodiscard]] static bool record_latency(
      SourceState& state, const ReceivedPacket& packet) noexcept {
    if (packet.packet.header.source_timestamp_ns >
        packet.receive_timestamp_ns) {
      state.latest_latency_ns = 0U;
      saturating_increment(state.timestamp_anomalies);
      return true;
    }

    state.latest_latency_ns =
        packet.receive_timestamp_ns - packet.packet.header.source_timestamp_ns;
    state.maximum_latency_ns =
        std::max(state.maximum_latency_ns, state.latest_latency_ns);
    return false;
  }

  static void mark_issue(SourceState& state,
                         std::uint64_t timestamp) noexcept {
    state.issue_seen = true;
    state.last_issue_timestamp_ns =
        std::max(state.last_issue_timestamp_ns, timestamp);
    state.healthy_packets_since_issue = 0U;
  }

  [[nodiscard]] double current_rate_hz(const SourceState& state) const noexcept {
    if (state.recent_timestamp_count < 2U) {
      return 0.0;
    }

    const auto oldest_index =
        state.recent_timestamp_count < kRecentTimestampCapacity
            ? 0U
            : state.next_timestamp_index;
    const auto newest_index =
        (state.next_timestamp_index + kRecentTimestampCapacity - 1U) %
        kRecentTimestampCapacity;
    const auto elapsed = state.recent_timestamps[newest_index] -
                         state.recent_timestamps[oldest_index];
    if (elapsed == 0U) {
      return 0.0;
    }

    const auto intervals = state.recent_timestamp_count - 1U;
    return static_cast<double>(intervals) * kNanosecondsPerSecond /
           static_cast<double>(elapsed);
  }

  [[nodiscard]] SourceHealthStatus evaluate_status(
      const SourceState& state, std::uint64_t now_timestamp_ns) const noexcept {
    const auto inactivity =
        elapsed_or_zero(now_timestamp_ns, state.last_receive_timestamp_ns);
    if (inactivity >= duration_ns(config_.offline_after)) {
      return SourceHealthStatus::kOffline;
    }
    if (inactivity >= duration_ns(config_.degraded_after)) {
      return SourceHealthStatus::kDegraded;
    }

    if (state.issue_seen) {
      const auto recovery_elapsed =
          elapsed_or_zero(now_timestamp_ns, state.last_issue_timestamp_ns);
      if (recovery_elapsed < duration_ns(config_.recovery_after) ||
          state.healthy_packets_since_issue < config_.healthy_after_packets) {
        return SourceHealthStatus::kDegraded;
      }
    }
    if (state.packets_received < config_.healthy_after_packets) {
      return SourceHealthStatus::kUnknown;
    }
    return SourceHealthStatus::kHealthy;
  }

  [[nodiscard]] SourceHealthSnapshot make_snapshot(
      const SourceState& state, std::uint64_t now_timestamp_ns) const {
    return {
        .source = state.source,
        .status = evaluate_status(state, now_timestamp_ns),
        .packets_received = state.packets_received,
        .first_receive_timestamp_ns = state.first_receive_timestamp_ns,
        .last_receive_timestamp_ns = state.last_receive_timestamp_ns,
        .inactivity_ns = elapsed_or_zero(now_timestamp_ns,
                                         state.last_receive_timestamp_ns),
        .current_rate_hz = current_rate_hz(state),
        .latest_latency_ns = state.latest_latency_ns,
        .maximum_latency_ns = state.maximum_latency_ns,
        .timestamp_anomalies = state.timestamp_anomalies,
        .gap_observations = state.gap_observations,
        .duplicate_observations = state.duplicate_observations,
        .reordered_observations = state.reordered_observations,
        .missing_samples_inferred = state.missing_samples_inferred,
        .payloads_rejected = state.payloads_rejected,
        .packets_dropped_queue_full = state.packets_dropped_queue_full,
    };
  }

  SourceHealthMonitorConfig config_;
  SensorStreamTracker stream_tracker_;
  mutable std::mutex mutex_;
  std::vector<SourceState> sources_;
};

SourceHealthMonitor::SourceHealthMonitor(SourceHealthMonitorConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

SourceHealthMonitor::~SourceHealthMonitor() = default;

SequenceObservation SourceHealthMonitor::observe_packet(
    const ReceivedPacket& packet) {
  return impl_->observe_packet(packet);
}

void SourceHealthMonitor::observe_issue(const ReceivedPacket& packet,
                                        SourceHealthIssue issue) {
  impl_->observe_issue(packet, issue);
}

SourceHealthReport SourceHealthMonitor::report(
    std::uint64_t now_timestamp_ns) const {
  return impl_->report(now_timestamp_ns);
}

const char* to_string(SourceHealthStatus status) noexcept {
  switch (status) {
    case SourceHealthStatus::kUnknown:
      return "UNKNOWN";
    case SourceHealthStatus::kHealthy:
      return "HEALTHY";
    case SourceHealthStatus::kDegraded:
      return "DEGRADED";
    case SourceHealthStatus::kOffline:
      return "OFFLINE";
  }
  return "UNKNOWN";
}

}  // namespace driveflow::runtime
