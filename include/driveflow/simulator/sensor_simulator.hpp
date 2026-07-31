#pragma once

#include "driveflow/net/udp_socket.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace driveflow::simulator {

enum class SensorType {
  kImu,
  kGnss,
  kCameraMeta,
};

struct SimulatorConfig {
  SensorType sensor_type{SensorType::kImu};
  net::Ipv4Endpoint destination;
  double rate_hz{};
  std::optional<std::uint64_t> packet_count;
  std::size_t camera_extra_data_bytes{};
};

struct ParseSimulatorConfigResult {
  std::optional<SimulatorConfig> config;
  std::string error;

  [[nodiscard]] explicit operator bool() const noexcept { return config.has_value(); }
};

[[nodiscard]] ParseSimulatorConfigResult parse_simulator_config(
    std::span<const std::string_view> arguments);

[[nodiscard]] std::string_view to_string(SensorType sensor_type) noexcept;
[[nodiscard]] std::string_view simulator_usage() noexcept;

struct SimulationSummary {
  std::uint64_t packets_sent{};
};

using StopPredicate = std::function<bool()>;

[[nodiscard]] SimulationSummary run_simulator(const SimulatorConfig& config,
                                              const StopPredicate& stop_requested);

}  // namespace driveflow::simulator
