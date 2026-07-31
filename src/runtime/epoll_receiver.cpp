#include "driveflow/runtime/epoll_receiver.hpp"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace driveflow::runtime {
namespace {

[[noreturn]] void throw_system_error(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

class FileDescriptor {
 public:
  explicit FileDescriptor(int value) noexcept : value_(value) {}

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept
      : value_(std::exchange(other.value_, -1)) {}

  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      close();
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }

  ~FileDescriptor() { close(); }

  [[nodiscard]] int get() const noexcept { return value_; }

 private:
  void close() noexcept {
    if (value_ >= 0) {
      (void)::close(value_);
      value_ = -1;
    }
  }

  int value_{-1};
};

[[nodiscard]] int create_epoll(const EpollReceiverConfig& config) {
  if (config.listen_endpoints.empty()) {
    throw std::invalid_argument("epoll receiver requires at least one listen endpoint");
  }
  if (config.max_datagrams_per_listener_per_poll == 0U) {
    throw std::invalid_argument(
        "max datagrams per listener per poll must be greater than zero");
  }
  if (config.listen_endpoints.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("too many epoll listen endpoints");
  }

  const int file_descriptor = ::epoll_create1(EPOLL_CLOEXEC);
  if (file_descriptor < 0) {
    throw_system_error("epoll_create1");
  }
  return file_descriptor;
}

[[nodiscard]] std::uint64_t current_receive_timestamp_ns() noexcept {
  const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  return static_cast<std::uint64_t>(nanoseconds);
}

struct Listener {
  net::UdpSocket socket;
  net::Ipv4Endpoint local_endpoint;
};

}  // namespace

struct EpollReceiver::Impl {
  explicit Impl(const EpollReceiverConfig& config)
      : epoll_file_descriptor(create_epoll(config)),
        max_datagrams_per_listener_per_poll(
            config.max_datagrams_per_listener_per_poll) {
    listeners.reserve(config.listen_endpoints.size());
    local_endpoints.reserve(config.listen_endpoints.size());

    for (const auto& requested_endpoint : config.listen_endpoints) {
      auto socket = net::UdpSocket::bind_to(requested_endpoint);
      socket.set_non_blocking();
      auto actual_endpoint = socket.local_endpoint();

      epoll_event event{};
      event.events = EPOLLIN;
      event.data.u64 = static_cast<std::uint64_t>(listeners.size());
      if (::epoll_ctl(epoll_file_descriptor.get(), EPOLL_CTL_ADD,
                      socket.native_handle(), &event) < 0) {
        throw_system_error("epoll_ctl(EPOLL_CTL_ADD)");
      }

      listeners.push_back(
          {.socket = std::move(socket), .local_endpoint = actual_endpoint});
      local_endpoints.push_back(std::move(actual_endpoint));
    }
  }

  FileDescriptor epoll_file_descriptor;
  std::size_t max_datagrams_per_listener_per_poll;
  std::vector<Listener> listeners;
  std::vector<net::Ipv4Endpoint> local_endpoints;
  ReceiverMetrics metrics;
};

EpollReceiver::EpollReceiver(const EpollReceiverConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

EpollReceiver::~EpollReceiver() = default;

std::span<const net::Ipv4Endpoint> EpollReceiver::local_endpoints() const noexcept {
  return impl_->local_endpoints;
}

ReceiverMetrics EpollReceiver::metrics() const noexcept {
  return impl_->metrics;
}

std::vector<ReceivedPacket> EpollReceiver::poll(
    std::chrono::milliseconds timeout) {
  if (timeout.count() < 0 ||
      timeout.count() > std::numeric_limits<int>::max()) {
    throw std::invalid_argument("epoll timeout must fit a non-negative int");
  }

  std::vector<epoll_event> events(impl_->listeners.size());
  const int ready_count =
      ::epoll_wait(impl_->epoll_file_descriptor.get(), events.data(),
                   static_cast<int>(events.size()),
                   static_cast<int>(timeout.count()));
  if (ready_count < 0) {
    if (errno == EINTR) {
      return {};
    }
    throw_system_error("epoll_wait");
  }
  if (ready_count > 0) {
    ++impl_->metrics.epoll_wakeups;
  }

  std::vector<ReceivedPacket> packets;
  packets.reserve(static_cast<std::size_t>(ready_count));

  for (int event_index = 0; event_index < ready_count; ++event_index) {
    const auto& event = events[static_cast<std::size_t>(event_index)];
    if ((event.events & EPOLLIN) == 0U) {
      continue;
    }

    const auto listener_index = static_cast<std::size_t>(event.data.u64);
    if (listener_index >= impl_->listeners.size()) {
      throw std::runtime_error("epoll returned an unknown listener");
    }
    auto& listener = impl_->listeners[listener_index];

    for (std::size_t received_count = 0U;
         received_count < impl_->max_datagrams_per_listener_per_poll;
         ++received_count) {
      auto datagram = listener.socket.try_receive();
      if (!datagram.has_value()) {
        break;
      }
      ++impl_->metrics.datagrams_received;
      const auto receive_timestamp_ns = current_receive_timestamp_ns();
      auto decoded_packet = protocol::decode_packet(datagram->bytes);
      if (!decoded_packet) {
        ++impl_->metrics.packets_rejected;
        continue;
      }
      ++impl_->metrics.packets_accepted;

      packets.push_back({
          .packet = std::move(*decoded_packet.packet),
          .source = std::move(datagram->source),
          .listener = listener.local_endpoint,
          .receive_timestamp_ns = receive_timestamp_ns,
      });
    }
  }

  return packets;
}

}  // namespace driveflow::runtime
