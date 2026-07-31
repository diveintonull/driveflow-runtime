#include "driveflow/runtime/runtime_config.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

namespace driveflow::runtime {
namespace {

TEST(RuntimeConfigTest, ParsesMultipleListenersAndRuntimeLimits) {
  constexpr std::array<std::string_view, 14> arguments{
      "--listen",         "127.0.0.1:9001",
      "--listen",         "0.0.0.0:9002",
      "--count",          "12",
      "--poll-timeout-ms", "25",
      "--max-drain",      "7",
      "--workers",        "3",
      "--queue-capacity", "128",
  };

  const auto result = parse_runtime_config(arguments);

  ASSERT_TRUE(result) << result.error;
  ASSERT_EQ(result.config->receiver.listen_endpoints.size(), 2U);
  EXPECT_EQ(result.config->receiver.listen_endpoints[0],
            (net::Ipv4Endpoint{.address = "127.0.0.1", .port = 9'001U}));
  EXPECT_EQ(result.config->receiver.listen_endpoints[1],
            (net::Ipv4Endpoint{.address = "0.0.0.0", .port = 9'002U}));
  EXPECT_EQ(result.config->receiver.max_datagrams_per_listener_per_poll, 7U);
  EXPECT_EQ(result.config->pipeline.worker_count, 3U);
  EXPECT_EQ(result.config->pipeline.queue_capacity, 128U);
  EXPECT_EQ(result.config->packet_count, 12U);
  EXPECT_EQ(result.config->poll_timeout, std::chrono::milliseconds{25});
}

TEST(RuntimeConfigTest, RejectsInvalidIpv4Listener) {
  constexpr std::array<std::string_view, 2> arguments{
      "--listen", "not-an-ip:9000"};

  const auto result = parse_runtime_config(arguments);

  EXPECT_FALSE(result);
  EXPECT_FALSE(result.error.empty());
}

TEST(RuntimeConfigTest, RejectsDuplicateSingletonOption) {
  constexpr std::array<std::string_view, 4> arguments{
      "--count", "1", "--count", "2"};

  const auto result = parse_runtime_config(arguments);

  EXPECT_FALSE(result);
  EXPECT_FALSE(result.error.empty());
}

}  // namespace
}  // namespace driveflow::runtime
