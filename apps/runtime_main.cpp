#include "driveflow/runtime/runtime.hpp"
#include "driveflow/runtime/runtime_config.hpp"

#include <cstddef>
#include <csignal>
#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_stop_signal(int) { stop_requested = 1; }

[[nodiscard]] bool install_signal_handlers() noexcept {
  return std::signal(SIGINT, handle_stop_signal) != SIG_ERR &&
         std::signal(SIGTERM, handle_stop_signal) != SIG_ERR;
}

}  // namespace

int main(int argc, char* argv[]) {
  using driveflow::runtime::Runtime;

  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << driveflow::runtime::runtime_usage();
    return 0;
  }

  std::vector<std::string_view> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  const auto parsed = driveflow::runtime::parse_runtime_config(arguments);
  if (!parsed) {
    std::cerr << "configuration error: " << parsed.error << '\n'
              << driveflow::runtime::runtime_usage();
    return 1;
  }
  if (!install_signal_handlers()) {
    std::cerr << "failed to install signal handlers\n";
    return 2;
  }

  try {
    Runtime runtime(*parsed.config);
    for (const auto& endpoint : runtime.local_endpoints()) {
      std::cout << "listening on " << endpoint.address << ':' << endpoint.port
                << '\n';
    }
    std::cout << "runtime started; press Ctrl+C to stop\n";

    const auto summary = runtime.run(
        [] { return stop_requested != 0; },
        [](const driveflow::runtime::ReceivedPacket&) {});

    std::cout << "runtime stopped"
              << " delivered=" << summary.packets_delivered
              << " datagrams=" << summary.receiver_metrics.datagrams_received
              << " accepted=" << summary.receiver_metrics.packets_accepted
              << " rejected=" << summary.receiver_metrics.packets_rejected
              << " epoll_wakeups=" << summary.receiver_metrics.epoll_wakeups
              << '\n';
  } catch (const std::exception& error) {
    std::cerr << "runtime failed: " << error.what() << '\n';
    return 3;
  }

  return 0;
}
