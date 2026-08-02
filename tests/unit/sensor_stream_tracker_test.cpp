#include "driveflow/runtime/sensor_stream_tracker.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace driveflow::runtime {
namespace {

[[nodiscard]] ReceivedPacket make_packet(
    std::uint64_t sequence_number,
    protocol::MessageType message_type = protocol::MessageType::kImu,
    std::uint16_t source_port = 31'001U,
    std::uint16_t listener_port = 9'001U) {
  return {
      .packet =
          {
              .header =
                  {
                      .message_type = message_type,
                      .sequence_number = sequence_number,
                  },
              .payload = {},
          },
      .source = {.address = "127.0.0.1", .port = source_port},
      .listener = {.address = "127.0.0.1", .port = listener_port},
  };
}

TEST(SensorStreamTrackerTest, IdentifiesSourceFromBothEndpointsAndMessageType) {
  const auto source = identify_sensor_source(
      make_packet(9U, protocol::MessageType::kGnss, 31'123U, 9'123U));

  EXPECT_EQ(source,
            (SensorSource{
                .remote_endpoint =
                    {.address = "127.0.0.1", .port = 31'123U},
                .listener_endpoint =
                    {.address = "127.0.0.1", .port = 9'123U},
                .message_type = protocol::MessageType::kGnss,
            }));
}

TEST(SensorStreamTrackerTest, TracksEachSourceIndependently) {
  SensorStreamTracker tracker;

  EXPECT_EQ(tracker.observe(make_packet(100U)).status,
            SequenceStatus::kFirst);
  EXPECT_EQ(tracker.observe(make_packet(1U, protocol::MessageType::kGnss)).status,
            SequenceStatus::kFirst);
  EXPECT_EQ(tracker.observe(make_packet(1U, protocol::MessageType::kImu,
                                       31'002U)).status,
            SequenceStatus::kFirst);
  EXPECT_EQ(tracker.observe(make_packet(1U, protocol::MessageType::kImu,
                                       31'001U, 9'002U)).status,
            SequenceStatus::kFirst);
  EXPECT_EQ(tracker.observe(make_packet(101U)).status,
            SequenceStatus::kInOrder);

  const auto metrics = tracker.metrics();
  EXPECT_EQ(metrics.streams_observed, 4U);
  EXPECT_EQ(metrics.packets_observed, 5U);
  EXPECT_EQ(metrics.first_observations, 4U);
  EXPECT_EQ(metrics.in_order_observations, 1U);
}

TEST(SensorStreamTrackerTest, ClassifiesGapReorderedAndRecentDuplicate) {
  SensorStreamTracker tracker;

  EXPECT_EQ(tracker.observe(make_packet(10U)),
            (SequenceObservation{
                .status = SequenceStatus::kFirst,
                .sequence_number = 10U,
            }));
  EXPECT_EQ(tracker.observe(make_packet(11U)).status,
            SequenceStatus::kInOrder);
  EXPECT_EQ(tracker.observe(make_packet(14U)),
            (SequenceObservation{
                .status = SequenceStatus::kGap,
                .sequence_number = 14U,
                .missing_samples = 2U,
            }));
  EXPECT_EQ(tracker.observe(make_packet(13U)).status,
            SequenceStatus::kReordered);
  EXPECT_EQ(tracker.observe(make_packet(13U)).status,
            SequenceStatus::kDuplicate);
  EXPECT_EQ(tracker.observe(make_packet(14U)).status,
            SequenceStatus::kDuplicate);
  EXPECT_EQ(tracker.observe(make_packet(15U)).status,
            SequenceStatus::kInOrder);

  const auto metrics = tracker.metrics();
  EXPECT_EQ(metrics.streams_observed, 1U);
  EXPECT_EQ(metrics.packets_observed, 7U);
  EXPECT_EQ(metrics.first_observations, 1U);
  EXPECT_EQ(metrics.in_order_observations, 2U);
  EXPECT_EQ(metrics.gap_observations, 1U);
  EXPECT_EQ(metrics.duplicate_observations, 2U);
  EXPECT_EQ(metrics.reordered_observations, 1U);
  EXPECT_EQ(metrics.missing_samples_inferred, 2U);
}

TEST(SensorStreamTrackerTest, TreatsUint64WrapAsForwardProgress) {
  constexpr auto kMaximum = std::numeric_limits<std::uint64_t>::max();
  SensorStreamTracker tracker;

  EXPECT_EQ(tracker.observe(make_packet(kMaximum - 1U)).status,
            SequenceStatus::kFirst);
  EXPECT_EQ(tracker.observe(make_packet(kMaximum)).status,
            SequenceStatus::kInOrder);
  EXPECT_EQ(tracker.observe(make_packet(0U)).status,
            SequenceStatus::kInOrder);
  EXPECT_EQ(tracker.observe(make_packet(2U)).missing_samples, 1U);
  EXPECT_EQ(tracker.observe(make_packet(1U)).status,
            SequenceStatus::kReordered);
  EXPECT_EQ(tracker.observe(make_packet(1U)).status,
            SequenceStatus::kDuplicate);

  const auto metrics = tracker.metrics();
  EXPECT_EQ(metrics.packets_observed, 6U);
  EXPECT_EQ(metrics.first_observations, 1U);
  EXPECT_EQ(metrics.in_order_observations, 2U);
  EXPECT_EQ(metrics.gap_observations, 1U);
  EXPECT_EQ(metrics.duplicate_observations, 1U);
  EXPECT_EQ(metrics.reordered_observations, 1U);
  EXPECT_EQ(metrics.missing_samples_inferred, 1U);
}

TEST(SensorStreamTrackerTest, TreatsHalfRangeJumpAsAmbiguousReordering) {
  constexpr auto kHalfRange = std::uint64_t{1U} << 63U;
  SensorStreamTracker tracker;

  EXPECT_EQ(tracker.observe(make_packet(0U)).status,
            SequenceStatus::kFirst);
  EXPECT_EQ(tracker.observe(make_packet(kHalfRange)).status,
            SequenceStatus::kReordered);

  const auto metrics = tracker.metrics();
  EXPECT_EQ(metrics.gap_observations, 0U);
  EXPECT_EQ(metrics.reordered_observations, 1U);
}

TEST(SensorStreamTrackerTest, BoundsDuplicateHistoryToRecent64Numbers) {
  SensorStreamTracker tracker;

  EXPECT_EQ(tracker.observe(make_packet(100U)).status,
            SequenceStatus::kFirst);
  EXPECT_EQ(tracker.observe(make_packet(200U)).status,
            SequenceStatus::kGap);
  EXPECT_EQ(tracker.observe(make_packet(100U)).status,
            SequenceStatus::kReordered);
  EXPECT_EQ(tracker.observe(make_packet(100U)).status,
            SequenceStatus::kReordered);

  const auto metrics = tracker.metrics();
  EXPECT_EQ(metrics.duplicate_observations, 0U);
  EXPECT_EQ(metrics.reordered_observations, 2U);
  EXPECT_EQ(metrics.missing_samples_inferred, 99U);
}

TEST(SensorStreamTrackerTest, SaturatesInferredMissingMetric) {
  constexpr auto kForwardJump = (std::uint64_t{1U} << 63U) - 1U;
  SensorStreamTracker tracker;

  auto sequence = std::uint64_t{0U};
  (void)tracker.observe(make_packet(sequence));
  for (std::size_t jump = 0U; jump < 3U; ++jump) {
    sequence += kForwardJump;
    EXPECT_EQ(tracker.observe(make_packet(sequence)).status,
              SequenceStatus::kGap);
  }

  EXPECT_EQ(tracker.metrics().missing_samples_inferred,
            std::numeric_limits<std::uint64_t>::max());
}

TEST(SensorStreamTrackerTest, StopsAdmittingSourcesAtConfiguredCapacity) {
  SensorStreamTracker tracker({.max_sources = 1U});

  EXPECT_EQ(tracker.observe(make_packet(1U, protocol::MessageType::kImu,
                                       31'001U)).status,
            SequenceStatus::kFirst);
  EXPECT_EQ(tracker.observe(make_packet(1U, protocol::MessageType::kImu,
                                       31'002U)).status,
            SequenceStatus::kUntracked);
  EXPECT_EQ(tracker.observe(make_packet(2U, protocol::MessageType::kImu,
                                       31'001U)).status,
            SequenceStatus::kInOrder);

  const auto metrics = tracker.metrics();
  EXPECT_EQ(metrics.streams_observed, 1U);
  EXPECT_EQ(metrics.packets_observed, 3U);
  EXPECT_EQ(metrics.first_observations, 1U);
  EXPECT_EQ(metrics.in_order_observations, 1U);
  EXPECT_EQ(metrics.untracked_observations, 1U);
}

TEST(SensorStreamTrackerTest, RejectsZeroSourceCapacity) {
  EXPECT_THROW(
      (void)SensorStreamTracker(
          SensorStreamTrackerConfig{.max_sources = 0U}),
      std::invalid_argument);
}

}  // namespace
}  // namespace driveflow::runtime
