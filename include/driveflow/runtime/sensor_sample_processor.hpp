#pragma once

#include "driveflow/net/udp_socket.hpp"
#include "driveflow/protocol/sensor_payload.hpp"
#include "driveflow/runtime/epoll_receiver.hpp"

#include <atomic>
#include <cstdint>
#include <functional>

namespace driveflow::runtime {

// A validated, typed sensor measurement plus the transport metadata observed by
// the Runtime. Packet-envelope details such as magic, CRC, and encoded payload
// length have already served their validation purpose and are not repeated.
struct SensorSample {
  std::uint64_t sequence_number{};
  std::uint64_t source_timestamp_ns{};
  std::uint64_t receive_timestamp_ns{};
  net::Ipv4Endpoint source;
  net::Ipv4Endpoint listener;
  protocol::SensorPayload payload;

  bool operator==(const SensorSample&) const = default;
};

// A handler may be invoked concurrently by multiple Runtime workers. The
// SensorSample reference remains valid only for the duration of the call.
using SensorSampleHandler = std::function<void(const SensorSample&)>;

struct SensorSampleProcessorMetrics {
  // ReceivedPacket values for which process() was called.
  std::uint64_t packets_examined{};
  // Payloads decoded into a SensorSample, whether or not the handler later
  // threw an exception.
  std::uint64_t samples_decoded{};
  // Packet envelopes that were valid but whose type-specific payload failed
  // validation.
  std::uint64_t payloads_rejected{};
  std::uint64_t imu_samples{};
  std::uint64_t gnss_samples{};
  std::uint64_t camera_meta_samples{};
};

// Converts validated packet envelopes into typed SensorSample values. process()
// and metrics() are safe to call concurrently. The supplied handler must make
// its own shared state thread-safe; its exceptions deliberately propagate so
// WorkerPipeline can count and isolate them.
class SensorSampleProcessor {
 public:
  explicit SensorSampleProcessor(SensorSampleHandler sample_handler);

  SensorSampleProcessor(const SensorSampleProcessor&) = delete;
  SensorSampleProcessor& operator=(const SensorSampleProcessor&) = delete;
  SensorSampleProcessor(SensorSampleProcessor&&) = delete;
  SensorSampleProcessor& operator=(SensorSampleProcessor&&) = delete;

  // Returns kNone when a sample was decoded and the handler returned normally.
  // Invalid type-specific payloads are counted and returned without invoking
  // the handler. A handler exception propagates instead of producing a return
  // value.
  [[nodiscard]] protocol::SensorPayloadError process(
      const ReceivedPacket& received_packet);

  // During active processing this is a monotonic snapshot. Once all process()
  // calls have finished, packets_examined equals samples_decoded plus
  // payloads_rejected, and samples_decoded equals the sum of the type counts.
  [[nodiscard]] SensorSampleProcessorMetrics metrics() const noexcept;

 private:
  SensorSampleHandler sample_handler_;
  std::atomic<std::uint64_t> packets_examined_{};
  std::atomic<std::uint64_t> samples_decoded_{};
  std::atomic<std::uint64_t> payloads_rejected_{};
  std::atomic<std::uint64_t> imu_samples_{};
  std::atomic<std::uint64_t> gnss_samples_{};
  std::atomic<std::uint64_t> camera_meta_samples_{};
};

}  // namespace driveflow::runtime
