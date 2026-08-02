#include "driveflow/net/udp_socket.hpp"
#include "driveflow/protocol/packet.hpp"
#include "driveflow/protocol/sensor_payload.hpp"
#include "driveflow/simulator/sensor_simulator.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace driveflow::simulator {
namespace {

[[nodiscard]] std::vector<net::ReceivedDatagram> receive_datagrams(
    net::UdpSocket& receiver, std::size_t expected_count) {
  receiver.set_non_blocking();
  std::vector<net::ReceivedDatagram> datagrams;
  datagrams.reserve(expected_count);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (datagrams.size() < expected_count &&
         std::chrono::steady_clock::now() < deadline) {
    if (auto datagram = receiver.try_receive(); datagram.has_value()) {
      datagrams.push_back(std::move(*datagram));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }
  return datagrams;
}

[[nodiscard]] std::vector<std::uint64_t> receive_sequence_numbers(
    net::UdpSocket& receiver, std::size_t expected_count) {
  const auto datagrams = receive_datagrams(receiver, expected_count);
  std::vector<std::uint64_t> sequence_numbers;
  sequence_numbers.reserve(datagrams.size());
  for (const auto& datagram : datagrams) {
    const auto decoded = protocol::decode_packet(datagram.bytes);
    if (!decoded) {
      ADD_FAILURE() << "packet decode failed: "
                    << protocol::to_string(decoded.error);
      continue;
    }
    sequence_numbers.push_back(decoded.packet->header.sequence_number);
  }
  return sequence_numbers;
}

[[nodiscard]] FaultInjectionConfig periodic_drop(std::uint64_t every) {
  FaultInjectionConfig faults;
  faults.drop_every = every;
  return faults;
}

[[nodiscard]] FaultInjectionConfig periodic_duplicate(std::uint64_t every) {
  FaultInjectionConfig faults;
  faults.duplicate_every = every;
  return faults;
}

[[nodiscard]] FaultInjectionConfig periodic_reorder(std::uint64_t every) {
  FaultInjectionConfig faults;
  faults.reorder_every = every;
  return faults;
}

[[nodiscard]] FaultInjectionConfig periodic_delay(
    std::uint64_t every, std::chrono::milliseconds duration) {
  FaultInjectionConfig faults;
  faults.delay = PeriodicDelay{.every = every, .duration = duration};
  return faults;
}

[[nodiscard]] FaultInjectionConfig periodic_corruption(std::uint64_t every) {
  FaultInjectionConfig faults;
  faults.corrupt_every = every;
  return faults;
}

TEST(SimulatorConfigTest, ParsesExplicitImuConfiguration) {
  constexpr std::array<std::string_view, 10> arguments{
      "--sensor", "imu",          "--destination", "192.0.2.10", "--port",
      "9100",     "--rate-hz",    "125.5",          "--count",    "7",
  };

  const auto result = parse_simulator_config(arguments);

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.config->sensor_type, SensorType::kImu);
  EXPECT_EQ(result.config->destination.address, "192.0.2.10");
  EXPECT_EQ(result.config->destination.port, 9'100U);
  EXPECT_DOUBLE_EQ(result.config->rate_hz, 125.5);
  ASSERT_TRUE(result.config->packet_count.has_value());
  EXPECT_EQ(*result.config->packet_count, 7U);
  EXPECT_EQ(result.config->camera_extra_data_bytes, 0U);
  EXPECT_EQ(result.config->faults, FaultInjectionConfig{});
}

TEST(SimulatorConfigTest, AppliesSensorSpecificDefaults) {
  constexpr std::array<std::string_view, 2> gnss_arguments{"--sensor", "gnss"};
  constexpr std::array<std::string_view, 2> camera_arguments{"--sensor", "camera"};

  const auto gnss = parse_simulator_config(gnss_arguments);
  const auto camera = parse_simulator_config(camera_arguments);

  ASSERT_TRUE(gnss) << gnss.error;
  EXPECT_EQ(gnss.config->sensor_type, SensorType::kGnss);
  EXPECT_EQ(gnss.config->destination.address, "127.0.0.1");
  EXPECT_EQ(gnss.config->destination.port, 9'000U);
  EXPECT_DOUBLE_EQ(gnss.config->rate_hz, 10.0);
  EXPECT_FALSE(gnss.config->packet_count.has_value());

  ASSERT_TRUE(camera) << camera.error;
  EXPECT_EQ(camera.config->sensor_type, SensorType::kCameraMeta);
  EXPECT_EQ(camera.config->destination.address, "127.0.0.1");
  EXPECT_EQ(camera.config->destination.port, 9'000U);
  EXPECT_DOUBLE_EQ(camera.config->rate_hz, 30.0);
  EXPECT_FALSE(camera.config->packet_count.has_value());
}

TEST(SimulatorConfigTest, ParsesCameraExtraDataSize) {
  constexpr std::array<std::string_view, 4> arguments{
      "--sensor", "camera", "--camera-extra-bytes", "1024"};

  const auto result = parse_simulator_config(arguments);

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.config->sensor_type, SensorType::kCameraMeta);
  EXPECT_EQ(result.config->camera_extra_data_bytes, 1'024U);
}

TEST(SimulatorConfigTest, ParsesDeterministicFaultInjection) {
  constexpr std::array<std::string_view, 14> arguments{
      "--sensor",          "imu", "--drop-every",    "2",
      "--duplicate-every", "3",   "--reorder-every", "4",
      "--delay-every",     "5",   "--delay-ms",      "25",
      "--corrupt-every",   "6",
  };

  const auto result = parse_simulator_config(arguments);

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.config->faults.drop_every, 2U);
  EXPECT_EQ(result.config->faults.duplicate_every, 3U);
  EXPECT_EQ(result.config->faults.reorder_every, 4U);
  ASSERT_TRUE(result.config->faults.delay.has_value());
  EXPECT_EQ(result.config->faults.delay->every, 5U);
  EXPECT_EQ(result.config->faults.delay->duration,
            std::chrono::milliseconds{25});
  EXPECT_EQ(result.config->faults.corrupt_every, 6U);
}

TEST(SimulatorConfigTest, RejectsInvalidConfigurations) {
  const std::vector<std::vector<std::string_view>> invalid_arguments{
      {},
      {"--sensor"},
      {"--sensor", "lidar"},
      {"--unknown", "value"},
      {"--sensor", "imu", "--destination", "not-an-ipv4-address"},
      {"--sensor", "imu", "--port", "0"},
      {"--sensor", "imu", "--port", "65536"},
      {"--sensor", "imu", "--rate-hz", "0"},
      {"--sensor", "imu", "--rate-hz", "-1"},
      {"--sensor", "imu", "--rate-hz", "1000001"},
      {"--sensor", "imu", "--rate-hz", "nan"},
      {"--sensor", "imu", "--count", "0"},
      {"--sensor", "imu", "--camera-extra-bytes", "1"},
      {"--sensor", "camera", "--camera-extra-bytes", "65452"},
      {"--sensor", "imu", "--drop-every", "0"},
      {"--sensor", "imu", "--duplicate-every", "-1"},
      {"--sensor", "imu", "--reorder-every", "0"},
      {"--sensor", "imu", "--delay-every", "2"},
      {"--sensor", "imu", "--delay-ms", "10"},
      {"--sensor", "imu", "--delay-every", "2", "--delay-ms", "0"},
      {"--sensor", "imu", "--delay-every", "2", "--delay-ms", "60001"},
      {"--sensor", "imu", "--corrupt-every", "0"},
      {"--sensor", "imu", "--sensor", "gnss"},
  };

  for (std::size_t index = 0; index < invalid_arguments.size(); ++index) {
    SCOPED_TRACE(index);
    const auto result =
        parse_simulator_config(std::span<const std::string_view>{invalid_arguments[index]});
    EXPECT_FALSE(result);
    EXPECT_FALSE(result.error.empty());
  }
}

TEST(SensorSimulatorTest, SendsOrderedImuPacketsOverUdp) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1'000'000.0,
      .packet_count = 3U,
      .camera_extra_data_bytes = 0U,
      .faults = {},
  };

  const auto summary = run_simulator(config, [] { return false; });
  EXPECT_EQ(summary.packets_sent, 3U);

  std::uint64_t previous_timestamp = 0U;
  for (std::uint64_t expected_sequence = 1U; expected_sequence <= 3U;
       ++expected_sequence) {
    const auto datagram = receiver.receive();
    const auto decoded_packet = protocol::decode_packet(datagram.bytes);
    ASSERT_TRUE(decoded_packet) << protocol::to_string(decoded_packet.error);
    const auto& header = decoded_packet.packet->header;
    EXPECT_EQ(header.message_type, protocol::MessageType::kImu);
    EXPECT_EQ(header.sequence_number, expected_sequence);
    EXPECT_GT(header.source_timestamp_ns, 0U);
    EXPECT_GT(header.source_timestamp_ns, previous_timestamp);
    previous_timestamp = header.source_timestamp_ns;

    const auto decoded_payload =
        protocol::decode_sensor_payload(header.message_type, decoded_packet.packet->payload);
    ASSERT_TRUE(decoded_payload) << protocol::to_string(decoded_payload.error);
    ASSERT_TRUE(std::holds_alternative<protocol::ImuSample>(*decoded_payload.payload));
    const auto& sample = std::get<protocol::ImuSample>(*decoded_payload.payload);
    EXPECT_FLOAT_EQ(sample.linear_acceleration_mps2.z, 9.81F);
  }
}

TEST(SensorSimulatorTest, SendsGnssPayloadOverUdp) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kGnss,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1'000'000.0,
      .packet_count = 1U,
      .camera_extra_data_bytes = 0U,
      .faults = {},
  };

  const auto summary = run_simulator(config, [] { return false; });
  EXPECT_EQ(summary.packets_sent, 1U);
  const auto datagram = receiver.receive();
  const auto decoded_packet = protocol::decode_packet(datagram.bytes);
  ASSERT_TRUE(decoded_packet) << protocol::to_string(decoded_packet.error);
  EXPECT_EQ(decoded_packet.packet->header.message_type, protocol::MessageType::kGnss);

  const auto decoded_payload = protocol::decode_sensor_payload(
      decoded_packet.packet->header.message_type, decoded_packet.packet->payload);
  ASSERT_TRUE(decoded_payload) << protocol::to_string(decoded_payload.error);
  ASSERT_TRUE(std::holds_alternative<protocol::GnssFix>(*decoded_payload.payload));
  const auto& sample = std::get<protocol::GnssFix>(*decoded_payload.payload);
  EXPECT_DOUBLE_EQ(sample.latitude_deg, 31.2304);
  EXPECT_DOUBLE_EQ(sample.longitude_deg, 121.4737);
}

