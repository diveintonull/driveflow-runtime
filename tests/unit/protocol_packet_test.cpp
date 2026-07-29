#include "driveflow/protocol/packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace driveflow::protocol {
namespace {

constexpr std::size_t kMagicOffset = 0U;
constexpr std::size_t kVersionOffset = 4U;
constexpr std::size_t kMessageTypeOffset = 6U;
constexpr std::size_t kPayloadLengthOffset = 24U;

template <typename UInt>
void overwrite_big_endian(std::vector<std::uint8_t>& bytes, std::size_t offset, UInt value) {
  for (std::size_t index = 0; index < sizeof(UInt); ++index) {
    const std::size_t shift = (sizeof(UInt) - index - 1U) * 8U;
    bytes[offset + index] = static_cast<std::uint8_t>(value >> shift);
  }
}

[[nodiscard]] std::vector<std::uint8_t> sample_packet() {
  constexpr std::array<std::uint8_t, 4> payload{0xdeU, 0xadU, 0xbeU, 0xefU};
  return encode_packet(MessageType::kImu, 0x0102030405060708ULL, 0x1112131415161718ULL,
                       payload);
}

TEST(PacketCodecTest, RoundTripsHeaderAndPayload) {
  constexpr std::array<std::uint8_t, 5> payload{1U, 2U, 3U, 4U, 5U};

  const auto encoded =
      encode_packet(MessageType::kGnss, 42U, 9'876'543'210ULL, payload);
  const auto decoded = decode_packet(encoded);

  ASSERT_TRUE(decoded);
  ASSERT_TRUE(decoded.packet.has_value());
  EXPECT_EQ(decoded.error, DecodeError::kNone);
  EXPECT_EQ(decoded.packet->header.magic, kPacketMagic);
  EXPECT_EQ(decoded.packet->header.version, kProtocolVersion);
  EXPECT_EQ(decoded.packet->header.message_type, MessageType::kGnss);
  EXPECT_EQ(decoded.packet->header.sequence_number, 42U);
  EXPECT_EQ(decoded.packet->header.source_timestamp_ns, 9'876'543'210ULL);
  EXPECT_EQ(decoded.packet->header.payload_length, payload.size());
  EXPECT_EQ(decoded.packet->payload, std::vector<std::uint8_t>(payload.begin(), payload.end()));
}

TEST(PacketCodecTest, WritesIntegersInNetworkByteOrder) {
  const auto encoded = sample_packet();

  constexpr std::array<std::uint8_t, 28> expected_prefix{
      0x44U, 0x52U, 0x46U, 0x4cU, 0x00U, 0x01U, 0x00U, 0x01U, 0x01U, 0x02U,
      0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x11U, 0x12U, 0x13U, 0x14U,
      0x15U, 0x16U, 0x17U, 0x18U, 0x00U, 0x00U, 0x00U, 0x04U};
  EXPECT_TRUE(std::equal(expected_prefix.begin(), expected_prefix.end(), encoded.begin()));
}

TEST(PacketCodecTest, RejectsTruncatedHeader) {
  const std::vector<std::uint8_t> bytes(kPacketHeaderSize - 1U, 0U);
  EXPECT_EQ(decode_packet(bytes).error, DecodeError::kTruncatedHeader);
}

TEST(PacketCodecTest, RejectsTruncatedPayload) {
  auto encoded = sample_packet();
  encoded.pop_back();
  EXPECT_EQ(decode_packet(encoded).error, DecodeError::kTruncatedPayload);
}

TEST(PacketCodecTest, RejectsInvalidMagic) {
  auto encoded = sample_packet();
  encoded[kMagicOffset] ^= 0xffU;
  EXPECT_EQ(decode_packet(encoded).error, DecodeError::kInvalidMagic);
}

TEST(PacketCodecTest, RejectsUnsupportedVersion) {
  auto encoded = sample_packet();
  overwrite_big_endian(encoded, kVersionOffset, static_cast<std::uint16_t>(2U));
  EXPECT_EQ(decode_packet(encoded).error, DecodeError::kUnsupportedVersion);
}

TEST(PacketCodecTest, RejectsUnknownMessageType) {
  auto encoded = sample_packet();
  overwrite_big_endian(encoded, kMessageTypeOffset, static_cast<std::uint16_t>(99U));
  EXPECT_EQ(decode_packet(encoded).error, DecodeError::kUnknownMessageType);
}

TEST(PacketCodecTest, RejectsIncorrectPayloadLength) {
  auto encoded = sample_packet();
  overwrite_big_endian(encoded, kPayloadLengthOffset, static_cast<std::uint32_t>(3U));
  EXPECT_EQ(decode_packet(encoded).error, DecodeError::kLengthMismatch);
}

TEST(PacketCodecTest, RejectsOversizedPayloadLength) {
  auto encoded = sample_packet();
  overwrite_big_endian(encoded, kPayloadLengthOffset, kMaxPayloadSize + 1U);
  EXPECT_EQ(decode_packet(encoded).error, DecodeError::kPayloadTooLarge);
}

TEST(PacketCodecTest, RejectsCrcMismatch) {
  auto encoded = sample_packet();
  encoded.back() ^= 0x01U;
  EXPECT_EQ(decode_packet(encoded).error, DecodeError::kCrcMismatch);
}

TEST(PacketCodecTest, CrcMatchesStandardCheckValue) {
  constexpr std::array<std::uint8_t, 9> input{'1', '2', '3', '4', '5',
                                               '6', '7', '8', '9'};
  EXPECT_EQ(crc32(input), 0xcbf43926U);
}

TEST(PacketCodecTest, EncoderRejectsUnknownMessageType) {
  const auto unknown = static_cast<MessageType>(99U);
  EXPECT_THROW((void)encode_packet(unknown, 0U, 0U, {}), std::invalid_argument);
}

TEST(PacketCodecTest, EncoderRejectsOversizedPayload) {
  const std::vector<std::uint8_t> payload(static_cast<std::size_t>(kMaxPayloadSize) + 1U, 0U);
  EXPECT_THROW((void)encode_packet(MessageType::kCameraMeta, 0U, 0U, payload),
               std::length_error);
}

}  // namespace
}  // namespace driveflow::protocol
