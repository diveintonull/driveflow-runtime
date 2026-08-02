#pragma once

#include "driveflow/net/udp_socket.hpp"

#include <chrono>
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

struct PeriodicDelay {
  std::uint64_t every{};
  std::chrono::milliseconds duration{};

  bool operator==(const PeriodicDelay&) const = default;
};

// Every interval is evaluated against the generated packet Sequence Number.
// An absent interval disables that fault.
struct FaultInjectionConfig {
  std::optional<std::uint64_t> drop_every;
  std::optional<std::uint64_t> duplicate_every;
  std::optional<std::uint64_t> reorder_every;
  std::optional<PeriodicDelay> delay;
  std::optional<std::uint64_t> corrupt_every;

  bool operator==(const FaultInjectionConfig&) const = default;
};

struct SimulatorConfig {
  SensorType sensor_type{SensorType::kImu};
  net::Ipv4Endpoint destination;
  double rate_hz{};
  // Counts logical packets generated before fault injection. With injection
  // enabled, this can differ from the number of UDP datagrams actually sent.
  std::optional<std::uint64_t> packet_count;
  std::size_t camera_extra_data_bytes{};
  FaultInjectionConfig faults;
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
  std::uint64_t packets_generated{};
  std::uint64_t packets_sent{};
  std::uint64_t packets_dropped{};
  std::uint64_t duplicate_packets_sent{};
  std::uint64_t reorder_events{};
  std::uint64_t delayed_packets{};
  std::uint64_t corrupted_packets_sent{};
};

using StopPredicate = std::function<bool()>;

[[nodiscard]] SimulationSummary run_simulator(const SimulatorConfig& config,
                                              const StopPredicate& stop_requested);

}  // namespace driveflow::simulator