TEST(SensorSimulatorTest, SendsCameraMetadataAndConfiguredExtraDataOverUdp) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kCameraMeta,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1'000'000.0,
      .packet_count = 2U,
      .camera_extra_data_bytes = 4U,
      .faults = {},
  };

  const auto summary = run_simulator(config, [] { return false; });
  EXPECT_EQ(summary.packets_sent, 2U);
  for (std::uint64_t expected_sequence = 1U; expected_sequence <= 2U;
       ++expected_sequence) {
    const auto datagram = receiver.receive();
    const auto decoded_packet = protocol::decode_packet(datagram.bytes);
    ASSERT_TRUE(decoded_packet) << protocol::to_string(decoded_packet.error);
    EXPECT_EQ(decoded_packet.packet->header.message_type,
              protocol::MessageType::kCameraMeta);
    const auto decoded_payload = protocol::decode_sensor_payload(
        decoded_packet.packet->header.message_type, decoded_packet.packet->payload);
    ASSERT_TRUE(decoded_payload) << protocol::to_string(decoded_payload.error);
    ASSERT_TRUE(std::holds_alternative<protocol::CameraMeta>(*decoded_payload.payload));
    const auto& sample = std::get<protocol::CameraMeta>(*decoded_payload.payload);
    EXPECT_EQ(sample.frame_id, expected_sequence);
    EXPECT_EQ(sample.width, 1'920U);
    EXPECT_EQ(sample.height, 1'080U);
    EXPECT_EQ(sample.extra_data,
              (std::vector<std::uint8_t>{0U, 1U, 2U, 3U}));
  }
}

TEST(SensorSimulatorTest, DropsEveryNthGeneratedPacket) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1'000'000.0,
      .packet_count = 5U,
      .camera_extra_data_bytes = 0U,
      .faults = periodic_drop(2U),
  };

  const auto summary = run_simulator(config, [] { return false; });

  EXPECT_EQ(summary.packets_generated, 5U);
  EXPECT_EQ(summary.packets_sent, 3U);
  EXPECT_EQ(summary.packets_dropped, 2U);
  EXPECT_EQ(receive_sequence_numbers(receiver, 3U),
            (std::vector<std::uint64_t>{1U, 3U, 5U}));
}

