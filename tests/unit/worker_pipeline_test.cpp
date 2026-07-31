#include "driveflow/runtime/worker_pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace driveflow::runtime {
namespace {

[[nodiscard]] ReceivedPacket make_received_packet(std::uint64_t sequence_number) {
  return {
      .packet = {.header = {.sequence_number = sequence_number}, .payload = {}},
      .source = {.address = "127.0.0.1", .port = 40'000U},
      .listener = {.address = "127.0.0.1", .port = 9'000U},
      .receive_timestamp_ns = 123U,
  };
}

TEST(WorkerPipelineTest, ProcessesEveryAcceptedPacketBeforeStopReturns) {
  std::mutex processed_mutex;
  std::vector<std::uint64_t> processed_sequences;
  WorkerPipeline pipeline(
      {.worker_count = 2U, .queue_capacity = 8U},
      [&](const ReceivedPacket& packet) {
        const std::scoped_lock lock(processed_mutex);
        processed_sequences.push_back(packet.packet.header.sequence_number);
      });

  EXPECT_EQ(pipeline.try_submit(make_received_packet(11U)),
            SubmitResult::kAccepted);
  EXPECT_EQ(pipeline.try_submit(make_received_packet(22U)),
            SubmitResult::kAccepted);
  EXPECT_EQ(pipeline.try_submit(make_received_packet(33U)),
            SubmitResult::kAccepted);

  const auto metrics = pipeline.stop();

  std::ranges::sort(processed_sequences);
  EXPECT_EQ(processed_sequences,
            (std::vector<std::uint64_t>{11U, 22U, 33U}));
  EXPECT_EQ(metrics.packets_submitted, 3U);
  EXPECT_EQ(metrics.packets_processed, 3U);
  EXPECT_EQ(metrics.packets_dropped_queue_full, 0U);
  EXPECT_EQ(metrics.handler_failures, 0U);
}

TEST(WorkerPipelineTest, RejectsNewestPacketWhenBoundedQueueIsFull) {
  std::atomic_bool first_handler_call{true};
  std::latch first_packet_started{1};
  std::latch release_first_packet{1};
  WorkerPipeline pipeline(
      {.worker_count = 1U, .queue_capacity = 1U},
      [&](const ReceivedPacket&) {
        if (first_handler_call.exchange(false)) {
          first_packet_started.count_down();
          release_first_packet.wait();
        }
      });

  EXPECT_EQ(pipeline.try_submit(make_received_packet(1U)),
            SubmitResult::kAccepted);
  first_packet_started.wait();
  EXPECT_EQ(pipeline.try_submit(make_received_packet(2U)),
            SubmitResult::kAccepted);
  EXPECT_EQ(pipeline.try_submit(make_received_packet(3U)),
            SubmitResult::kQueueFull);

  release_first_packet.count_down();
  const auto metrics = pipeline.stop();

  EXPECT_EQ(metrics.packets_submitted, 2U);
  EXPECT_EQ(metrics.packets_processed, 2U);
  EXPECT_EQ(metrics.packets_dropped_queue_full, 1U);
  EXPECT_EQ(metrics.queue_high_watermark, 1U);
}

TEST(WorkerPipelineTest, StopDrainsAcceptedWorkAndRejectsLaterSubmissions) {
  std::atomic_uint64_t processed_count{};
  WorkerPipeline pipeline(
      {.worker_count = 1U, .queue_capacity = 4U},
      [&](const ReceivedPacket&) { ++processed_count; });

  EXPECT_EQ(pipeline.try_submit(make_received_packet(1U)),
            SubmitResult::kAccepted);
  const auto stopped_metrics = pipeline.stop();

  EXPECT_EQ(processed_count, 1U);
  EXPECT_EQ(stopped_metrics.packets_processed, 1U);
  EXPECT_EQ(pipeline.try_submit(make_received_packet(2U)),
            SubmitResult::kStopped);

  const auto final_metrics = pipeline.stop();
  EXPECT_EQ(final_metrics.packets_submitted, 1U);
  EXPECT_EQ(final_metrics.packets_rejected_stopped, 1U);
}

TEST(WorkerPipelineTest, HandlerFailureDoesNotStopLaterPackets) {
  std::atomic_uint64_t successful_count{};
  WorkerPipeline pipeline(
      {.worker_count = 1U, .queue_capacity = 4U},
      [&](const ReceivedPacket& packet) {
        if (packet.packet.header.sequence_number == 10U) {
          throw std::runtime_error("expected test failure");
        }
        ++successful_count;
      });

  EXPECT_EQ(pipeline.try_submit(make_received_packet(10U)),
            SubmitResult::kAccepted);
  EXPECT_EQ(pipeline.try_submit(make_received_packet(20U)),
            SubmitResult::kAccepted);

  const auto metrics = pipeline.stop();
  EXPECT_EQ(successful_count, 1U);
  EXPECT_EQ(metrics.packets_submitted, 2U);
  EXPECT_EQ(metrics.packets_processed, 1U);
  EXPECT_EQ(metrics.handler_failures, 1U);
}

TEST(WorkerPipelineTest, RejectsInvalidConfigurationAndEmptyProcessor) {
  const PacketProcessor processor = [](const ReceivedPacket&) {};
  const WorkerPipelineConfig no_workers{
      .worker_count = 0U,
      .queue_capacity = 1U,
  };
  const WorkerPipelineConfig no_queue_space{
      .worker_count = 1U,
      .queue_capacity = 0U,
  };
  const WorkerPipelineConfig valid_config{
      .worker_count = 1U,
      .queue_capacity = 1U,
  };

  EXPECT_THROW((void)WorkerPipeline(no_workers, processor),
               std::invalid_argument);
  EXPECT_THROW((void)WorkerPipeline(no_queue_space, processor),
               std::invalid_argument);
  EXPECT_THROW((void)WorkerPipeline(valid_config, PacketProcessor{}),
               std::invalid_argument);
}

}  // namespace
}  // namespace driveflow::runtime
