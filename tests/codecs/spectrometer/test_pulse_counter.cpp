#include "pychron/codecs/pulse_counter.hpp"

#include <gtest/gtest.h>

using namespace pychron;
namespace pc = pychron::codec::pulse_counter;

TEST(PulseCounterCodec, ReadCountsCommand) {
  EXPECT_EQ(pc::read_counts(), codec::Command::ascii("R\r", ReadSpec::until("\r")));
  EXPECT_TRUE(pc::is_read_request(pc::read_counts().tx));
  EXPECT_FALSE(pc::is_read_request(to_bytes("X\r")));
}

TEST(PulseCounterCodec, DecodesOneOrManyChannels) {
  EXPECT_EQ(*pc::decode_counts(1, to_bytes("1234\r")), (std::vector<std::uint64_t>{1234}));
  EXPECT_EQ(*pc::decode_counts(3, to_bytes("0,17,90000000000\r")),
            (std::vector<std::uint64_t>{0, 17, 90000000000ULL}));
}

TEST(PulseCounterCodec, EncodeRoundTrips) {
  std::vector<std::uint64_t> counts{5, 0, 12};
  EXPECT_EQ(pc::encode_counts(counts), to_bytes("5,0,12\r"));
  EXPECT_EQ(*pc::decode_counts(3, pc::encode_counts(counts)), counts);
}

TEST(PulseCounterCodec, RejectsBadReplies) {
  for (std::string_view reply : {"12", "\r", "1,\r", "-3\r", "1.5\r", "a\r", "1,2\r", "E OVERFLOW\r"}) {
    auto r = pc::decode_counts(1, to_bytes(reply));
    ASSERT_FALSE(r) << reply;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol) << reply;
    EXPECT_TRUE(r.error().device.empty());
  }
  EXPECT_EQ(pc::encode_error("OVERFLOW"), to_bytes("E OVERFLOW\r"));
}
