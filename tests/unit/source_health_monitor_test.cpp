#include "driveflow/runtime/source_health_monitor.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace driveflow::runtime {
namespace {

constexpr std::uint64_t kMillisecond = 1'000'000U;

[[nodiscard]] ReceivedPacket make_packet(
    std::uint64_t sequence_number, std::uint64_t receive_timestamp_ns,
    std::uint64_t source_timestamp_ns = 0U,
    protocol::MessageType message_type = protocol::MessageType::kImu,
    std::uint16_t source_port = 31'001U) {
  return {
      .packet =
          {
              .header =
                  {
                      .message_type = message_type,
                      .sequence_number = sequence_number,
                      .source_timestamp_ns = source_timestamp_ns,
                  },
              .payload = {},
          },
      .source = {.address = "127.0.0.1", .port = source_port},
      .listener = {.address = "127.0.0.1", .port = 9'001U},
      .receive_timestamp_ns = receive_timestamp_ns,
  };
}

[[nodiscard]] SourceHealthMonitorConfig test_config() {
  return {
      .max_sources = 8U,
      .healthy_after_packets = 2U,
      .degraded_after = std::chrono::milliseconds{500},
      .offline_after = std::chrono::seconds{2},
      .recovery_after = std::chrono::milliseconds{300},
  };
}

TEST(SourceHealthMonitorTest, BecomesHealthyAndReportsRecentRateAndLatency) {
  SourceHealthMonitor monitor(test_config());
  const auto first = make_packet(10U, 1'000U * kMillisecond,
                                 990U * kMillisecond);
  const auto second = make_packet(11U, 1'100U * kMillisecond,
                                  1'085U * kMillisecond);

  EXPECT_EQ(monitor.observe_packet(first).status, SequenceStatus::kFirst);
  auto report = monitor.report(1'000U * kMillisecond);
  ASSERT_EQ(report.sources.size(), 1U);
  EXPECT_EQ(report.sources.front().status, SourceHealthStatus::kUnknown);

  EXPECT_EQ(monitor.observe_packet(second).status, SequenceStatus::kInOrder);
  report = monitor.report(1'100U * kMillisecond);
  ASSERT_EQ(report.sources.size(), 1U);
  const auto& source = report.sources.front();
  EXPECT_EQ(source.status, SourceHealthStatus::kHealthy);
  EXPECT_EQ(source.packets_received, 2U);
  EXPECT_DOUBLE_EQ(source.current_rate_hz, 10.0);
  EXPECT_EQ(source.latest_latency_ns, 15U * kMillisecond);
  EXPECT_EQ(source.maximum_latency_ns, 15U * kMillisecond);
  EXPECT_EQ(source.inactivity_ns, 0U);
  EXPECT_EQ(monitor.report(1'050U * kMillisecond)
                .sources.front()
                .inactivity_ns,
            0U);
}

TEST(SourceHealthMonitorTest, CalculatesRateFromMostRecent64Arrivals) {
  SourceHealthMonitor monitor(test_config());
  (void)monitor.observe_packet(make_packet(1U, 0U));
  (void)monitor.observe_packet(make_packet(2U, 1'000U * kMillisecond));
  for (std::uint64_t sequence = 3U; sequence <= 66U; ++sequence) {
    const auto receive_timestamp =
        (1'000U + (sequence - 2U) * 10U) * kMillisecond;
    (void)monitor.observe_packet(make_packet(sequence, receive_timestamp));
  }

  const auto snapshot =
      monitor.report(1'640U * kMillisecond).sources.front();
  EXPECT_EQ(snapshot.packets_received, 66U);
  EXPECT_NEAR(snapshot.current_rate_hz, 100.0, 0.001);
}

TEST(SourceHealthMonitorTest, DegradesOnSequenceIssuesThenRecovers) {
  SourceHealthMonitor monitor(test_config());
  (void)monitor.observe_packet(make_packet(1U, 0U));
  (void)monitor.observe_packet(make_packet(2U, 100U * kMillisecond));
  EXPECT_EQ(monitor.observe_packet(make_packet(5U, 200U * kMillisecond)).status,
            SequenceStatus::kGap);
  EXPECT_EQ(monitor.observe_packet(make_packet(4U, 250U * kMillisecond)).status,
            SequenceStatus::kReordered);
  EXPECT_EQ(monitor.observe_packet(make_packet(4U, 260U * kMillisecond)).status,
            SequenceStatus::kDuplicate);

  auto report = monitor.report(260U * kMillisecond);
  ASSERT_EQ(report.sources.size(), 1U);
  EXPECT_EQ(report.sources.front().status, SourceHealthStatus::kDegraded);
  EXPECT_EQ(report.sources.front().gap_observations, 1U);
  EXPECT_EQ(report.sources.front().reordered_observations, 1U);
  EXPECT_EQ(report.sources.front().duplicate_observations, 1U);
  EXPECT_EQ(report.sources.front().missing_samples_inferred, 2U);

  (void)monitor.observe_packet(make_packet(6U, 300U * kMillisecond));
  (void)monitor.observe_packet(make_packet(7U, 600U * kMillisecond));
  report = monitor.report(600U * kMillisecond);
  EXPECT_EQ(report.sources.front().status, SourceHealthStatus::kHealthy);
}

TEST(SourceHealthMonitorTest, UsesSilenceForDegradedAndOfflineStates) {
  SourceHealthMonitor monitor(test_config());
  (void)monitor.observe_packet(make_packet(1U, 100U * kMillisecond));
  (void)monitor.observe_packet(make_packet(2U, 200U * kMillisecond));

  EXPECT_EQ(monitor.report(699U * kMillisecond).sources.front().status,
            SourceHealthStatus::kHealthy);
  EXPECT_EQ(monitor.report(700U * kMillisecond).sources.front().status,
            SourceHealthStatus::kDegraded);
  const auto offline = monitor.report(2'200U * kMillisecond).sources.front();
  EXPECT_EQ(offline.status, SourceHealthStatus::kOffline);
  EXPECT_EQ(offline.inactivity_ns, 2'000U * kMillisecond);

  (void)monitor.observe_packet(make_packet(3U, 2'300U * kMillisecond));
  EXPECT_EQ(monitor.report(2'300U * kMillisecond).sources.front().status,
            SourceHealthStatus::kHealthy);
}

TEST(SourceHealthMonitorTest, RecordsWorkerAndQueueIssuesPerSource) {
  SourceHealthMonitor monitor(test_config());
  auto packet = make_packet(1U, 100U * kMillisecond);
  (void)monitor.observe_packet(packet);
  packet = make_packet(2U, 200U * kMillisecond);
  (void)monitor.observe_packet(packet);

  monitor.observe_issue(packet, SourceHealthIssue::kPayloadRejected);
  monitor.observe_issue(packet, SourceHealthIssue::kQueueDropped);

  const auto snapshot = monitor.report(200U * kMillisecond).sources.front();
  EXPECT_EQ(snapshot.status, SourceHealthStatus::kDegraded);
  EXPECT_EQ(snapshot.payloads_rejected, 1U);
  EXPECT_EQ(snapshot.packets_dropped_queue_full, 1U);
}

TEST(SourceHealthMonitorTest, TreatsFutureSourceTimestampAsAnIssue) {
  SourceHealthMonitor monitor(test_config());
  (void)monitor.observe_packet(
      make_packet(1U, 100U * kMillisecond, 101U * kMillisecond));

  const auto snapshot = monitor.report(100U * kMillisecond).sources.front();
  EXPECT_EQ(snapshot.status, SourceHealthStatus::kDegraded);
  EXPECT_EQ(snapshot.timestamp_anomalies, 1U);
  EXPECT_EQ(snapshot.latest_latency_ns, 0U);
}

TEST(SourceHealthMonitorTest, BoundsSourcesAndPreservesAggregateStreamMetrics) {
  auto config = test_config();
  config.max_sources = 1U;
  SourceHealthMonitor monitor(config);

  EXPECT_EQ(monitor.observe_packet(make_packet(1U, 100U, 0U,
                                               protocol::MessageType::kImu,
                                               31'001U))
                .status,
            SequenceStatus::kFirst);
  EXPECT_EQ(monitor.observe_packet(make_packet(1U, 200U, 0U,
                                               protocol::MessageType::kImu,
                                               31'002U))
                .status,
            SequenceStatus::kUntracked);

  const auto report = monitor.report(200U);
  EXPECT_EQ(report.sources.size(), 1U);
  EXPECT_EQ(report.stream_metrics.streams_observed, 1U);
  EXPECT_EQ(report.stream_metrics.packets_observed, 2U);
  EXPECT_EQ(report.stream_metrics.first_observations, 1U);
  EXPECT_EQ(report.stream_metrics.untracked_observations, 1U);
}

TEST(SourceHealthMonitorTest, SortsSnapshotsByObservableSourceIdentity) {
  SourceHealthMonitor monitor(test_config());
  (void)monitor.observe_packet(make_packet(1U, 100U, 0U,
                                           protocol::MessageType::kGnss,
                                           31'002U));
  (void)monitor.observe_packet(make_packet(1U, 100U, 0U,
                                           protocol::MessageType::kImu,
                                           31'001U));

  const auto report = monitor.report(100U);
  ASSERT_EQ(report.sources.size(), 2U);
  EXPECT_EQ(report.sources[0].source.remote_endpoint.port, 31'001U);
  EXPECT_EQ(report.sources[1].source.remote_endpoint.port, 31'002U);
}

TEST(SourceHealthMonitorTest, SerializesConcurrentWorkerIssueUpdates) {
  SourceHealthMonitor monitor(test_config());
  const auto packet = make_packet(1U, 100U);
  (void)monitor.observe_packet(packet);

  constexpr std::size_t kThreadCount = 4U;
  constexpr std::size_t kIssuesPerThread = 250U;
  std::vector<std::thread> workers;
  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    workers.emplace_back([&] {
      for (std::size_t issue = 0U; issue < kIssuesPerThread; ++issue) {
        monitor.observe_issue(packet, SourceHealthIssue::kPayloadRejected);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  const auto snapshot = monitor.report(100U).sources.front();
  EXPECT_EQ(snapshot.payloads_rejected,
            kThreadCount * kIssuesPerThread);
}

TEST(SourceHealthMonitorTest, RejectsInvalidConfiguration) {
  auto config = test_config();
  config.max_sources = 0U;
  EXPECT_THROW((void)SourceHealthMonitor(config), std::invalid_argument);

  config = test_config();
  config.healthy_after_packets = 0U;
  EXPECT_THROW((void)SourceHealthMonitor(config), std::invalid_argument);

  config = test_config();
  config.degraded_after = std::chrono::nanoseconds{0};
  EXPECT_THROW((void)SourceHealthMonitor(config), std::invalid_argument);

  config = test_config();
  config.offline_after = config.degraded_after;
  EXPECT_THROW((void)SourceHealthMonitor(config), std::invalid_argument);

  config = test_config();
  config.recovery_after = std::chrono::nanoseconds{0};
  EXPECT_THROW((void)SourceHealthMonitor(config), std::invalid_argument);
}

TEST(SourceHealthMonitorTest, StringifiesEveryStatus) {
  EXPECT_STREQ(to_string(SourceHealthStatus::kUnknown), "UNKNOWN");
  EXPECT_STREQ(to_string(SourceHealthStatus::kHealthy), "HEALTHY");
  EXPECT_STREQ(to_string(SourceHealthStatus::kDegraded), "DEGRADED");
  EXPECT_STREQ(to_string(SourceHealthStatus::kOffline), "OFFLINE");
}

}  // namespace
}  // namespace driveflow::runtime