TEST(SensorSimulatorTest, DuplicatesEveryNthGeneratedPacket) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1'000'000.0,
      .packet_count = 4U,
      .camera_extra_data_bytes = 0U,
      .faults = periodic_duplicate(2U),
  };

  const auto summary = run_simulator(config, [] { return false; });

  EXPECT_EQ(summary.packets_generated, 4U);
  EXPECT_EQ(summary.packets_sent, 6U);
  EXPECT_EQ(summary.duplicate_packets_sent, 2U);
  EXPECT_EQ(receive_sequence_numbers(receiver, 6U),
            (std::vector<std::uint64_t>{1U, 2U, 2U, 3U, 4U, 4U}));
}

TEST(SensorSimulatorTest, ReordersEveryNthPacketWithTheNextSendablePacket) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1'000'000.0,
      .packet_count = 5U,
      .camera_extra_data_bytes = 0U,
      .faults = periodic_reorder(2U),
  };

  const auto summary = run_simulator(config, [] { return false; });

  EXPECT_EQ(summary.packets_generated, 5U);
  EXPECT_EQ(summary.packets_sent, 5U);
  EXPECT_EQ(summary.reorder_events, 2U);
  EXPECT_EQ(receive_sequence_numbers(receiver, 5U),
            (std::vector<std::uint64_t>{1U, 3U, 2U, 5U, 4U}));
}

