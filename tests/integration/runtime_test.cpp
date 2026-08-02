#include "driveflow/net/udp_socket.hpp"
#include "driveflow/protocol/packet.hpp"
#include "driveflow/protocol/sensor_payload.hpp"
#include "driveflow/runtime/runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace driveflow::runtime {
namespace {

TEST(RuntimeIntegrationTest, DecodesSamplesOnWorkersAndDrainsAtCount) {
  RuntimeConfig config;
  config.receiver.listen_endpoints = {
      {.address = "127.0.0.1", .port = 0U},
  };
  config.packet_count = 2U;
  config.poll_timeout = std::chrono::milliseconds{50};
  config.pipeline = {
      .worker_count = 2U,
      .queue_capacity = 8U,
  };
  Runtime runtime(config);
  const auto listener = runtime.local_endpoints().front();

  const protocol::ImuSample imu{
      .linear_acceleration_mps2 = {.x = 1.0F, .y = 2.0F, .z = 3.0F},
      .angular_velocity_rps = {.x = 4.0F, .y = 5.0F, .z = 6.0F},
  };
  const protocol::GnssFix gnss{
      .latitude_deg = 1.3521,
      .longitude_deg = 103.8198,
      .altitude_m = 15.0,
      .horizontal_accuracy_m = 0.8F,
      .vertical_accuracy_m = 1.2F,
  };
  const auto imu_payload =
      protocol::encode_sensor_payload(protocol::SensorPayload{imu});
  const auto gnss_payload =
      protocol::encode_sensor_payload(protocol::SensorPayload{gnss});

  auto sender = net::UdpSocket::open();
  sender.send_to(
      protocol::encode_packet(protocol::MessageType::kImu, 41U, 401U,
                              imu_payload),
      listener);
  sender.send_to(
      protocol::encode_packet(protocol::MessageType::kGnss, 42U, 402U,
                              gnss_payload),
      listener);

  std::vector<std::uint64_t> delivered_sequences;
  std::vector<protocol::MessageType> delivered_types;
  std::vector<std::thread::id> handler_threads;
  std::mutex delivered_mutex;
  const auto runtime_thread = std::this_thread::get_id();
  const auto summary = runtime.run(
      [] { return false; },
      [&](const SensorSample& sample) {
        const std::scoped_lock lock(delivered_mutex);
        delivered_sequences.push_back(sample.sequence_number);
        delivered_types.push_back(protocol::message_type(sample.payload));
        handler_threads.push_back(std::this_thread::get_id());
      });

  std::ranges::sort(delivered_sequences);
  std::ranges::sort(delivered_types);
  EXPECT_EQ(delivered_sequences, (std::vector<std::uint64_t>{41U, 42U}));
  EXPECT_EQ(delivered_types,
            (std::vector<protocol::MessageType>{protocol::MessageType::kImu,
                                                protocol::MessageType::kGnss}));
  ASSERT_EQ(handler_threads.size(), 2U);
  EXPECT_NE(handler_threads[0], runtime_thread);
  EXPECT_NE(handler_threads[1], runtime_thread);
  EXPECT_EQ(summary.packets_received, 2U);
  EXPECT_EQ(summary.stream_metrics.streams_observed, 2U);
  EXPECT_EQ(summary.stream_metrics.packets_observed, 2U);
  EXPECT_EQ(summary.stream_metrics.first_observations, 2U);
  EXPECT_EQ(summary.stream_metrics.in_order_observations, 0U);
  EXPECT_EQ(summary.pipeline_metrics.packets_submitted, 2U);
  EXPECT_EQ(summary.pipeline_metrics.packets_processed, 2U);
  EXPECT_EQ(summary.pipeline_metrics.packets_dropped_queue_full, 0U);
  EXPECT_EQ(summary.pipeline_metrics.handler_failures, 0U);
  EXPECT_EQ(summary.sample_metrics.packets_examined, 2U);
  EXPECT_EQ(summary.sample_metrics.samples_decoded, 2U);
  EXPECT_EQ(summary.sample_metrics.payloads_rejected, 0U);
  EXPECT_EQ(summary.sample_metrics.imu_samples, 1U);
  EXPECT_EQ(summary.sample_metrics.gnss_samples, 1U);
  EXPECT_EQ(summary.sample_metrics.camera_meta_samples, 0U);
  EXPECT_EQ(summary.receiver_metrics.datagrams_received, 2U);
  EXPECT_EQ(summary.receiver_metrics.packets_accepted, 2U);
  EXPECT_EQ(summary.receiver_metrics.packets_rejected, 0U);
}

TEST(RuntimeIntegrationTest, TracksSequenceOrderBeforeParallelDispatch) {
  RuntimeConfig config;
  config.receiver.listen_endpoints = {
      {.address = "127.0.0.1", .port = 0U},
  };
  config.packet_count = 6U;
  config.poll_timeout = std::chrono::milliseconds{50};
  config.pipeline = {
      .worker_count = 4U,
      .queue_capacity = 8U,
  };
  Runtime runtime(config);
  const auto listener = runtime.local_endpoints().front();

  const auto payload =
      protocol::encode_sensor_payload(protocol::SensorPayload{protocol::ImuSample{}});
  constexpr std::array<std::uint64_t, 6> kSequences{
      10U,
      11U,
      14U,
      13U,
      13U,
      14U,
  };
  auto sender = net::UdpSocket::open();
  for (const auto sequence : kSequences) {
    sender.send_to(
        protocol::encode_packet(protocol::MessageType::kImu, sequence,
                                sequence * 10U, payload),
        listener);
  }

  std::atomic_size_t handler_calls{};
  const auto summary = runtime.run(
      [] { return false; },
      [&](const SensorSample&) {
        handler_calls.fetch_add(1U, std::memory_order_relaxed);
      });

  EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), kSequences.size());
  EXPECT_EQ(summary.packets_received, kSequences.size());
  EXPECT_EQ(summary.stream_metrics.streams_observed, 1U);
  EXPECT_EQ(summary.stream_metrics.packets_observed, kSequences.size());
  EXPECT_EQ(summary.stream_metrics.first_observations, 1U);
  EXPECT_EQ(summary.stream_metrics.in_order_observations, 1U);
  EXPECT_EQ(summary.stream_metrics.gap_observations, 1U);
  EXPECT_EQ(summary.stream_metrics.duplicate_observations, 2U);
  EXPECT_EQ(summary.stream_metrics.reordered_observations, 1U);
  EXPECT_EQ(summary.stream_metrics.missing_samples_inferred, 2U);
  EXPECT_EQ(summary.pipeline_metrics.packets_submitted, kSequences.size());
  EXPECT_EQ(summary.pipeline_metrics.packets_processed, kSequences.size());
  EXPECT_EQ(summary.pipeline_metrics.handler_failures, 0U);
  EXPECT_EQ(summary.sample_metrics.packets_examined, kSequences.size());
  EXPECT_EQ(summary.sample_metrics.samples_decoded, kSequences.size());
  EXPECT_EQ(summary.sample_metrics.payloads_rejected, 0U);
  EXPECT_EQ(summary.sample_metrics.imu_samples, kSequences.size());
}

