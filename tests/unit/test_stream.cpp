/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// local includes
#include <src/network.h>

namespace stream {
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
  std::optional<std::pair<std::uint16_t, std::string_view>> parse_control_packet(const ENetPacket &packet);
  std::optional<std::string_view> parse_input_data(std::string_view payload);
}  // namespace stream

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(0, 2, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ControlPacketTests, RejectsZeroLengthPacket) {
  net::packet_t packet {enet_packet_create(nullptr, 0, 0)};

  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(packet->data, nullptr);
  EXPECT_EQ(stream::parse_control_packet(*packet), std::nullopt);
}

TEST(ControlPacketTests, RejectsOneBytePacket) {
  const std::uint8_t data {0x06};
  net::packet_t packet {enet_packet_create(&data, sizeof(data), 0)};

  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(stream::parse_control_packet(*packet), std::nullopt);
}

TEST(ControlPacketTests, AcceptsTypeWithoutPayload) {
  const std::array<std::uint8_t, 2> data {0x06, 0x02};
  net::packet_t packet {enet_packet_create(data.data(), data.size(), 0)};

  ASSERT_NE(packet, nullptr);
  auto message = stream::parse_control_packet(*packet);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->first, 0x0206);
  EXPECT_TRUE(message->second.empty());
}

TEST(ControlPacketTests, AcceptsTypeAndPayload) {
  const std::array<std::uint8_t, 5> data {0x06, 0x02, 'a', 'b', 'c'};
  net::packet_t packet {enet_packet_create(data.data(), data.size(), 0)};

  ASSERT_NE(packet, nullptr);
  auto message = stream::parse_control_packet(*packet);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->first, 0x0206);
  EXPECT_EQ(message->second, "abc");
}

namespace {
  // Build an IDX_INPUT_DATA payload: a big-endian length prefix followed by `body_size` bytes.
  std::string make_input_payload(std::uint32_t declared_length, std::size_t body_size) {
    std::string payload {
      static_cast<char>(declared_length >> 24),
      static_cast<char>(declared_length >> 16),
      static_cast<char>(declared_length >> 8),
      static_cast<char>(declared_length),
    };
    for (std::size_t i = 0; i < body_size; ++i) {
      payload.push_back(static_cast<char>('a' + i % 26));
    }
    return payload;
  }
}  // namespace

TEST(InputDataParseTests, RejectsMalformedLengths) {
  EXPECT_EQ(stream::parse_input_data(std::string_view {}), std::nullopt);
  EXPECT_EQ(stream::parse_input_data(std::string_view {"\0\0\0", 3}), std::nullopt);
  EXPECT_EQ(stream::parse_input_data(std::string_view {"\0\0\0\0", 4}), std::nullopt);

  // Declared length below the 16-byte GCM tag.
  const auto too_short = make_input_payload(15, 32);
  EXPECT_EQ(stream::parse_input_data(too_short), std::nullopt);

  // Declared length far beyond the bytes actually present.
  const auto huge = make_input_payload(0xFFFFFFFF, 32);
  EXPECT_EQ(stream::parse_input_data(huge), std::nullopt);

  // Declared length is three bytes more than the body holds.
  const auto overrun = make_input_payload(32, 29);
  EXPECT_EQ(stream::parse_input_data(overrun), std::nullopt);
}

TEST(InputDataParseTests, AcceptsExactAndTrailing) {
  const auto exact = make_input_payload(16, 16);
  const auto exact_view = stream::parse_input_data(exact);
  ASSERT_TRUE(exact_view.has_value());
  EXPECT_EQ(*exact_view, std::string_view {exact}.substr(4, 16));

  const auto trailing = make_input_payload(32, 40);
  const auto trailing_view = stream::parse_input_data(trailing);
  ASSERT_TRUE(trailing_view.has_value());
  EXPECT_EQ(*trailing_view, std::string_view {trailing}.substr(4, 32));
}
