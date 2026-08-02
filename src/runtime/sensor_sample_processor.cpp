#include "driveflow/runtime/sensor_sample_processor.hpp"

#include <stdexcept>
#include <utility>

namespace driveflow::runtime {

SensorSampleProcessor::SensorSampleProcessor(
    SensorSampleHandler sample_handler)
    : sample_handler_(std::move(sample_handler)) {
  if (!sample_handler_) {
    throw std::invalid_argument("sensor sample handler must not be empty");
  }
}

protocol::SensorPayloadError SensorSampleProcessor::process(
    const ReceivedPacket& received_packet) {
  packets_examined_.fetch_add(1U, std::memory_order_relaxed);

  auto decoded = protocol::decode_sensor_payload(
      received_packet.packet.header.message_type,
      received_packet.packet.payload);
  if (!decoded) {
    payloads_rejected_.fetch_add(1U, std::memory_order_relaxed);
    return decoded.error;
  }

  SensorSample sample{
      .sequence_number = received_packet.packet.header.sequence_number,
      .source_timestamp_ns =
          received_packet.packet.header.source_timestamp_ns,
      .receive_timestamp_ns = received_packet.receive_timestamp_ns,
      .source = received_packet.source,
      .listener = received_packet.listener,
      .payload = std::move(*decoded.payload),
  };

  samples_decoded_.fetch_add(1U, std::memory_order_relaxed);
  switch (protocol::message_type(sample.payload)) {
    case protocol::MessageType::kImu:
      imu_samples_.fetch_add(1U, std::memory_order_relaxed);
      break;
    case protocol::MessageType::kGnss:
      gnss_samples_.fetch_add(1U, std::memory_order_relaxed);
      break;
    case protocol::MessageType::kCameraMeta:
      camera_meta_samples_.fetch_add(1U, std::memory_order_relaxed);
      break;
  }

  sample_handler_(sample);
  return protocol::SensorPayloadError::kNone;
}

SensorSampleProcessorMetrics SensorSampleProcessor::metrics() const noexcept {
  return {
      .packets_examined =
          packets_examined_.load(std::memory_order_relaxed),
      .samples_decoded = samples_decoded_.load(std::memory_order_relaxed),
      .payloads_rejected =
          payloads_rejected_.load(std::memory_order_relaxed),
      .imu_samples = imu_samples_.load(std::memory_order_relaxed),
      .gnss_samples = gnss_samples_.load(std::memory_order_relaxed),
      .camera_meta_samples =
          camera_meta_samples_.load(std::memory_order_relaxed),
  };
}

}  // namespace driveflow::runtime