TEST(RuntimeIntegrationTest, ContinuesProcessingAfterSourceCapacityIsReached) {
  RuntimeConfig config;
  config.receiver.listen_endpoints = {
      {.address = "127.0.0.1", .port = 0U},
  };
  config.max_sensor_sources = 1U;
  config.packet_count = 2U;
  config.poll_timeout = std::chrono::milliseconds{50};
  config.pipeline = {
      .worker_count = 2U,
      .queue_capacity = 4U,
  };
  Runtime runtime(config);
  const auto listener = runtime.local_endpoints().front();

  const auto payload =
      protocol::encode_sensor_payload(protocol::SensorPayload{protocol::ImuSample{}});
  auto first_sender = net::UdpSocket::open();
  auto second_sender = net::UdpSocket::open();
  first_sender.send_to(
      protocol::encode_packet(protocol::MessageType::kImu, 1U, 10U, payload),
      listener);
  second_sender.send_to(
      protocol::encode_packet(protocol::MessageType::kImu, 1U, 20U, payload),
      listener);

  std::atomic_size_t handler_calls{};
  const auto summary = runtime.run(
      [] { return false; },
      [&](const SensorSample&) {
        handler_calls.fetch_add(1U, std::memory_order_relaxed);
      });

  EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), 2U);
  EXPECT_EQ(summary.packets_received, 2U);
  EXPECT_EQ(summary.stream_metrics.streams_observed, 1U);
  EXPECT_EQ(summary.stream_metrics.packets_observed, 2U);
  EXPECT_EQ(summary.stream_metrics.first_observations, 1U);
  EXPECT_EQ(summary.stream_metrics.untracked_observations, 1U);
  EXPECT_EQ(summary.pipeline_metrics.packets_submitted, 2U);
  EXPECT_EQ(summary.pipeline_metrics.packets_processed, 2U);
  EXPECT_EQ(summary.sample_metrics.samples_decoded, 2U);
}

