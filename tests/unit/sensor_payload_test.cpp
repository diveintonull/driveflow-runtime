#include "driveflow/protocol/sensor_payload.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace driveflow::protocol {
namespace {

template <typename UInt>
void overwrite_big_endian(std::vector<std::uint8_t>& bytes, std::size_t offset, UInt value) {
  for (std::size_t index = 0; index < sizeof(UInt); ++index) {
    const std::size_t shift = (sizeof(UInt) - index - 1U) * 8U;
    bytes[offset + index] = static_cast<std::uint8_t>(value >> shift);
  }
}

TEST(SensorPayloadTest, MapsVariantAlternativeToMessageType) {
  EXPECT_EQ(message_type(SensorPayload{ImuSample{}}), MessageType::kImu);
  EXPECT_EQ(message_type(SensorPayload{GnssFix{}}), MessageType::kGnss);
  EXPECT_EQ(message_type(SensorPayload{CameraMeta{}}), MessageType::kCameraMeta);
}

TEST(SensorPayloadTest, RoundTripsImuSample) {
  const ImuSample sample{
      .linear_acceleration_mps2 = {.x = 1.25F, .y = -2.5F, .z = 9.81F},
      .angular_velocity_rps = {.x = 0.1F, .y = -0.2F, .z = 0.3F},
  };

  const auto bytes = encode_sensor_payload(SensorPayload{sample});
  const auto decoded = decode_sensor_payload(MessageType::kImu, bytes);

  ASSERT_TRUE(decoded);
  ASSERT_TRUE(decoded.payload.has_value());
  EXPECT_EQ(bytes.size(), kImuPayloadSize);
  EXPECT_EQ(std::get<ImuSample>(*decoded.payload), sample);
}

TEST(SensorPayloadTest, WritesFloatBitsInNetworkByteOrder) {
  const ImuSample sample{
      .linear_acceleration_mps2 = {.x = 1.0F, .y = -2.0F, .z = 0.0F},
      .angular_velocity_rps = {},
  };

  const auto bytes = encode_sensor_payload(SensorPayload{sample});

  EXPECT_EQ(bytes[0], 0x3fU);
  EXPECT_EQ(bytes[1], 0x80U);
  EXPECT_EQ(bytes[2], 0x00U);
  EXPECT_EQ(bytes[3], 0x00U);
  EXPECT_EQ(bytes[4], 0xc0U);
  EXPECT_EQ(bytes[5], 0x00U);
  EXPECT_EQ(bytes[6], 0x00U);
  EXPECT_EQ(bytes[7], 0x00U);
}

TEST(SensorPayloadTest, RoundTripsGnssFix) {
  const GnssFix fix{
      .latitude_deg = 1.3521,
      .longitude_deg = 103.8198,
      .altitude_m = 15.75,
      .horizontal_accuracy_m = 0.8F,
      .vertical_accuracy_m = 1.2F,
  };

  const auto bytes = encode_sensor_payload(SensorPayload{fix});
  const auto decoded = decode_sensor_payload(MessageType::kGnss, bytes);

  ASSERT_TRUE(decoded);
  ASSERT_TRUE(decoded.payload.has_value());
  EXPECT_EQ(bytes.size(), kGnssPayloadSize);
  EXPECT_EQ(std::get<GnssFix>(*decoded.payload), fix);
}

TEST(SensorPayloadTest, RoundTripsCameraMetadataAndExtraData) {
  const CameraMeta metadata{
      .frame_id = 42U,
      .width = 1920U,
      .height = 1080U,
      .exposure_time_us = 8'000U,
      .extra_data = {0xdeU, 0xadU, 0xbeU, 0xefU},
  };

  const auto bytes = encode_sensor_payload(SensorPayload{metadata});
  const auto decoded = decode_sensor_payload(MessageType::kCameraMeta, bytes);

  ASSERT_TRUE(decoded);
  ASSERT_TRUE(decoded.payload.has_value());
  EXPECT_EQ(bytes.size(), kCameraMetaFixedPayloadSize + metadata.extra_data.size());
  EXPECT_EQ(std::get<CameraMeta>(*decoded.payload), metadata);
}

TEST(SensorPayloadTest, WritesCameraExtraDataLengthInNetworkByteOrder) {
  const CameraMeta metadata{
      .frame_id = 1U,
      .width = 640U,
      .height = 480U,
      .exposure_time_us = 1'000U,
      .extra_data = {1U, 2U, 3U},
  };

  const auto bytes = encode_sensor_payload(SensorPayload{metadata});

  EXPECT_EQ(bytes[20], 0x00U);
  EXPECT_EQ(bytes[21], 0x00U);
  EXPECT_EQ(bytes[22], 0x00U);
  EXPECT_EQ(bytes[23], 0x03U);
}

TEST(SensorPayloadTest, RejectsWrongFixedPayloadSizes) {
  EXPECT_EQ(decode_sensor_payload(MessageType::kImu,
                                  std::vector<std::uint8_t>(kImuPayloadSize - 1U, 0U))
                .error,
            SensorPayloadError::kSizeMismatch);
  EXPECT_EQ(decode_sensor_payload(MessageType::kGnss,
                                  std::vector<std::uint8_t>(kGnssPayloadSize + 1U, 0U))
                .error,
            SensorPayloadError::kSizeMismatch);
}

TEST(SensorPayloadTest, RejectsCameraExtraDataLengthMismatch) {
  CameraMeta metadata{
      .frame_id = 1U,
      .width = 640U,
      .height = 480U,
      .exposure_time_us = 1'000U,
      .extra_data = {1U, 2U, 3U},
  };
  auto bytes = encode_sensor_payload(SensorPayload{metadata});
  overwrite_big_endian(bytes, 20U, static_cast<std::uint32_t>(4U));

  EXPECT_EQ(decode_sensor_payload(MessageType::kCameraMeta, bytes).error,
            SensorPayloadError::kSizeMismatch);
}

TEST(SensorPayloadTest, EncoderRejectsNonFiniteImuValue) {
  ImuSample sample{};
  sample.angular_velocity_rps.z = std::numeric_limits<float>::infinity();

  EXPECT_THROW((void)encode_sensor_payload(SensorPayload{sample}), std::invalid_argument);
}

TEST(SensorPayloadTest, EncoderRejectsOutOfRangeGnssCoordinate) {
  const GnssFix fix{
      .latitude_deg = 91.0,
      .longitude_deg = 0.0,
      .altitude_m = 0.0,
      .horizontal_accuracy_m = 1.0F,
      .vertical_accuracy_m = 1.0F,
  };

  EXPECT_THROW((void)encode_sensor_payload(SensorPayload{fix}), std::invalid_argument);
}

TEST(SensorPayloadTest, DecoderRejectsOutOfRangeGnssCoordinate) {
  const GnssFix valid{
      .latitude_deg = 45.0,
      .longitude_deg = 90.0,
      .altitude_m = 10.0,
      .horizontal_accuracy_m = 1.0F,
      .vertical_accuracy_m = 2.0F,
  };
  auto bytes = encode_sensor_payload(SensorPayload{valid});
  overwrite_big_endian(bytes, 0U, std::bit_cast<std::uint64_t>(91.0));

  EXPECT_EQ(decode_sensor_payload(MessageType::kGnss, bytes).error,
            SensorPayloadError::kValueOutOfRange);
}

TEST(SensorPayloadTest, DecoderRejectsZeroCameraDimension) {
  std::vector<std::uint8_t> bytes(kCameraMetaFixedPayloadSize, 0U);

  EXPECT_EQ(decode_sensor_payload(MessageType::kCameraMeta, bytes).error,
            SensorPayloadError::kValueOutOfRange);
}

TEST(SensorPayloadTest, EncoderRejectsOversizedCameraExtraData) {
  const CameraMeta metadata{
      .frame_id = 1U,
      .width = 640U,
      .height = 480U,
      .exposure_time_us = 1'000U,
      .extra_data = std::vector<std::uint8_t>(kMaxCameraExtraDataSize + 1U, 0U),
  };

  EXPECT_THROW((void)encode_sensor_payload(SensorPayload{metadata}), std::length_error);
}

TEST(SensorPayloadTest, RejectsUnknownMessageType) {
  const auto unknown = static_cast<MessageType>(99U);

  EXPECT_EQ(decode_sensor_payload(unknown, {}).error,
            SensorPayloadError::kUnknownMessageType);
}

}  // namespace
}  // namespace driveflow::protocol
