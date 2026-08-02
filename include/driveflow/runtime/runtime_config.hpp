#pragma once

#include "driveflow/runtime/epoll_receiver.hpp"
#include "driveflow/runtime/worker_pipeline.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace driveflow::runtime {

struct RuntimeConfig {
  EpollReceiverConfig receiver{
      .listen_endpoints = {{.address = "0.0.0.0", .port = 9'000U}},
  };
  WorkerPipelineConfig pipeline;
  std::size_t max_sensor_sources{256U};
  std::optional<std::uint64_t> packet_count;
  std::chrono::milliseconds poll_timeout{100};
};

struct ParseRuntimeConfigResult {
  std::optional<RuntimeConfig> config;
  std::string error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return config.has_value();
  }
};

[[nodiscard]] ParseRuntimeConfigResult parse_runtime_config(
    std::span<const std::string_view> arguments);
[[nodiscard]] std::string_view runtime_usage() noexcept;

}  // namespace driveflow::runtime
