#include "driveflow/protocol/packet.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace driveflow::protocol {
namespace {

constexpr std::size_t kMagicOffset = 0U;
constexpr std::size_t kVersionOffset = 4U;
constexpr std::size_t kMessageTypeOffset = 6U;
constexpr std::size_t kSequenceNumberOffset = 8U;
constexpr std::size_t kTimestampOffset = 16U;
constexpr std::size_t kPayloadLengthOffset = 24U;
constexpr std::size_t kCrcOffset = 28U;
constexpr std::uint32_t kCrcInitialState = 0xffffffffU;
constexpr std::uint32_t kCrcPolynomial = 0xedb88320U;

template <typename UInt>
void write_big_endian(std::span<std::uint8_t> output, std::size_t offset, UInt value) {
  static_assert(std::is_unsigned_v<UInt>);
  for (std::size_t index = 0; index < sizeof(UInt); ++index) {
    const std::size_t shift = (sizeof(UInt) - index - 1U) * 8U;
    output[offset + index] = static_cast<std::uint8_t>(value >> shift);
  }
}

template <typename UInt>
[[nodiscard]] UInt read_big_endian(std::span<const std::uint8_t> input, std::size_t offset) {
  static_assert(std::is_unsigned_v<UInt>);
  UInt value{};
  for (std::size_t index = 0; index < sizeof(UInt); ++index) {
    value = static_cast<UInt>((value << 8U) | input[offset + index]);
  }
  return value;
}

[[nodiscard]] bool is_known_message_type(std::uint16_t raw_type) noexcept {
  switch (static_cast<MessageType>(raw_type)) {
    case MessageType::kImu:
    case MessageType::kGnss:
    case MessageType::kCameraMeta:
      return true;
  }
  return false;
}

[[nodiscard]] std::uint32_t update_crc(std::uint32_t state,
                                       std::span<const std::uint8_t> bytes) noexcept {
  for (const std::uint8_t byte : bytes) {
    state ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (state & 1U);
      state = (state >> 1U) ^ (kCrcPolynomial & mask);
    }
  }
  return state;
}

[[nodiscard]] std::uint32_t packet_crc(std::span<const std::uint8_t> header_without_crc,
                                       std::span<const std::uint8_t> payload) noexcept {
  std::uint32_t state = update_crc(kCrcInitialState, header_without_crc);
  state = update_crc(state, payload);
  return state ^ kCrcInitialState;
}

[[nodiscard]] DecodeResult failure(DecodeError error) {
  return {.packet = std::nullopt, .error = error};
}

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
  return update_crc(kCrcInitialState, bytes) ^ kCrcInitialState;
}

std::vector<std::uint8_t> encode_packet(MessageType message_type, std::uint64_t sequence_number,
                                        std::uint64_t source_timestamp_ns,
                                        std::span<const std::uint8_t> payload) {
  const auto raw_type = static_cast<std::uint16_t>(message_type);
  if (!is_known_message_type(raw_type)) {
    throw std::invalid_argument("unknown DriveFlow message type");
  }
  if (payload.size() > kMaxPayloadSize) {
    throw std::length_error("DriveFlow payload exceeds the v1 limit");
  }

  std::vector<std::uint8_t> encoded(kPacketHeaderSize + payload.size(), 0U);
  const std::span<std::uint8_t> output(encoded);
  write_big_endian(output, kMagicOffset, kPacketMagic);
  write_big_endian(output, kVersionOffset, kProtocolVersion);
  write_big_endian(output, kMessageTypeOffset, raw_type);
  write_big_endian(output, kSequenceNumberOffset, sequence_number);
  write_big_endian(output, kTimestampOffset, source_timestamp_ns);
  write_big_endian(output, kPayloadLengthOffset, static_cast<std::uint32_t>(payload.size()));
  const auto payload_output = encoded.begin() + static_cast<std::ptrdiff_t>(kPacketHeaderSize);
  std::copy(payload.begin(), payload.end(), payload_output);

  const auto header_without_crc = std::span<const std::uint8_t>(encoded).first(kCrcOffset);
  const auto encoded_payload = std::span<const std::uint8_t>(encoded).subspan(kPacketHeaderSize);
  write_big_endian(output, kCrcOffset, packet_crc(header_without_crc, encoded_payload));
  return encoded;
}

DecodeResult decode_packet(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kPacketHeaderSize) {
    return failure(DecodeError::kTruncatedHeader);
  }

  PacketHeader header;
  header.magic = read_big_endian<std::uint32_t>(bytes, kMagicOffset);
  header.version = read_big_endian<std::uint16_t>(bytes, kVersionOffset);
  const auto raw_type = read_big_endian<std::uint16_t>(bytes, kMessageTypeOffset);
  header.message_type = static_cast<MessageType>(raw_type);
  header.sequence_number = read_big_endian<std::uint64_t>(bytes, kSequenceNumberOffset);
  header.source_timestamp_ns = read_big_endian<std::uint64_t>(bytes, kTimestampOffset);
  header.payload_length = read_big_endian<std::uint32_t>(bytes, kPayloadLengthOffset);
  header.crc32 = read_big_endian<std::uint32_t>(bytes, kCrcOffset);

  if (header.magic != kPacketMagic) {
    return failure(DecodeError::kInvalidMagic);
  }
  if (header.version != kProtocolVersion) {
    return failure(DecodeError::kUnsupportedVersion);
  }
  if (!is_known_message_type(raw_type)) {
    return failure(DecodeError::kUnknownMessageType);
  }
  if (header.payload_length > kMaxPayloadSize) {
    return failure(DecodeError::kPayloadTooLarge);
  }

  const std::size_t expected_size = kPacketHeaderSize + header.payload_length;
  if (bytes.size() < expected_size) {
    return failure(DecodeError::kTruncatedPayload);
  }
  if (bytes.size() != expected_size) {
    return failure(DecodeError::kLengthMismatch);
  }

  const auto header_without_crc = bytes.first(kCrcOffset);
  const auto payload = bytes.subspan(kPacketHeaderSize);
  if (packet_crc(header_without_crc, payload) != header.crc32) {
    return failure(DecodeError::kCrcMismatch);
  }

  Packet packet{.header = header, .payload = {}};
  packet.payload.assign(payload.begin(), payload.end());
  return {.packet = std::move(packet), .error = DecodeError::kNone};
}

const char* to_string(DecodeError error) noexcept {
  switch (error) {
    case DecodeError::kNone:
      return "none";
    case DecodeError::kTruncatedHeader:
      return "truncated header";
    case DecodeError::kInvalidMagic:
      return "invalid magic";
    case DecodeError::kUnsupportedVersion:
      return "unsupported version";
    case DecodeError::kUnknownMessageType:
      return "unknown message type";
    case DecodeError::kPayloadTooLarge:
      return "payload too large";
    case DecodeError::kTruncatedPayload:
      return "truncated payload";
    case DecodeError::kLengthMismatch:
      return "packet length mismatch";
    case DecodeError::kCrcMismatch:
      return "CRC mismatch";
  }
  return "unknown decode error";
}

}  // namespace driveflow::protocol
