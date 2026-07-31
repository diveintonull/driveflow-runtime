#include "driveflow/runtime/runtime_config.hpp"

#include <arpa/inet.h>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace driveflow::runtime {
namespace {

[[nodiscard]] ParseRuntimeConfigResult failure(std::string message) {
  return {.config = std::nullopt, .error = std::move(message)};
}

[[nodiscard]] std::optional<std::uint64_t> parse_positive_integer(
    std::string_view text) {
  std::uint64_t value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} ||
      end != text.data() + text.size() || value == 0U) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] std::optional<net::Ipv4Endpoint> parse_endpoint(
    std::string_view text) {
  const auto separator = text.rfind(':');
  if (separator == std::string_view::npos || separator == 0U ||
      separator + 1U == text.size()) {
    return std::nullopt;
  }

  const auto port = parse_positive_integer(text.substr(separator + 1U));
  if (!port.has_value() || *port > 65'535U) {
    return std::nullopt;
  }
  std::string address(text.substr(0U, separator));
  in_addr binary_address{};
  if (::inet_pton(AF_INET, address.c_str(), &binary_address) != 1) {
    return std::nullopt;
  }
  return net::Ipv4Endpoint{
      .address = std::move(address),
      .port = static_cast<std::uint16_t>(*port),
  };
}

}  // namespace

ParseRuntimeConfigResult parse_runtime_config(
    std::span<const std::string_view> arguments) {
  RuntimeConfig config;
  bool has_explicit_listener = false;
  std::unordered_set<std::string_view> seen_singleton_options;

  for (std::size_t index = 0U; index < arguments.size(); ++index) {
    const auto option = arguments[index];
    if (index + 1U >= arguments.size()) {
      return failure("missing value for " + std::string(option));
    }
    const auto value = arguments[++index];

    if (option == "--listen") {
      const auto endpoint = parse_endpoint(value);
      if (!endpoint.has_value()) {
        return failure("--listen must use IPv4:port with port between 1 and 65535");
      }
      if (!has_explicit_listener) {
        config.receiver.listen_endpoints.clear();
        has_explicit_listener = true;
      }
      config.receiver.listen_endpoints.push_back(*endpoint);
      continue;
    }
    if (!seen_singleton_options.insert(option).second) {
      return failure("duplicate option: " + std::string(option));
    }

    const auto number = parse_positive_integer(value);
    if (!number.has_value()) {
      return failure(std::string(option) + " must be a positive integer");
    }

    if (option == "--count") {
      config.packet_count = *number;
    } else if (option == "--poll-timeout-ms") {
      if (*number > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return failure("--poll-timeout-ms is too large");
      }
      config.poll_timeout =
          std::chrono::milliseconds{static_cast<int>(*number)};
    } else if (option == "--max-drain") {
      if (*number >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return failure("--max-drain is too large");
      }
      config.receiver.max_datagrams_per_listener_per_poll =
          static_cast<std::size_t>(*number);
    } else {
      return failure("unknown option: " + std::string(option));
    }
  }

  return {.config = std::move(config), .error = {}};
}

std::string_view runtime_usage() noexcept {
  return
      "usage: driveflow_runtime [options]\n"
      "  --listen <IPv4:port>       listen endpoint; may be repeated\n"
      "  --count <packets>           stop after delivering this many packets\n"
      "  --poll-timeout-ms <ms>      epoll wait timeout (default: 100)\n"
      "  --max-drain <datagrams>     per-listener drain limit (default: 64)\n"
      "default listener: 0.0.0.0:9000\n";
}

}  // namespace driveflow::runtime