TEST(SensorSimulatorTest, FlushesAnUnpairedReorderPacketAtFiniteRunEnd) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1'000'000.0,
      .packet_count = 2U,
      .camera_extra_data_bytes = 0U,
      .faults = periodic_reorder(2U),
  };

  const auto summary = run_simulator(config, [] { return false; });

  EXPECT_EQ(summary.packets_sent, 2U);
  EXPECT_EQ(summary.reorder_events, 0U);
  EXPECT_EQ(receive_sequence_numbers(receiver, 2U),
            (std::vector<std::uint64_t>{1U, 2U}));
}

TEST(SensorSimulatorTest, DelaysEveryNthGeneratedPacket) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1'000'000.0,
      .packet_count = 2U,
      .camera_extra_data_bytes = 0U,
      .faults = periodic_delay(2U, std::chrono::milliseconds{20}),
  };

  const auto started_at = std::chrono::steady_clock::now();
  const auto summary = run_simulator(config, [] { return false; });
  const auto elapsed = std::chrono::steady_clock::now() - started_at;

  EXPECT_EQ(summary.packets_generated, 2U);
  EXPECT_EQ(summary.packets_sent, 2U);
  EXPECT_EQ(summary.delayed_packets, 1U);
  EXPECT_GE(elapsed, std::chrono::milliseconds{15});
  EXPECT_EQ(receive_sequence_numbers(receiver, 2U),
            (std::vector<std::uint64_t>{1U, 2U}));
}

TEST(SensorSimulatorTest, CorruptsPacketBytesAfterCrcEncoding) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1'000'000.0,
      .packet_count = 3U,
      .camera_extra_data_bytes = 0U,
      .faults = periodic_corruption(2U),
  };

  const auto summary = run_simulator(config, [] { return false; });
  const auto datagrams = receive_datagrams(receiver, 3U);

  ASSERT_EQ(datagrams.size(), 3U);
  EXPECT_TRUE(protocol::decode_packet(datagrams[0].bytes));
  const auto corrupted = protocol::decode_packet(datagrams[1].bytes);
  EXPECT_FALSE(corrupted);
  EXPECT_EQ(corrupted.error, protocol::DecodeError::kCrcMismatch);
  EXPECT_TRUE(protocol::decode_packet(datagrams[2].bytes));
  EXPECT_EQ(summary.packets_generated, 3U);
  EXPECT_EQ(summary.packets_sent, 3U);
  EXPECT_EQ(summary.corrupted_packets_sent, 1U);
}

TEST(SensorSimulatorTest, DropTakesPriorityOverAllOtherFaults) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1'000'000.0,
      .packet_count = 2U,
      .camera_extra_data_bytes = 0U,
      .faults = {
          .drop_every = 2U,
          .duplicate_every = 2U,
          .reorder_every = 2U,
          .delay = PeriodicDelay{
              .every = 2U,
              .duration = std::chrono::milliseconds{20},
          },
          .corrupt_every = 2U,
      },
  };

  const auto summary = run_simulator(config, [] { return false; });

  EXPECT_EQ(summary.packets_generated, 2U);
  EXPECT_EQ(summary.packets_sent, 1U);
  EXPECT_EQ(summary.packets_dropped, 1U);
  EXPECT_EQ(summary.duplicate_packets_sent, 0U);
  EXPECT_EQ(summary.reorder_events, 0U);
  EXPECT_EQ(summary.delayed_packets, 0U);
  EXPECT_EQ(summary.corrupted_packets_sent, 0U);
  EXPECT_EQ(receive_sequence_numbers(receiver, 1U),
            (std::vector<std::uint64_t>{1U}));
}

