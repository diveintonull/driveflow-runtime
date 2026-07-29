#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace driveflow::protocol {

inline constexpr std::uint32_t kPacketMagic = 0x4452464cU;
inline constexpr std::uint16_t kProtocolVersion = 1U;
inline constexpr std::size_t kPacketHeaderSize = 32U;
inline constexpr std::uint32_t kMaxPayloadSize = 65'475U;

enum class MessageType : std::uint16_t {
  kImu = 1U,
  kGnss = 2U,
  kCameraMeta = 3U,
};

struct PacketHeader {
  std::uint32_t magic{kPacketMagic};
  std::uint16_t version{kProtocolVersion};
  MessageType message_type{MessageType::kImu};
  std::uint64_t sequence_number{};
  std::uint64_t source_timestamp_ns{};
  std::uint32_t payload_length{};
  std::uint32_t crc32{};

  bool operator==(const PacketHeader&) const = default;
};

struct Packet {
  PacketHeader header;
  std::vector<std::uint8_t> payload;

  bool operator==(const Packet&) const = default;
};

enum class DecodeError {
  kNone,
  kTruncatedHeader,
  kInvalidMagic,
  kUnsupportedVersion,
  kUnknownMessageType,
  kPayloadTooLarge,
  kTruncatedPayload,
  kLengthMismatch,
  kCrcMismatch,
};

struct DecodeResult {
  std::optional<Packet> packet;
  DecodeError error{DecodeError::kNone};

  [[nodiscard]] explicit operator bool() const noexcept { return packet.has_value(); }
};

[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;

[[nodiscard]] std::vector<std::uint8_t> encode_packet(
    MessageType message_type, std::uint64_t sequence_number, std::uint64_t source_timestamp_ns,
    std::span<const std::uint8_t> payload);

[[nodiscard]] DecodeResult decode_packet(std::span<const std::uint8_t> bytes);

[[nodiscard]] const char* to_string(DecodeError error) noexcept;

}  // namespace driveflow::protocol
