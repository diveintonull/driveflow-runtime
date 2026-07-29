#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace driveflow::net {

inline constexpr std::size_t kMaxUdpPayloadSize = 65'507U;

struct Ipv4Endpoint {
  std::string address;
  std::uint16_t port{};

  bool operator==(const Ipv4Endpoint&) const = default;
};

struct ReceivedDatagram {
  std::vector<std::uint8_t> bytes;
  Ipv4Endpoint source;
};

class UdpSocket {
 public:
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  UdpSocket(UdpSocket&& other) noexcept;
  UdpSocket& operator=(UdpSocket&& other) noexcept;
  ~UdpSocket();

  [[nodiscard]] static UdpSocket open();
  [[nodiscard]] static UdpSocket bind_to(const Ipv4Endpoint& local_endpoint);

  void send_to(std::span<const std::uint8_t> bytes,
               const Ipv4Endpoint& destination) const;
  [[nodiscard]] ReceivedDatagram receive(
      std::size_t maximum_size = kMaxUdpPayloadSize) const;
  [[nodiscard]] Ipv4Endpoint local_endpoint() const;

 private:
  explicit UdpSocket(int file_descriptor) noexcept;

  int file_descriptor_{-1};
};

}  // namespace driveflow::net