TEST(SensorSimulatorTest, RespondsToStopRequestDuringInjectedDelay) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1'000'000.0,
      .packet_count = 1U,
      .camera_extra_data_bytes = 0U,
      .faults = periodic_delay(1U, std::chrono::milliseconds{1'000}),
  };

  std::atomic_bool stop_requested{false};
  std::jthread stopper{[&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    stop_requested.store(true, std::memory_order_relaxed);
  }};

  const auto started_at = std::chrono::steady_clock::now();
  const auto summary = run_simulator(config, [&] {
    return stop_requested.load(std::memory_order_relaxed);
  });
  const auto elapsed = std::chrono::steady_clock::now() - started_at;

  EXPECT_EQ(summary.packets_generated, 1U);
  EXPECT_EQ(summary.packets_sent, 0U);
  EXPECT_EQ(summary.delayed_packets, 1U);
  EXPECT_LT(elapsed, std::chrono::milliseconds{250});
}

TEST(SensorSimulatorTest, RespondsToStopRequestDuringRateWait) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1.0,
      .packet_count = std::nullopt,
      .camera_extra_data_bytes = 0U,
      .faults = {},
  };

  std::atomic_bool stop_requested{false};
  std::jthread stopper{[&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    stop_requested.store(true, std::memory_order_relaxed);
  }};

  const auto started_at = std::chrono::steady_clock::now();
  const auto summary = run_simulator(config, [&] {
    return stop_requested.load(std::memory_order_relaxed);
  });
  const auto elapsed = std::chrono::steady_clock::now() - started_at;

  EXPECT_EQ(summary.packets_generated, 1U);
  EXPECT_EQ(summary.packets_sent, 1U);
  EXPECT_LT(elapsed, std::chrono::milliseconds{250});
}

TEST(SimulatorCliTest, ExposesUsageAndStableSensorNames) {
  EXPECT_EQ(to_string(SensorType::kImu), "imu");
  EXPECT_EQ(to_string(SensorType::kGnss), "gnss");
  EXPECT_EQ(to_string(SensorType::kCameraMeta), "camera");
  const auto usage = simulator_usage();
  EXPECT_NE(usage.find("--sensor <imu|gnss|camera>"), std::string_view::npos);
  EXPECT_NE(usage.find("--camera-extra-bytes <bytes>"), std::string_view::npos);
  EXPECT_NE(usage.find("--count <packets>"), std::string_view::npos);
  EXPECT_NE(usage.find("--drop-every <N>"), std::string_view::npos);
  EXPECT_NE(usage.find("--duplicate-every <N>"), std::string_view::npos);
  EXPECT_NE(usage.find("--reorder-every <N>"), std::string_view::npos);
  EXPECT_NE(usage.find("--delay-every <N>"), std::string_view::npos);
  EXPECT_NE(usage.find("--delay-ms <milliseconds>"), std::string_view::npos);
  EXPECT_NE(usage.find("--corrupt-every <N>"), std::string_view::npos);
}

TEST(SensorSimulatorTest, RejectsInvalidDirectConfiguration) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  const SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = receiver.local_endpoint(),
      .rate_hz = 0.0,
      .packet_count = 1U,
      .camera_extra_data_bytes = 0U,
      .faults = {},
  };

  EXPECT_THROW(
      (void)run_simulator(config, [] { return false; }),
      std::invalid_argument);
}

TEST(SensorSimulatorTest, RejectsInvalidDirectFaultConfiguration) {
  auto receiver =
      net::UdpSocket::bind_to({.address = "127.0.0.1", .port = 0U});
  SimulatorConfig config{
      .sensor_type = SensorType::kImu,
      .destination = receiver.local_endpoint(),
      .rate_hz = 1'000'000.0,
      .packet_count = 1U,
      .camera_extra_data_bytes = 0U,
      .faults = {},
  };

  config.faults.drop_every = 0U;
  EXPECT_THROW(
      (void)run_simulator(config, [] { return false; }),
      std::invalid_argument);

  config.faults = {};
  config.faults.delay = PeriodicDelay{
      .every = 0U,
      .duration = std::chrono::milliseconds{1},
  };
  EXPECT_THROW(
      (void)run_simulator(config, [] { return false; }),
      std::invalid_argument);

  config.faults.delay = PeriodicDelay{
      .every = 1U,
      .duration = std::chrono::milliseconds{60'001},
  };
  EXPECT_THROW(
      (void)run_simulator(config, [] { return false; }),
      std::invalid_argument);
}

}  // namespace
}  // namespace driveflow::simulator
