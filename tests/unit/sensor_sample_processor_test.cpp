#include "driveflow/runtime/sensor_sample_processor.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace driveflow::runtime {
namespace {

[[nodiscard]] ReceivedPacket make_received_packet(
    protocol::SensorPayload payload, std::uint64_t sequence_number = 41U,
    std::uint64_t source_timestamp_ns = 1'000U,
    std::uint64_t receive_timestamp_ns = 1'250U) {
  const auto type = protocol::message_type(payload);
  auto bytes = protocol::encode_sensor_payload(payload);
  const auto payload_length = static_cast<std::uint32_t>(bytes.size());
  return {
      .packet =
          {
              .header =
                  {
                      .message_type = type,
                      .sequence_number = sequence_number,
                      .source_timestamp_ns = source_timestamp_ns,
                      .payload_length = payload_length,
                  },
              .payload = std::move(bytes),
          },
      .source = {.address = "127.0.0.1", .port = 31'001U},
      .listener = {.address = "127.0.0.1", .port = 9'001U},
      .receive_timestamp_ns = receive_timestamp_ns,
  };
}

TEST(SensorSampleProcessorTest, DecodesEverySensorTypeAndPreservesMetadata) {
  std::vector<SensorSample> delivered;
  SensorSampleProcessor processor(
      [&](const SensorSample& sample) { delivered.push_back(sample); });

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
  const protocol::CameraMeta camera{
      .frame_id = 91U,
      .width = 1'920U,
      .height = 1'080U,
      .exposure_time_us = 8'000U,
      .extra_data = {0xdeU, 0xadU},
  };

  EXPECT_EQ(processor.process(make_received_packet(imu, 10U, 100U, 150U)),
            protocol::SensorPayloadError::kNone);
  EXPECT_EQ(processor.process(make_received_packet(gnss, 11U, 200U, 260U)),
            protocol::SensorPayloadError::kNone);
  EXPECT_EQ(processor.process(make_received_packet(camera, 12U, 300U, 370U)),
            protocol::SensorPayloadError::kNone);

  ASSERT_EQ(delivered.size(), 3U);
  EXPECT_EQ(std::get<protocol::ImuSample>(delivered[0].payload), imu);
  EXPECT_EQ(std::get<protocol::GnssFix>(delivered[1].payload), gnss);
  EXPECT_EQ(std::get<protocol::CameraMeta>(delivered[2].payload), camera);
  EXPECT_EQ(delivered[0].sequence_number, 10U);
  EXPECT_EQ(delivered[0].source_timestamp_ns, 100U);
  EXPECT_EQ(delivered[0].receive_timestamp_ns, 150U);
  EXPECT_EQ(delivered[0].source,
            (net::Ipv4Endpoint{.address = "127.0.0.1", .port = 31'001U}));
  EXPECT_EQ(delivered[0].listener,
            (net::Ipv4Endpoint{.address = "127.0.0.1", .port = 9'001U}));

  const auto metrics = processor.metrics();
  EXPECT_EQ(metrics.packets_examined, 3U);
  EXPECT_EQ(metrics.samples_decoded, 3U);
  EXPECT_EQ(metrics.payloads_rejected, 0U);
  EXPECT_EQ(metrics.imu_samples, 1U);
  EXPECT_EQ(metrics.gnss_samples, 1U);
  EXPECT_EQ(metrics.camera_meta_samples, 1U);
}

TEST(SensorSampleProcessorTest, RejectsInvalidTypedPayloadAndContinues) {
  std::vector<SensorSample> delivered;
  SensorSampleProcessor processor(
      [&](const SensorSample& sample) { delivered.push_back(sample); });

  auto invalid = make_received_packet(protocol::ImuSample{});
  invalid.packet.payload.pop_back();
  const protocol::GnssFix valid{
      .latitude_deg = 1.0,
      .longitude_deg = 2.0,
      .altitude_m = 3.0,
      .horizontal_accuracy_m = 1.0F,
      .vertical_accuracy_m = 2.0F,
  };

  EXPECT_EQ(processor.process(invalid),
            protocol::SensorPayloadError::kSizeMismatch);
  EXPECT_EQ(processor.process(make_received_packet(valid)),
            protocol::SensorPayloadError::kNone);

  ASSERT_EQ(delivered.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<protocol::GnssFix>(delivered.front().payload));
  const auto metrics = processor.metrics();
  EXPECT_EQ(metrics.packets_examined, 2U);
  EXPECT_EQ(metrics.samples_decoded, 1U);
  EXPECT_EQ(metrics.payloads_rejected, 1U);
  EXPECT_EQ(metrics.imu_samples, 0U);
  EXPECT_EQ(metrics.gnss_samples, 1U);
  EXPECT_EQ(metrics.camera_meta_samples, 0U);
}

TEST(SensorSampleProcessorTest, MaintainsMetricsAcrossConcurrentCalls) {
  constexpr std::size_t kThreadCount = 8U;
  constexpr std::size_t kCallsPerThread = 250U;
  constexpr std::size_t kExpectedCalls = kThreadCount * kCallsPerThread;

  std::atomic<std::size_t> handler_calls{};
  std::atomic_bool processing_failed{};
  SensorSampleProcessor processor([&](const SensorSample&) {
    handler_calls.fetch_add(1U, std::memory_order_relaxed);
  });
  const auto packet = make_received_packet(protocol::ImuSample{});

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    threads.emplace_back([&] {
      for (std::size_t call = 0U; call < kCallsPerThread; ++call) {
        if (processor.process(packet) != protocol::SensorPayloadError::kNone) {
          processing_failed.store(true, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_FALSE(processing_failed.load(std::memory_order_relaxed));
  EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), kExpectedCalls);
  const auto metrics = processor.metrics();
  EXPECT_EQ(metrics.packets_examined, kExpectedCalls);
  EXPECT_EQ(metrics.samples_decoded, kExpectedCalls);
  EXPECT_EQ(metrics.payloads_rejected, 0U);
  EXPECT_EQ(metrics.imu_samples, kExpectedCalls);
  EXPECT_EQ(metrics.gnss_samples, 0U);
  EXPECT_EQ(metrics.camera_meta_samples, 0U);
}

TEST(SensorSampleProcessorTest, PropagatesHandlerFailureAfterCountingSample) {
  SensorSampleProcessor processor([](const SensorSample&) {
    throw std::runtime_error("downstream failure");
  });

  EXPECT_THROW(
      (void)processor.process(make_received_packet(protocol::ImuSample{})),
      std::runtime_error);

  const auto metrics = processor.metrics();
  EXPECT_EQ(metrics.packets_examined, 1U);
  EXPECT_EQ(metrics.samples_decoded, 1U);
  EXPECT_EQ(metrics.payloads_rejected, 0U);
  EXPECT_EQ(metrics.imu_samples, 1U);
}

TEST(SensorSampleProcessorTest, RejectsEmptyHandler) {
  EXPECT_THROW((void)SensorSampleProcessor(SensorSampleHandler{}),
               std::invalid_argument);
}

}  // namespace
}  // namespace driveflow::runtime
