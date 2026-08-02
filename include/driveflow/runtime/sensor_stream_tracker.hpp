#pragma once

#include "driveflow/runtime/epoll_receiver.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace driveflow::runtime {

struct SensorStreamTrackerConfig {
  std::size_t max_sources{256U};
};

// The observable identity of one ordered sensor stream during a Runtime run.
// Message type is part of the identity because one UDP endpoint may multiplex
// independently sequenced sensor types.
struct SensorSource {
  net::Ipv4Endpoint remote_endpoint;
  net::Ipv4Endpoint listener_endpoint;
  protocol::MessageType message_type{protocol::MessageType::kImu};

  bool operator==(const SensorSource&) const = default;
};

[[nodiscard]] SensorSource identify_sensor_source(
    const ReceivedPacket& packet);

enum class SequenceStatus {
  kFirst,
  kInOrder,
  kGap,
  kDuplicate,
  kReordered,
  kUntracked,
};

struct SequenceObservation {
  SequenceStatus status{SequenceStatus::kFirst};
  std::uint64_t sequence_number{};
  // Non-zero only for kGap. This is an immediate inference from a forward
  // jump, not proof that the samples will never arrive later.
  std::uint64_t missing_samples{};

  bool operator==(const SequenceObservation&) const = default;
};

struct SensorStreamMetrics {
  std::uint64_t streams_observed{};
  std::uint64_t packets_observed{};
  std::uint64_t first_observations{};
  std::uint64_t in_order_observations{};
  std::uint64_t gap_observations{};
  std::uint64_t duplicate_observations{};
  std::uint64_t reordered_observations{};
  std::uint64_t untracked_observations{};
  // Cumulative sequence numbers inferred missing when gaps were first seen.
  // Later reordered arrivals do not revise this historical counter.
  std::uint64_t missing_samples_inferred{};
};

// Classifies packet sequence numbers in the order observe() is called. Runtime
// calls it on the single I/O thread before worker dispatch so thread scheduling
// cannot be mistaken for network reordering. The tracker is intentionally not
// thread-safe; callers must provide one serial observation order.
class SensorStreamTracker {
 public:
  explicit SensorStreamTracker(
      SensorStreamTrackerConfig config = SensorStreamTrackerConfig{});

  [[nodiscard]] SequenceObservation observe(const ReceivedPacket& packet);
  [[nodiscard]] SensorStreamMetrics metrics() const noexcept;

 private:
  struct SourceState {
    SensorSource source;
    std::uint64_t highest_sequence{};
    std::uint64_t recent_sequences{};
  };

  SensorStreamTrackerConfig config_;
  std::vector<SourceState> sources_;
  SensorStreamMetrics metrics_;
};

}  // namespace driveflow::runtime
