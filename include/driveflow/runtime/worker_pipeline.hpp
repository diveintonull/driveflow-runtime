#pragma once

#include "driveflow/runtime/epoll_receiver.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace driveflow::runtime {

struct WorkerPipelineConfig {
  // Both values must be greater than zero. Queue capacity counts waiting
  // packets; packets currently executing on workers do not consume it.
  std::size_t worker_count{2U};
  std::size_t queue_capacity{1'024U};
};

enum class SubmitResult {
  // The packet will be processed before stop() finishes.
  kAccepted,
  // The packet was rejected immediately because every queue slot was occupied.
  kQueueFull,
  // The packet was rejected because shutdown had started.
  kStopped,
};

struct WorkerPipelineMetrics {
  // Packets for which try_submit() returned kAccepted.
  std::uint64_t packets_submitted{};
  // PacketProcessor calls that returned normally.
  std::uint64_t packets_processed{};
  // Packets for which try_submit() returned kQueueFull.
  std::uint64_t packets_dropped_queue_full{};
  // Packets for which try_submit() returned kStopped.
  std::uint64_t packets_rejected_stopped{};
  // PacketProcessor calls that threw; workers continue after these failures.
  std::uint64_t handler_failures{};
  // Largest number of packets waiting in the queue at one time.
  std::size_t queue_high_watermark{};
};

// A processor may be invoked concurrently by multiple workers and must protect
// any shared mutable state it uses. Completion order is not guaranteed.
using PacketProcessor = std::function<void(const ReceivedPacket&)>;

// Owns a fixed set of worker threads behind a non-blocking bounded submission
// interface. Construction may throw std::invalid_argument for invalid
// configuration or an empty processor.
class WorkerPipeline {
 public:
  WorkerPipeline(const WorkerPipelineConfig& config,
                 PacketProcessor packet_processor);
  // Equivalent to stop(); accepted work is drained before destruction returns.
  ~WorkerPipeline();

  WorkerPipeline(const WorkerPipeline&) = delete;
  WorkerPipeline& operator=(const WorkerPipeline&) = delete;
  WorkerPipeline(WorkerPipeline&&) = delete;
  WorkerPipeline& operator=(WorkerPipeline&&) = delete;

  // Thread-safe and non-blocking with respect to worker execution.
  // The packet is consumed only for kAccepted and remains unchanged when the
  // queue is full or shutdown has started.
  [[nodiscard]] SubmitResult try_submit(ReceivedPacket&& packet);

  // Thread-safe and idempotent. Stops accepting, drains accepted work, joins
  // all workers, and returns a stable metrics snapshot. Call this from outside
  // the PacketProcessor; a worker cannot join itself.
  [[nodiscard]] WorkerPipelineMetrics stop();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace driveflow::runtime
