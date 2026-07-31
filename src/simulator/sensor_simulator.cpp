#include "driveflow/simulator/sensor_simulator.hpp"

#include "driveflow/protocol/sensor_payload.hpp"

#include <arpa/inet.h>

#include <charconv>
#include <chrono>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

namespace driveflow::simulator {
namespace {

constexpr double kMaximumRateHz = 1'000'000.0;

template <typename Number>
[[nodiscard]] bool parse_number(std::string_view text, Number& value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] ParseSimulatorConfigResult parse_failure(std::string error) {
  return {.config = std::nullopt, .error = std::move(error)};
}

[[nodiscard]] bool is_ipv4_address(const std::string& text) {
  in_addr address{};
  return ::inet_pton(AF_INET, text.c_str(), &address) == 1;
}

[[nodiscard]] double default_rate_hz(SensorType sensor_type) noexcept {
  if (sensor_type == SensorType::kImu) {
    return 200.0;
  }
  if (sensor_type == SensorType::kGnss) {
    return 10.0;
  }
  return 30.0;
}

void validate_runtime_config(const SimulatorConfig& config) {
  const bool known_sensor =
      config.sensor_type == SensorType::kImu ||
      config.sensor_type == SensorType::kGnss ||
      config.sensor_type == SensorType::kCameraMeta;
  if (!known_sensor) {
    throw std::invalid_argument("unknown sensor type");
  }
  if (!is_ipv4_address(config.destination.address)) {
    throw std::invalid_argument("destination must be an IPv4 address");
  }
  if (config.destination.port == 0U) {
    throw std::invalid_argument("destination port must be between 1 and 65535");
  }
  if (!std::isfinite(config.rate_hz) || config.rate_hz <= 0.0 ||
      config.rate_hz > kMaximumRateHz) {
    throw std::invalid_argument("rate must be greater than 0 and no more than 1000000 Hz");
  }
  if (config.packet_count.has_value() && *config.packet_count == 0U) {
    throw std::invalid_argument("packet count must be positive");
  }
  if (config.camera_extra_data_bytes > protocol::kMaxCameraExtraDataSize) {
    throw std::invalid_argument("camera extra data exceeds the protocol payload limit");
  }
  if (config.sensor_type != SensorType::kCameraMeta &&
      config.camera_extra_data_bytes != 0U) {
    throw std::invalid_argument("camera extra data is only valid for CameraMeta");
  }
}

using SimulationClock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t current_timestamp_ns() {
  const auto elapsed = SimulationClock::now().time_since_epoch();
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  return static_cast<std::uint64_t>(nanoseconds);
}

[[nodiscard]] protocol::SensorPayload make_sensor_sample(
    SensorType sensor_type, std::uint64_t sequence_number, std::size_t camera_extra_bytes) {
  if (sensor_type == SensorType::kCameraMeta) {
    std::vector<std::uint8_t> extra_data(camera_extra_bytes);
    for (std::size_t index = 0; index < extra_data.size(); ++index) {
      extra_data[index] = static_cast<std::uint8_t>(index % 256U);
    }
    return protocol::CameraMeta{
        .frame_id = sequence_number,
        .width = 1'920U,
        .height = 1'080U,
        .exposure_time_us = 8'000U,
        .extra_data = std::move(extra_data),
    };
  }
  if (sensor_type == SensorType::kGnss) {
    return protocol::GnssFix{
        .latitude_deg = 31.2304,
        .longitude_deg = 121.4737,
        .altitude_m = 4.0,
        .horizontal_accuracy_m = 1.5F,
        .vertical_accuracy_m = 2.5F,
    };
  }
  return protocol::ImuSample{
      .linear_acceleration_mps2 = {.x = 0.0F, .y = 0.0F, .z = 9.81F},
      .angular_velocity_rps = {.x = 0.01F, .y = -0.02F, .z = 0.03F},
  };
}

constexpr auto kStopPollInterval = std::chrono::milliseconds{10};

[[nodiscard]] bool wait_until_or_stop(SimulationClock::time_point deadline,
                                      const StopPredicate& stop_requested) {
  while (!stop_requested()) {
    const auto now = SimulationClock::now();
    if (now >= deadline) {
      return false;
    }
    auto wakeup = now + kStopPollInterval;
    if (wakeup > deadline) {
      wakeup = deadline;
    }
    std::this_thread::sleep_until(wakeup);
  }
  return true;
}

}  // namespace

ParseSimulatorConfigResult parse_simulator_config(
    std::span<const std::string_view> arguments) {
  SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = {.address = "127.0.0.1", .port = 9'000U},
      .rate_hz = 0.0,
      .packet_count = std::nullopt,
      .camera_extra_data_bytes = 0U,
  };
  bool has_sensor = false;
  bool has_explicit_rate = false;
  std::unordered_set<std::string_view> seen_options;

  for (std::size_t index = 0; index < arguments.size(); index += 2U) {
    if (index + 1U >= arguments.size()) {
      return parse_failure("every option requires a value");
    }

    const auto option = arguments[index];
    const auto value = arguments[index + 1U];
    if (!seen_options.insert(option).second) {
      return parse_failure("option specified more than once: " + std::string{option});
    }

    if (option == "--sensor") {
      if (value == "imu") {
        config.sensor_type = SensorType::kImu;
      } else if (value == "gnss") {
        config.sensor_type = SensorType::kGnss;
      } else if (value == "camera") {
        config.sensor_type = SensorType::kCameraMeta;
      } else {
        return parse_failure("sensor must be imu, gnss, or camera");
      }
      has_sensor = true;
    } else if (option == "--destination") {
      config.destination.address = value;
    } else if (option == "--port") {
      if (!parse_number(value, config.destination.port)) {
        return parse_failure("port must be an integer between 1 and 65535");
      }
    } else if (option == "--rate-hz") {
      if (!parse_number(value, config.rate_hz)) {
        return parse_failure("rate-hz must be a number");
      }
      has_explicit_rate = true;
    } else if (option == "--count") {
      std::uint64_t packet_count{};
      if (!parse_number(value, packet_count)) {
        return parse_failure("count must be a positive integer");
      }
      config.packet_count = packet_count;
    } else if (option == "--camera-extra-bytes") {
      if (!parse_number(value, config.camera_extra_data_bytes)) {
        return parse_failure("camera-extra-bytes must be a non-negative integer");
      }
    } else {
      return parse_failure("unknown option: " + std::string{option});
    }
  }

  if (!has_sensor) {
    return parse_failure("--sensor is required");
  }
  if (!has_explicit_rate) {
    config.rate_hz = default_rate_hz(config.sensor_type);
  }
  if (!is_ipv4_address(config.destination.address)) {
    return parse_failure("destination must be an IPv4 address");
  }
  if (config.destination.port == 0U) {
    return parse_failure("port must be an integer between 1 and 65535");
  }
  if (!std::isfinite(config.rate_hz) || config.rate_hz <= 0.0 ||
      config.rate_hz > kMaximumRateHz) {
    return parse_failure("rate-hz must be greater than 0 and no more than 1000000");
  }
  if (config.packet_count.has_value() && *config.packet_count == 0U) {
    return parse_failure("count must be a positive integer");
  }
  if (config.camera_extra_data_bytes > protocol::kMaxCameraExtraDataSize) {
    return parse_failure("camera-extra-bytes exceeds the protocol payload limit");
  }
  if (config.sensor_type != SensorType::kCameraMeta &&
      config.camera_extra_data_bytes != 0U) {
    return parse_failure("camera-extra-bytes is only valid for the camera sensor");
  }

  return {.config = config, .error = {}};
}

std::string_view to_string(SensorType sensor_type) noexcept {
  switch (sensor_type) {
    case SensorType::kImu:
      return "imu";
    case SensorType::kGnss:
      return "gnss";
    case SensorType::kCameraMeta:
      return "camera";
  }
  return "unknown";
}

std::string_view simulator_usage() noexcept {
  return R"(usage: driveflow_sensor_simulator --sensor <imu|gnss|camera> [options]

options:
  --destination <IPv4>           Destination address (default: 127.0.0.1)
  --port <port>                  Destination port (default: 9000)
  --rate-hz <frequency>          Override the sensor default rate
  --count <packets>              Stop after this many packets (default: continuous)
  --camera-extra-bytes <bytes>   Extra CameraMeta payload bytes (default: 0)
  --help                         Show this help
)";
}