TEST(RuntimeIntegrationTest, RejectsInvalidPayloadAfterPacketAcceptance) {
  RuntimeConfig config;
  config.receiver.listen_endpoints = {
      {.address = "127.0.0.1", .port = 0U},
  };
  config.packet_count = 1U;
  config.poll_timeout = std::chrono::milliseconds{50};
  config.pipeline = {
      .worker_count = 1U,
      .queue_capacity = 4U,
  };
  Runtime runtime(config);
  const auto listener = runtime.local_endpoints().front();

  constexpr std::array<std::uint8_t, 1> invalid_imu_payload{0x5aU};
  auto sender = net::UdpSocket::open();
  sender.send_to(
      protocol::encode_packet(protocol::MessageType::kImu, 5U, 50U,
                              invalid_imu_payload),
      listener);

  std::atomic_bool handler_called{};
  const auto summary = runtime.run(
      [] { return false; },
      [&](const SensorSample&) {
        handler_called.store(true, std::memory_order_relaxed);
      });

  EXPECT_FALSE(handler_called.load(std::memory_order_relaxed));
  EXPECT_EQ(summary.packets_received, 1U);
  EXPECT_EQ(summary.receiver_metrics.packets_accepted, 1U);
  EXPECT_EQ(summary.receiver_metrics.packets_rejected, 0U);
  EXPECT_EQ(summary.stream_metrics.streams_observed, 1U);
  EXPECT_EQ(summary.stream_metrics.packets_observed, 1U);
  EXPECT_EQ(summary.stream_metrics.first_observations, 1U);
  EXPECT_EQ(summary.pipeline_metrics.packets_processed, 1U);
  EXPECT_EQ(summary.pipeline_metrics.handler_failures, 0U);
  EXPECT_EQ(summary.sample_metrics.packets_examined, 1U);
  EXPECT_EQ(summary.sample_metrics.samples_decoded, 0U);
  EXPECT_EQ(summary.sample_metrics.payloads_rejected, 1U);
}

TEST(RuntimeIntegrationTest, IsolatesSampleHandlerFailureInWorkerPipeline) {
  RuntimeConfig config;
  config.receiver.listen_endpoints = {
      {.address = "127.0.0.1", .port = 0U},
  };
  config.packet_count = 1U;
  config.poll_timeout = std::chrono::milliseconds{50};
  config.pipeline = {
      .worker_count = 1U,
      .queue_capacity = 4U,
  };
  Runtime runtime(config);
  const auto listener = runtime.local_endpoints().front();

  const auto payload =
      protocol::encode_sensor_payload(protocol::SensorPayload{protocol::ImuSample{}});
  auto sender = net::UdpSocket::open();
  sender.send_to(
      protocol::encode_packet(protocol::MessageType::kImu, 8U, 80U, payload),
      listener);

  const auto summary = runtime.run(
      [] { return false; },
      [](const SensorSample&) { throw std::runtime_error("handler failed"); });

  EXPECT_EQ(summary.stream_metrics.streams_observed, 1U);
  EXPECT_EQ(summary.stream_metrics.packets_observed, 1U);
  EXPECT_EQ(summary.stream_metrics.first_observations, 1U);
  EXPECT_EQ(summary.pipeline_metrics.packets_submitted, 1U);
  EXPECT_EQ(summary.pipeline_metrics.packets_processed, 0U);
  EXPECT_EQ(summary.pipeline_metrics.handler_failures, 1U);
  EXPECT_EQ(summary.sample_metrics.packets_examined, 1U);
  EXPECT_EQ(summary.sample_metrics.samples_decoded, 1U);
  EXPECT_EQ(summary.sample_metrics.payloads_rejected, 0U);
  EXPECT_EQ(summary.sample_metrics.imu_samples, 1U);
}

}  // namespace
}  // namespace driveflow::runtime
