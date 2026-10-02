#include "pychron/persistence/ids.hpp"

#include <gtest/gtest.h>

#include <set>

using namespace pychron::persistence;

TEST(Uuid, ParseFormatRoundTrip) {
  const std::string text = "0192f6a3-7b1c-7def-8123-456789abcdef";
  const auto u = Uuid::parse(text);
  ASSERT_TRUE(u);
  EXPECT_EQ(u->str(), text);
  EXPECT_EQ(Uuid::parse("0192F6A3-7B1C-7DEF-8123-456789ABCDEF"), u);
  EXPECT_EQ(u->version(), 7);
}

TEST(Uuid, RejectsMalformedText) {
  EXPECT_FALSE(Uuid::parse(""));
  EXPECT_FALSE(Uuid::parse("0192f6a37b1c7def8123456789abcdef"));
  EXPECT_FALSE(Uuid::parse("0192f6a3-7b1c-7def-8123-456789abcdeg"));
  EXPECT_FALSE(Uuid::parse("0192f6a3+7b1c-7def-8123-456789abcdef"));
  EXPECT_TRUE(Uuid{}.is_nil());
}

TEST(Uuid, V7HasVersionVariantAndTimestamp) {
  UuidV7Generator gen([] { return std::int64_t{0x0192f6a37b1c}; }, 42);
  const Uuid u = gen.next();
  EXPECT_EQ(u.version(), 7);
  EXPECT_EQ(u.bytes()[8] & 0xc0, 0x80);  // RFC 9562 variant
  EXPECT_EQ(u.str().substr(0, 13), "0192f6a3-7b1c");
}

TEST(Uuid, V7IsMonotonicWhenTheClockStallsOrStepsBack) {
  std::int64_t now = 1'700'000'000'000;
  UuidV7Generator gen([&] { return now; }, 7);
  Uuid prev = gen.next();
  for (int i = 0; i < 10'000; ++i) {
    if (i == 5000) now -= 1000;  // NTP step backwards
    if (i % 1000 == 0) ++now;
    const Uuid next = gen.next();
    ASSERT_LT(prev, next) << "at " << i;
    prev = next;
  }
}

TEST(Uuid, ProcessGeneratorIsUniqueAndOrdered) {
  std::set<Uuid> seen;
  Uuid prev = Uuid::v7();
  for (int i = 0; i < 1000; ++i) {
    const Uuid u = Uuid::v7();
    EXPECT_LT(prev, u);
    EXPECT_TRUE(seen.insert(u).second);
    prev = u;
  }
}

TEST(UtcTime, IsoRoundTripWithMicroseconds) {
  const auto t = UtcTime::parse("2026-10-02T13:04:05.123456Z");
  ASSERT_TRUE(t);
  EXPECT_EQ(t->iso(), "2026-10-02T13:04:05.123456Z");
  EXPECT_EQ(UtcTime::parse(t->iso()), t);
}

TEST(UtcTime, ParsesShortFractionsAndNone) {
  EXPECT_EQ(UtcTime::parse("1970-01-01T00:00:00Z")->micros, 0);
  EXPECT_EQ(UtcTime::parse("1970-01-01T00:00:01.5Z")->micros, 1'500'000);
  EXPECT_EQ(UtcTime::parse("1970-01-01T00:00:00.123Z")->micros, 123'000);
  EXPECT_EQ(UtcTime::parse("1969-12-31T23:59:59.999999Z")->micros, -1);
  EXPECT_EQ(UtcTime{-1}.iso(), "1969-12-31T23:59:59.999999Z");
}

TEST(UtcTime, RejectsNonUtcAndMalformed) {
  EXPECT_FALSE(UtcTime::parse("2026-10-02T13:04:05"));
  EXPECT_FALSE(UtcTime::parse("2026-10-02T13:04:05+02:00"));
  EXPECT_FALSE(UtcTime::parse("2026-13-02T13:04:05Z"));
  EXPECT_FALSE(UtcTime::parse("2026-10-02 13:04:05Z"));
  EXPECT_FALSE(UtcTime::parse("2026-10-02T13:04:05.1234567Z"));
}
