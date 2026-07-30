#include "driveflow/protocol/sensor_payload.hpp"

#include "wire.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace driveflow::protocol {
namespace {

static_assert(sizeof(float) == sizeof(std::uint32_t));
static_assert(sizeof(double) == sizeof(std::uint64_t));
static_assert(std::numeric_limits<float>::is_iec559);
static_assert(std::numeric_limits<double>::is_iec559);

constexpr std::size_t kImuAccelerationXOffset = 0U;
constexpr std::size_t kImuAccelerationYOffset = 4U;
constexpr std::size_t kImuAccelerationZOffset = 8U;
constexpr std::size_t kImuAngularVelocityXOffset = 12U;
constexpr std::size_t kImuAngularVelocityYOffset = 16U;
constexpr std::size_t kImuAngularVelocityZOffset = 20U;

constexpr std::size_t kGnssLatitudeOffset = 0U;
constexpr std::size_t kGnssLongitudeOffset = 8U;
constexpr std::size_t kGnssAltitudeOffset = 16U;
constexpr std::size_t kGnssHorizontalAccuracyOffset = 24U;
constexpr std::size_t kGnssVerticalAccuracyOffset = 28U;

constexpr std::size_t kCameraFrameIdOffset = 0U;
constexpr std::size_t kCameraWidthOffset = 8U;
constexpr std::size_t kCameraHeightOffset = 12U;
constexpr std::size_t kCameraExposureOffset = 16U;
constexpr std::size_t kCameraExtraDataLengthOffset = 20U;

[[nodiscard]] SensorPayloadError validate(const ImuSample& sample) noexcept {
  const auto& acceleration = sample.linear_acceleration_mps2;
  const auto& angular_velocity = sample.angular_velocity_rps;
  if (!std::isfinite(acceleration.x) || !std::isfinite(acceleration.y) ||
      !std::isfinite(acceleration.z) || !std::isfinite(angular_velocity.x) ||
      !std::isfinite(angular_velocity.y) || !std::isfinite(angular_velocity.z)) {
    return SensorPayloadError::kNonFiniteNumber;
  }
  return SensorPayloadError::kNone;
}

[[nodiscard]] SensorPayloadError validate(const GnssFix& fix) noexcept {
  if (!std::isfinite(fix.latitude_deg) || !std::isfinite(fix.longitude_deg) ||
      !std::isfinite(fix.altitude_m) || !std::isfinite(fix.horizontal_accuracy_m) ||
      !std::isfinite(fix.vertical_accuracy_m)) {
    return SensorPayloadError::kNonFiniteNumber;
  }
  if (fix.latitude_deg < -90.0 || fix.latitude_deg > 90.0 ||
      fix.longitude_deg < -180.0 || fix.longitude_deg > 180.0 ||
      fix.horizontal_accuracy_m < 0.0F || fix.vertical_accuracy_m < 0.0F) {
    return SensorPayloadError::kValueOutOfRange;
  }
  return SensorPayloadError::kNone;
}

[[nodiscard]] SensorPayloadError validate(const CameraMeta& metadata) noexcept {
  if (metadata.width == 0U || metadata.height == 0U ||
      metadata.extra_data.size() > kMaxCameraExtraDataSize) {
    return SensorPayloadError::kValueOutOfRange;
  }
  return SensorPayloadError::kNone;
}

void require_valid(const SensorPayloadError error) {
  if (error != SensorPayloadError::kNone) {
    throw std::invalid_argument(to_string(error));
  }
}

[[nodiscard]] std::vector<std::uint8_t> encode(const ImuSample& sample) {
  require_valid(validate(sample));
  std::vector<std::uint8_t> bytes(kImuPayloadSize, 0U);
  const std::span<std::uint8_t> output(bytes);
  wire::write_float(output, kImuAccelerationXOffset, sample.linear_acceleration_mps2.x);
  wire::write_float(output, kImuAccelerationYOffset, sample.linear_acceleration_mps2.y);
  wire::write_float(output, kImuAccelerationZOffset, sample.linear_acceleration_mps2.z);
  wire::write_float(output, kImuAngularVelocityXOffset, sample.angular_velocity_rps.x);
  wire::write_float(output, kImuAngularVelocityYOffset, sample.angular_velocity_rps.y);
  wire::write_float(output, kImuAngularVelocityZOffset, sample.angular_velocity_rps.z);
  return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> encode(const GnssFix& fix) {
  require_valid(validate(fix));
  std::vector<std::uint8_t> bytes(kGnssPayloadSize, 0U);
  const std::span<std::uint8_t> output(bytes);
  wire::write_double(output, kGnssLatitudeOffset, fix.latitude_deg);
  wire::write_double(output, kGnssLongitudeOffset, fix.longitude_deg);
  wire::write_double(output, kGnssAltitudeOffset, fix.altitude_m);
  wire::write_float(output, kGnssHorizontalAccuracyOffset, fix.horizontal_accuracy_m);
  wire::write_float(output, kGnssVerticalAccuracyOffset, fix.vertical_accuracy_m);
  return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> encode(const CameraMeta& metadata) {
  if (metadata.extra_data.size() > kMaxCameraExtraDataSize) {
    throw std::length_error("camera extra data exceeds the DriveFlow payload limit");
  }
  require_valid(validate(metadata));

  std::vector<std::uint8_t> bytes(kCameraMetaFixedPayloadSize + metadata.extra_data.size(), 0U);
  const std::span<std::uint8_t> output(bytes);
  wire::write_big_endian(output, kCameraFrameIdOffset, metadata.frame_id);
  wire::write_big_endian(output, kCameraWidthOffset, metadata.width);
  wire::write_big_endian(output, kCameraHeightOffset, metadata.height);
  wire::write_big_endian(output, kCameraExposureOffset, metadata.exposure_time_us);
  wire::write_big_endian(output, kCameraExtraDataLengthOffset,
                         static_cast<std::uint32_t>(metadata.extra_data.size()));
  std::copy(metadata.extra_data.begin(), metadata.extra_data.end(),
            output.begin() + static_cast<std::ptrdiff_t>(kCameraMetaFixedPayloadSize));
  return bytes;
}

[[nodiscard]] DecodeSensorPayloadResult failure(SensorPayloadError error) {
  return {.payload = std::nullopt, .error = error};
}

[[nodiscard]] DecodeSensorPayloadResult success(SensorPayload payload) {
  return {.payload = std::move(payload), .error = SensorPayloadError::kNone};
}

[[nodiscard]] DecodeSensorPayloadResult decode_imu(std::span<const std::uint8_t> bytes) {
  if (bytes.size() != kImuPayloadSize) {
    return failure(SensorPayloadError::kSizeMismatch);
  }

  ImuSample sample{
      .linear_acceleration_mps2 =
          {.x = wire::read_float(bytes, kImuAccelerationXOffset),
           .y = wire::read_float(bytes, kImuAccelerationYOffset),
           .z = wire::read_float(bytes, kImuAccelerationZOffset)},
      .angular_velocity_rps =
          {.x = wire::read_float(bytes, kImuAngularVelocityXOffset),
           .y = wire::read_float(bytes, kImuAngularVelocityYOffset),
           .z = wire::read_float(bytes, kImuAngularVelocityZOffset)},
  };
  const SensorPayloadError error = validate(sample);
  return error == SensorPayloadError::kNone ? success(SensorPayload{sample}) : failure(error);
}

[[nodiscard]] DecodeSensorPayloadResult decode_gnss(std::span<const std::uint8_t> bytes) {
  if (bytes.size() != kGnssPayloadSize) {
    return failure(SensorPayloadError::kSizeMismatch);
  }

  GnssFix fix{
      .latitude_deg = wire::read_double(bytes, kGnssLatitudeOffset),
      .longitude_deg = wire::read_double(bytes, kGnssLongitudeOffset),
      .altitude_m = wire::read_double(bytes, kGnssAltitudeOffset),
      .horizontal_accuracy_m = wire::read_float(bytes, kGnssHorizontalAccuracyOffset),
      .vertical_accuracy_m = wire::read_float(bytes, kGnssVerticalAccuracyOffset),
  };
  const SensorPayloadError error = validate(fix);
  return error == SensorPayloadError::kNone ? success(SensorPayload{fix}) : failure(error);
}

[[nodiscard]] DecodeSensorPayloadResult decode_camera(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kCameraMetaFixedPayloadSize) {
    return failure(SensorPayloadError::kSizeMismatch);
  }

  const std::uint32_t raw_extra_size =
      wire::read_big_endian<std::uint32_t>(bytes, kCameraExtraDataLengthOffset);
  const std::size_t extra_size = raw_extra_size;
  if (extra_size > kMaxCameraExtraDataSize) {
    return failure(SensorPayloadError::kValueOutOfRange);
  }
  if (bytes.size() != kCameraMetaFixedPayloadSize + extra_size) {
    return failure(SensorPayloadError::kSizeMismatch);
  }

  CameraMeta metadata{
      .frame_id = wire::read_big_endian<std::uint64_t>(bytes, kCameraFrameIdOffset),
      .width = wire::read_big_endian<std::uint32_t>(bytes, kCameraWidthOffset),
      .height = wire::read_big_endian<std::uint32_t>(bytes, kCameraHeightOffset),
      .exposure_time_us =
          wire::read_big_endian<std::uint32_t>(bytes, kCameraExposureOffset),
      .extra_data = {},
  };
  const auto extra_bytes = bytes.subspan(kCameraMetaFixedPayloadSize);
  metadata.extra_data.assign(extra_bytes.begin(), extra_bytes.end());

  const SensorPayloadError error = validate(metadata);
  return error == SensorPayloadError::kNone ? success(SensorPayload{std::move(metadata)})
                                            : failure(error);
}

}  // namespace

MessageType message_type(const SensorPayload& payload) noexcept {
  return std::visit(
      [](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, ImuSample>) {
          return MessageType::kImu;
        } else if constexpr (std::is_same_v<Value, GnssFix>) {
          return MessageType::kGnss;
        } else {
          return MessageType::kCameraMeta;
        }
      },
      payload);
}

std::vector<std::uint8_t> encode_sensor_payload(const SensorPayload& payload) {
  return std::visit([](const auto& value) { return encode(value); }, payload);
}

DecodeSensorPayloadResult decode_sensor_payload(MessageType type,
                                                std::span<const std::uint8_t> bytes) {
  switch (type) {
    case MessageType::kImu:
      return decode_imu(bytes);
    case MessageType::kGnss:
      return decode_gnss(bytes);
    case MessageType::kCameraMeta:
      return decode_camera(bytes);
  }
  return failure(SensorPayloadError::kUnknownMessageType);
}

const char* to_string(SensorPayloadError error) noexcept {
  switch (error) {
    case SensorPayloadError::kNone:
      return "none";
    case SensorPayloadError::kUnknownMessageType:
      return "unknown sensor message type";
    case SensorPayloadError::kSizeMismatch:
      return "sensor payload size mismatch";
    case SensorPayloadError::kNonFiniteNumber:
      return "sensor payload contains a non-finite number";
    case SensorPayloadError::kValueOutOfRange:
      return "sensor payload value is out of range";
  }
  return "unknown sensor payload error";
}

}  // namespace driveflow::protocol
