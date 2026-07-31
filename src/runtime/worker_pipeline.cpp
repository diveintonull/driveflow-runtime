#include "driveflow/runtime/worker_pipeline.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace driveflow::runtime {

class WorkerPipeline::Impl {
 public:
  Impl(const WorkerPipelineConfig& config, PacketProcessor packet_processor)
      : config_(validate_config(config)),
        packet_processor_(validate_processor(std::move(packet_processor))) {
    workers_.reserve(config_.worker_count);
    try {
      for (std::size_t index = 0U; index < config_.worker_count; ++index) {
        workers_.emplace_back([this] { worker_loop(); });
      }
    } catch (...) {
      request_stop();
      join_workers();
      throw;
    }
  }

  [[nodiscard]] SubmitResult try_submit(ReceivedPacket packet) {
    {
      const std::scoped_lock lock(state_mutex_);
      if (!accepting_) {
        ++metrics_.packets_rejected_stopped;
        return SubmitResult::kStopped;
      }
      if (queue_.size() >= config_.queue_capacity) {
        ++metrics_.packets_dropped_queue_full;
        return SubmitResult::kQueueFull;
      }

      queue_.push_back(std::move(packet));
      ++metrics_.packets_submitted;
      metrics_.queue_high_watermark =
          std::max(metrics_.queue_high_watermark, queue_.size());
    }
    work_available_.notify_one();
    return SubmitResult::kAccepted;
  }

  [[nodiscard]] WorkerPipelineMetrics stop() {
    const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    request_stop();
    join_workers();

    const std::scoped_lock state_lock(state_mutex_);
    return metrics_;
  }

 private:
  [[nodiscard]] static WorkerPipelineConfig validate_config(
      const WorkerPipelineConfig& config) {
    if (config.worker_count == 0U) {
      throw std::invalid_argument("worker count must be greater than zero");
    }
    if (config.queue_capacity == 0U) {
      throw std::invalid_argument("worker queue capacity must be greater than zero");
    }
    return config;
  }

  [[nodiscard]] static PacketProcessor validate_processor(
      PacketProcessor packet_processor) {
    if (!packet_processor) {
      throw std::invalid_argument("packet processor must not be empty");
    }
    return packet_processor;
  }

  void request_stop() {
    {
      const std::scoped_lock lock(state_mutex_);
      accepting_ = false;
      stop_requested_ = true;
    }
    work_available_.notify_all();
  }

  void join_workers() {
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  void worker_loop() noexcept {
    while (true) {
      ReceivedPacket packet;
      {
        std::unique_lock lock(state_mutex_);
        work_available_.wait(
            lock, [this] { return stop_requested_ || !queue_.empty(); });
        if (queue_.empty()) {
          if (stop_requested_) {
            return;
          }
          continue;
        }
        packet = std::move(queue_.front());
        queue_.pop_front();
      }

      try {
        packet_processor_(packet);
        const std::scoped_lock lock(state_mutex_);
        ++metrics_.packets_processed;
      } catch (...) {
        const std::scoped_lock lock(state_mutex_);
        ++metrics_.handler_failures;
      }
    }
  }

  WorkerPipelineConfig config_;
  PacketProcessor packet_processor_;
  std::mutex state_mutex_;
  std::mutex lifecycle_mutex_;
  std::condition_variable work_available_;
  std::deque<ReceivedPacket> queue_;
  std::vector<std::thread> workers_;
  WorkerPipelineMetrics metrics_;
  bool accepting_{true};
  bool stop_requested_{false};
};

WorkerPipeline::WorkerPipeline(const WorkerPipelineConfig& config,
                               PacketProcessor packet_processor)
    : impl_(std::make_unique<Impl>(config, std::move(packet_processor))) {}

WorkerPipeline::~WorkerPipeline() {
  try {
    (void)impl_->stop();
  } catch (...) {
  }
}

SubmitResult WorkerPipeline::try_submit(ReceivedPacket packet) {
  return impl_->try_submit(std::move(packet));
}

WorkerPipelineMetrics WorkerPipeline::stop() { return impl_->stop(); }

}  // namespace driveflow::runtime
