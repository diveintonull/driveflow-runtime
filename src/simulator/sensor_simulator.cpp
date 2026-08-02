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
#include <vector>

namespace driveflow::simulator {
namespace {

constexpr double kMaximumRateHz = 1'000'000.0;
constexpr auto kMaximumFaultDelay = std::chrono::milliseconds{60'000};

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

void validate_interval(const std::optional<std::uint64_t>& interval,
                       std::string_view name) {
  if (interval.has_value() && *interval == 0U) {
    throw std::invalid_argument(std::string{name} + " must be positive");
  }
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
  validate_interval(config.faults.drop_every, "drop interval");
  validate_interval(config.faults.duplicate_every, "duplicate interval");
  validate_interval(config.faults.reorder_every, "reorder interval");
  validate_interval(config.faults.corrupt_every, "corrupt interval");
  if (config.faults.delay.has_value()) {
    if (config.faults.delay->every == 0U) {
      throw std::invalid_argument("delay interval must be positive");
    }
    if (config.faults.delay->duration <= std::chrono::milliseconds::zero() ||
        config.faults.delay->duration > kMaximumFaultDelay) {
      throw std::invalid_argument("delay must be between 1 and 60000 milliseconds");
    }
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

[[nodiscard]] bool matches_interval(
    const std::optional<std::uint64_t>& interval,
    std::uint64_t sequence_number) noexcept {
  return interval.has_value() && sequence_number % *interval == 0U;
}

struct PendingPacket {
  std::vector<std::uint8_t> bytes;
  bool duplicate{};
  bool corrupted{};
  std::chrono::milliseconds delay{};
};

struct InjectionBatch {
  std::vector<PendingPacket> packets;
  std::uint64_t packets_dropped{};
  std::uint64_t reorder_events{};
};

class FaultInjector {
 public:
  explicit FaultInjector(FaultInjectionConfig config)
      : config_{std::move(config)} {}

  [[nodiscard]] InjectionBatch process(
      std::uint64_t sequence_number, std::vector<std::uint8_t> bytes) {
    InjectionBatch batch;
    if (matches_interval(config_.drop_every, sequence_number)) {
      batch.packets_dropped = 1U;
      return batch;
    }

    PendingPacket packet{
        .bytes = std::move(bytes),
        .duplicate = matches_interval(config_.duplicate_every, sequence_number),
        .corrupted = matches_interval(config_.corrupt_every, sequence_number),
        .delay = delay_for(sequence_number),
    };
    if (packet.corrupted && !packet.bytes.empty()) {
      packet.bytes.back() ^= 0x01U;
    }

    if (has_held_packet_) {
      batch.packets.reserve(2U);
      batch.packets.push_back(std::move(packet));
      batch.packets.push_back(std::move(held_packet_));
      held_packet_ = PendingPacket{};
      has_held_packet_ = false;
      batch.reorder_events = 1U;
      return batch;
    }
    if (matches_interval(config_.reorder_every, sequence_number)) {
      held_packet_ = std::move(packet);
      has_held_packet_ = true;
      return batch;
    }

    batch.packets.push_back(std::move(packet));
    return batch;
  }

  [[nodiscard]] InjectionBatch finish() {
    InjectionBatch batch;
    if (has_held_packet_) {
      batch.packets.push_back(std::move(held_packet_));
      held_packet_ = PendingPacket{};
      has_held_packet_ = false;
    }
    return batch;
  }

 private:
  [[nodiscard]] std::chrono::milliseconds delay_for(
      std::uint64_t sequence_number) const noexcept {
    if (config_.delay.has_value() &&
        sequence_number % config_.delay->every == 0U) {
      return config_.delay->duration;
    }
    return std::chrono::milliseconds::zero();
  }

  FaultInjectionConfig config_;
  PendingPacket held_packet_;
  bool has_held_packet_{};
};

[[nodiscard]] bool send_batch(const InjectionBatch& batch,
                              const SimulatorConfig& config,
                              const StopPredicate& stop_requested,
                              const net::UdpSocket& socket,
                              SimulationSummary& summary) {
  for (const auto& packet : batch.packets) {
    if (stop_requested()) {
      return false;
    }
    if (packet.delay > std::chrono::milliseconds::zero()) {
      ++summary.delayed_packets;
      if (wait_until_or_stop(SimulationClock::now() + packet.delay,
                             stop_requested)) {
        return false;
      }
    }

    socket.send_to(packet.bytes, config.destination);
    ++summary.packets_sent;
    if (packet.corrupted) {
      ++summary.corrupted_packets_sent;
    }
    if (packet.duplicate) {
      socket.send_to(packet.bytes, config.destination);
      ++summary.packets_sent;
      ++summary.duplicate_packets_sent;
      if (packet.corrupted) {
        ++summary.corrupted_packets_sent;
      }
    }
  }
  summary.reorder_events += batch.reorder_events;
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
      .faults = {},
  };
  bool has_sensor = false;
  bool has_explicit_rate = false;
  std::optional<std::uint64_t> delay_every;
  std::optional<std::uint64_t> delay_ms;
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
    } else if (option == "--drop-every") {
      std::uint64_t interval{};
      if (!parse_number(value, interval) || interval == 0U) {
        return parse_failure("drop-every must be a positive integer");
      }
      config.faults.drop_every = interval;
    } else if (option == "--duplicate-every") {
      std::uint64_t interval{};
      if (!parse_number(value, interval) || interval == 0U) {
        return parse_failure("duplicate-every must be a positive integer");
      }
      config.faults.duplicate_every = interval;
    } else if (option == "--reorder-every") {
      std::uint64_t interval{};
      if (!parse_number(value, interval) || interval == 0U) {
        return parse_failure("reorder-every must be a positive integer");
      }
      config.faults.reorder_every = interval;
    } else if (option == "--delay-every") {
      std::uint64_t interval{};
      if (!parse_number(value, interval) || interval == 0U) {
        return parse_failure("delay-every must be a positive integer");
      }
      delay_every = interval;
    } else if (option == "--delay-ms") {
      std::uint64_t duration_ms{};
      if (!parse_number(value, duration_ms) || duration_ms == 0U ||
          duration_ms > static_cast<std::uint64_t>(kMaximumFaultDelay.count())) {
        return parse_failure("delay-ms must be an integer between 1 and 60000");
      }
      delay_ms = duration_ms;
    } else if (option == "--corrupt-every") {
      std::uint64_t interval{};
      if (!parse_number(value, interval) || interval == 0U) {
        return parse_failure("corrupt-every must be a positive integer");
      }
      config.faults.corrupt_every = interval;
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
  if (delay_every.has_value() != delay_ms.has_value()) {
    return parse_failure("--delay-every and --delay-ms must be specified together");
  }
  if (delay_every.has_value()) {
    config.faults.delay = PeriodicDelay{
        .every = *delay_every,
        .duration = std::chrono::milliseconds{
            static_cast<std::chrono::milliseconds::rep>(*delay_ms)},
    };
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
  --count <packets>              Generated packet count (default: continuous)
  --camera-extra-bytes <bytes>   Extra CameraMeta payload bytes (default: 0)
  --drop-every <N>               Drop every Nth generated packet
  --duplicate-every <N>          Duplicate every Nth generated packet
  --reorder-every <N>            Send every Nth packet after its successor
  --delay-every <N>              Delay every Nth generated packet
  --delay-ms <milliseconds>      Delay duration; requires --delay-every
  --corrupt-every <N>            Corrupt every Nth packet after CRC encoding
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
  FaultInjector injector{config.faults};
  SimulationSummary summary;
  const auto period = std::chrono::duration_cast<SimulationClock::duration>(
      std::chrono::duration<double>{1.0 / config.rate_hz});
  auto next_deadline = SimulationClock::now();
  bool stopped = false;

  while (!stop_requested() &&
         (!config.packet_count.has_value() ||
          summary.packets_generated < *config.packet_count)) {
    const auto sequence_number = summary.packets_generated + 1U;
    const auto sample = make_sensor_sample(
        config.sensor_type, sequence_number, config.camera_extra_data_bytes);
    const auto payload = protocol::encode_sensor_payload(sample);
    const auto timestamp_ns = current_timestamp_ns();
    auto packet = protocol::encode_packet(
        protocol::message_type(sample), sequence_number, timestamp_ns, payload);
    ++summary.packets_generated;

    const auto batch = injector.process(sequence_number, std::move(packet));
    summary.packets_dropped += batch.packets_dropped;
    if (!send_batch(batch, config, stop_requested, socket, summary)) {
      stopped = true;
      break;
    }

    if (config.packet_count.has_value() &&
        summary.packets_generated == *config.packet_count) {
      break;
    }
    next_deadline += period;
    if (wait_until_or_stop(next_deadline, stop_requested)) {
      stopped = true;
      break;
    }
  }

  if (!stopped && !stop_requested()) {
    const auto final_batch = injector.finish();
    (void)send_batch(final_batch, config, stop_requested, socket, summary);
  }
  return summary;
}

}  // namespace driveflow::simulator
