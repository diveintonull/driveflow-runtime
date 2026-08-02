#include "driveflow/runtime/sensor_stream_tracker.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace driveflow::runtime {
namespace {

constexpr std::uint64_t kSequenceHalfRange = std::uint64_t{1U} << 63U;
constexpr std::uint64_t kRecentSequenceCount =
    std::numeric_limits<std::uint64_t>::digits;

void saturating_add(std::uint64_t& value, std::uint64_t increment) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (increment > maximum - value) {
    value = maximum;
    return;
  }
  value += increment;
}

}  // namespace

SensorStreamTracker::SensorStreamTracker(SensorStreamTrackerConfig config)
    : config_(config) {
  if (config_.max_sources == 0U) {
    throw std::invalid_argument(
        "sensor stream tracker source capacity must be greater than zero");
  }
}

SensorSource identify_sensor_source(const ReceivedPacket& packet) {
  return {
      .remote_endpoint = packet.source,
      .listener_endpoint = packet.listener,
      .message_type = packet.packet.header.message_type,
  };
}

SequenceObservation SensorStreamTracker::observe(
    const ReceivedPacket& packet) {
  ++metrics_.packets_observed;
  const auto source = identify_sensor_source(packet);
  const auto sequence_number = packet.packet.header.sequence_number;
  auto state = std::find_if(
      sources_.begin(), sources_.end(),
      [&](const SourceState& candidate) { return candidate.source == source; });

  if (state == sources_.end()) {
    if (sources_.size() >= config_.max_sources) {
      ++metrics_.untracked_observations;
      return {
          .status = SequenceStatus::kUntracked,
          .sequence_number = sequence_number,
      };
    }

    sources_.push_back({
        .source = source,
        .highest_sequence = sequence_number,
        .recent_sequences = 1U,
    });
    ++metrics_.streams_observed;
    ++metrics_.first_observations;
    return {
        .status = SequenceStatus::kFirst,
        .sequence_number = sequence_number,
    };
  }

  const auto forward_distance = sequence_number - state->highest_sequence;
  if (forward_distance == 0U) {
    ++metrics_.duplicate_observations;
    return {
        .status = SequenceStatus::kDuplicate,
        .sequence_number = sequence_number,
    };
  }

  if (forward_distance < kSequenceHalfRange) {
    if (forward_distance >= kRecentSequenceCount) {
      state->recent_sequences = 1U;
    } else {
      state->recent_sequences <<= static_cast<unsigned int>(forward_distance);
      state->recent_sequences |= 1U;
    }
    state->highest_sequence = sequence_number;

    const auto missing_samples = forward_distance - 1U;
    if (missing_samples == 0U) {
      ++metrics_.in_order_observations;
      return {
          .status = SequenceStatus::kInOrder,
          .sequence_number = sequence_number,
      };
    }

    ++metrics_.gap_observations;
    saturating_add(metrics_.missing_samples_inferred, missing_samples);
    return {
        .status = SequenceStatus::kGap,
        .sequence_number = sequence_number,
        .missing_samples = missing_samples,
    };
  }

  const auto backward_distance = state->highest_sequence - sequence_number;
  if (backward_distance < kRecentSequenceCount) {
    const auto mask = std::uint64_t{1U}
                      << static_cast<unsigned int>(backward_distance);
    if ((state->recent_sequences & mask) != 0U) {
      ++metrics_.duplicate_observations;
      return {
          .status = SequenceStatus::kDuplicate,
          .sequence_number = sequence_number,
      };
    }
    state->recent_sequences |= mask;
  }

  ++metrics_.reordered_observations;
  return {
      .status = SequenceStatus::kReordered,
      .sequence_number = sequence_number,
  };
}

SensorStreamMetrics SensorStreamTracker::metrics() const noexcept {
  return metrics_;
}

}  // namespace driveflow::runtime
