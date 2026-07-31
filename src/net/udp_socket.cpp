#include "driveflow/net/udp_socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace driveflow::net {
namespace {

[[noreturn]] void throw_system_error(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

[[nodiscard]] sockaddr_in to_sockaddr(const Ipv4Endpoint& endpoint) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(endpoint.port);

  const int result = ::inet_pton(AF_INET, endpoint.address.c_str(), &address.sin_addr);
  if (result == 0) {
    throw std::invalid_argument("invalid IPv4 address: " + endpoint.address);
  }
  if (result < 0) {
    throw_system_error("inet_pton");
  }
  return address;
}

[[nodiscard]] Ipv4Endpoint from_sockaddr(const sockaddr_in& address) {
  std::array<char, INET_ADDRSTRLEN> text{};
  if (::inet_ntop(AF_INET, &address.sin_addr, text.data(), text.size()) == nullptr) {
    throw_system_error("inet_ntop");
  }
  return {.address = text.data(), .port = ntohs(address.sin_port)};
}

void close_socket(int file_descriptor) noexcept {
  if (file_descriptor >= 0) {
    (void)::close(file_descriptor);
  }
}
[[nodiscard]] std::optional<ReceivedDatagram> receive_datagram(
    int file_descriptor, std::size_t maximum_size, bool return_would_block) {
  if (maximum_size == 0U || maximum_size > kMaxUdpPayloadSize) {
    throw std::invalid_argument("UDP receive size must be between 1 and 65507 bytes");
  }

  std::vector<std::uint8_t> bytes(maximum_size);
  sockaddr_in source{};
  iovec buffer{};
  buffer.iov_base = bytes.data();
  buffer.iov_len = bytes.size();

  msghdr message{};
  message.msg_name = &source;
  message.msg_namelen = sizeof(source);
  message.msg_iov = &buffer;
  message.msg_iovlen = 1U;

  const ssize_t received = ::recvmsg(file_descriptor, &message, 0);
  if (received < 0) {
    if (return_would_block && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return std::nullopt;
    }
    throw_system_error("recvmsg");
  }
  if ((message.msg_flags & MSG_TRUNC) != 0 ||
      static_cast<std::size_t>(received) > maximum_size) {
    throw std::length_error("received UDP datagram exceeds the configured buffer");
  }

  bytes.resize(static_cast<std::size_t>(received));
  return ReceivedDatagram{
      .bytes = std::move(bytes),
      .source = from_sockaddr(source),
  };
}


}  // namespace

UdpSocket::UdpSocket(int file_descriptor) noexcept : file_descriptor_(file_descriptor) {}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : file_descriptor_(std::exchange(other.file_descriptor_, -1)) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
  if (this != &other) {
    close_socket(file_descriptor_);
    file_descriptor_ = std::exchange(other.file_descriptor_, -1);
  }
  return *this;
}

UdpSocket::~UdpSocket() { close_socket(file_descriptor_); }

UdpSocket UdpSocket::open() {
  const int file_descriptor = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
  if (file_descriptor < 0) {
    throw_system_error("socket");
  }
  return UdpSocket(file_descriptor);
}

UdpSocket UdpSocket::bind_to(const Ipv4Endpoint& local_endpoint) {
  UdpSocket socket = open();
  const sockaddr_in address = to_sockaddr(local_endpoint);
  if (::bind(socket.file_descriptor_, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) < 0) {
    throw_system_error("bind");
  }
  return socket;
}

void UdpSocket::set_non_blocking() const {
  const int flags = ::fcntl(file_descriptor_, F_GETFL);
  if (flags < 0) {
    throw_system_error("fcntl(F_GETFL)");
  }
  if (::fcntl(file_descriptor_, F_SETFL, flags | O_NONBLOCK) < 0) {
    throw_system_error("fcntl(F_SETFL)");
  }
}

void UdpSocket::send_to(std::span<const std::uint8_t> bytes,
                        const Ipv4Endpoint& destination) const {
  if (bytes.size() > kMaxUdpPayloadSize) {
    throw std::length_error("UDP payload exceeds the IPv4 limit");
  }

  const sockaddr_in address = to_sockaddr(destination);
  const ssize_t sent = ::sendto(file_descriptor_, bytes.data(), bytes.size(), 0,
                                reinterpret_cast<const sockaddr*>(&address), sizeof(address));
  if (sent < 0) {
    throw_system_error("sendto");
  }
  if (static_cast<std::size_t>(sent) != bytes.size()) {
    throw std::runtime_error("sendto returned a partial UDP datagram");
  }
}

ReceivedDatagram UdpSocket::receive(std::size_t maximum_size) const {
  auto datagram = receive_datagram(file_descriptor_, maximum_size, false);
  return std::move(*datagram);
}

std::optional<ReceivedDatagram> UdpSocket::try_receive(
    std::size_t maximum_size) const {
  return receive_datagram(file_descriptor_, maximum_size, true);
}

Ipv4Endpoint UdpSocket::local_endpoint() const {
  sockaddr_in address{};
  socklen_t address_size = sizeof(address);
  if (::getsockname(file_descriptor_, reinterpret_cast<sockaddr*>(&address), &address_size) < 0) {
    throw_system_error("getsockname");
  }
  return from_sockaddr(address);
}

int UdpSocket::native_handle() const noexcept { return file_descriptor_; }

}  // namespace driveflow::net
