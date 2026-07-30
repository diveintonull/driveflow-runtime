#pragma once

#include "driveflow/protocol/packet.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace driveflow::protocol {

inline constexpr std::size_t kImuPayloadSize = 24U;
inline constexpr std::size_t kGnssPayloadSize = 32U;
inline constexpr std::size_t kCameraMetaFixedPayloadSize = 24U;
inline constexpr std::size_t kMaxCameraExtraDataSize =
    static_cast<std::size_t>(kMaxPayloadSize) - kCameraMetaFixedPayloadSize;

struct Vector3f {
  float x{};
  float y{};
  float z{};

  bool operator==(const Vector3f&) const = default;
};

struct ImuSample {
  Vector3f linear_acceleration_mps2;
  Vector3f angular_velocity_rps;

  bool operator==(const ImuSample&) const = default;
};

struct GnssFix {
  double latitude_deg{};
  double longitude_deg{};
  double altitude_m{};
  float horizontal_accuracy_m{};
  float vertical_accuracy_m{};

  bool operator==(const GnssFix&) const = default;
};

struct CameraMeta {
  std::uint64_t frame_id{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t exposure_time_us{};
  std::vector<std::uint8_t> extra_data;

  bool operator==(const CameraMeta&) const = default;
};

using SensorPayload = std::variant<ImuSample, GnssFix, CameraMeta>;

enum class SensorPayloadError {
  kNone,
  kUnknownMessageType,
  kSizeMismatch,
  kNonFiniteNumber,
  kValueOutOfRange,
};

struct DecodeSensorPayloadResult {
  std::optional<SensorPayload> payload;
  SensorPayloadError error{SensorPayloadError::kNone};

  [[nodiscard]] explicit operator bool() const noexcept { return payload.has_value(); }
};

[[nodiscard]] MessageType message_type(const SensorPayload& payload) noexcept;

[[nodiscard]] std::vector<std::uint8_t> encode_sensor_payload(
    const SensorPayload& payload);

[[nodiscard]] DecodeSensorPayloadResult decode_sensor_payload(
    MessageType type, std::span<const std::uint8_t> bytes);

[[nodiscard]] const char* to_string(SensorPayloadError error) noexcept;

}  // namespace driveflow::protocol
