#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace driveflow::protocol::wire {

template <typename UInt>
void write_big_endian(std::span<std::uint8_t> output, std::size_t offset, UInt value) {
  static_assert(std::is_unsigned_v<UInt>);
  for (std::size_t index = 0; index < sizeof(UInt); ++index) {
    const std::size_t shift = (sizeof(UInt) - index - 1U) * 8U;
    output[offset + index] = static_cast<std::uint8_t>(value >> shift);
  }
}

template <typename UInt>
[[nodiscard]] UInt read_big_endian(std::span<const std::uint8_t> input,
                                   std::size_t offset) {
  static_assert(std::is_unsigned_v<UInt>);
  UInt value{};
  for (std::size_t index = 0; index < sizeof(UInt); ++index) {
    value = static_cast<UInt>((value << 8U) | input[offset + index]);
  }
  return value;
}

inline void write_float(std::span<std::uint8_t> output, std::size_t offset,
                        float value) {
  write_big_endian(output, offset, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] inline float read_float(std::span<const std::uint8_t> input,
                                      std::size_t offset) {
  return std::bit_cast<float>(read_big_endian<std::uint32_t>(input, offset));
}

inline void write_double(std::span<std::uint8_t> output, std::size_t offset,
                         double value) {
  write_big_endian(output, offset, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] inline double read_double(std::span<const std::uint8_t> input,
                                        std::size_t offset) {
  return std::bit_cast<double>(read_big_endian<std::uint64_t>(input, offset));
}

}  // namespace driveflow::protocol::wire
