#include "driveflow/protocol/packet.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto result = driveflow::protocol::decode_packet(std::span(data, size));
  (void)result;
  return 0;
}
