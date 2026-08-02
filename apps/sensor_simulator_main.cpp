#include "driveflow/simulator/sensor_simulator.hpp"

#include <csignal>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_stop_signal(int) { stop_requested = 1; }

[[nodiscard]] bool help_requested(int argc, char* argv[]) {
  for (int index = 1; index < argc; ++index) {
    if (std::string_view{argv[index]} == "--help") {
      return true;
    }
  }
  return false;
}

void install_stop_handlers() {
  if (std::signal(SIGINT, handle_stop_signal) == SIG_ERR ||
      std::signal(SIGTERM, handle_stop_signal) == SIG_ERR) {
    throw std::runtime_error("failed to install stop signal handlers");
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  if (help_requested(argc, argv)) {
    std::cout << driveflow::simulator::simulator_usage();
    return 0;
  }

  std::vector<std::string_view> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  const auto parsed = driveflow::simulator::parse_simulator_config(arguments);
  if (!parsed) {
    std::cerr << "configuration error: " << parsed.error << "\n\n"
              << driveflow::simulator::simulator_usage();
    return 1;
  }

  try {
    install_stop_handlers();
    const auto& config = *parsed.config;
    std::cout << "starting " << driveflow::simulator::to_string(config.sensor_type)
              << " simulator at " << config.rate_hz << " Hz -> "
              << config.destination.address << ':' << config.destination.port << '\n';
    const auto summary = driveflow::simulator::run_simulator(config, [] {
      return stop_requested != 0;
    });
    std::cout << "stopped packets_generated=" << summary.packets_generated
              << " packets_sent=" << summary.packets_sent
              << " packets_dropped=" << summary.packets_dropped
              << " duplicate_packets_sent="
              << summary.duplicate_packets_sent
              << " reorder_events=" << summary.reorder_events
              << " delayed_packets=" << summary.delayed_packets
              << " corrupted_packets_sent="
              << summary.corrupted_packets_sent << '\n';
  } catch (const std::exception& error) {
    std::cerr << "simulator failed: " << error.what() << '\n';
    return 2;
  }
  return 0;
}