SimulationSummary run_simulator(const SimulatorConfig& config,
                                const StopPredicate& stop_requested) {
  validate_runtime_config(config);
  if (!stop_requested) {
    throw std::invalid_argument("stop predicate must be callable");
  }
  auto socket = net::UdpSocket::open();
  SimulationSummary summary;
  const auto period = std::chrono::duration_cast<SimulationClock::duration>(
      std::chrono::duration<double>{1.0 / config.rate_hz});
  auto next_deadline = SimulationClock::now();

  while (!stop_requested() &&
         (!config.packet_count.has_value() ||
          summary.packets_sent < *config.packet_count)) {
    const auto sample = make_sensor_sample(
        config.sensor_type, summary.packets_sent + 1U, config.camera_extra_data_bytes);
    const auto payload = protocol::encode_sensor_payload(sample);
    const auto timestamp_ns = current_timestamp_ns();
    const auto packet = protocol::encode_packet(
        protocol::message_type(sample), summary.packets_sent + 1U, timestamp_ns, payload);
    socket.send_to(packet, config.destination);
    ++summary.packets_sent;

    if (config.packet_count.has_value() &&
        summary.packets_sent == *config.packet_count) {
      break;
    }
    next_deadline += period;
    if (wait_until_or_stop(next_deadline, stop_requested)) {
      break;
    }
  }
  return summary;
}

}  // namespace driveflow::simulator
