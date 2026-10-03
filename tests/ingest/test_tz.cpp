#include "pychron/ingest/tz.hpp"

#include <gtest/gtest.h>

namespace pychron::ingest {
namespace {

std::string iso(const Result<LocalToUtc>& r) { return r->utc.iso(); }

TEST(Tz, Plain) {
  const auto r = local_to_utc("2019-07-01 12:00:00", "America/Denver");
  ASSERT_TRUE(r);
  EXPECT_EQ(iso(r), "2019-07-01T18:00:00.000000Z");
  EXPECT_EQ(r->kind, LocalKind::Unique);
}

TEST(Tz, Ambiguous) {
  const auto r = local_to_utc("2019-11-03 01:30:00", "America/Denver");
  ASSERT_TRUE(r);
  EXPECT_EQ(iso(r), "2019-11-03T07:30:00.000000Z");
  EXPECT_EQ(r->kind, LocalKind::Ambiguous);
}

TEST(Tz, Nonexistent) {
  const auto r = local_to_utc("2019-03-10 02:30:00", "America/Denver");
  ASSERT_TRUE(r);
  EXPECT_EQ(iso(r), "2019-03-10T09:00:00.000000Z");
  EXPECT_EQ(r->kind, LocalKind::Nonexistent);
}

TEST(Tz, Micros) {
  const auto r = local_to_utc("2019-07-01T12:00:00.250000", "America/Denver");
  ASSERT_TRUE(r);
  EXPECT_EQ(iso(r), "2019-07-01T18:00:00.250000Z");
  EXPECT_EQ(local_to_utc("2019-07-01 12:00:00.5", "America/Denver")->utc.micros % 1000000, 500000);
}

TEST(Tz, UnknownZone) {
  EXPECT_FALSE(known_zone("Mars/Olympus"));
  EXPECT_TRUE(known_zone("America/Denver"));
  EXPECT_FALSE(local_to_utc("2019-07-01 12:00:00", "Mars/Olympus"));
}

TEST(Tz, Garbage) {
  EXPECT_FALSE(local_to_utc("yesterday", "America/Denver"));
  EXPECT_FALSE(local_to_utc("", "America/Denver"));
  EXPECT_FALSE(local_to_utc("2019-07-01 12:00:00Z", "America/Denver"));
  EXPECT_FALSE(local_to_utc("2019-07-01 12:00:00-06:00", "America/Denver"));
  EXPECT_FALSE(local_to_utc("2019-07-01 12:00:00.", "America/Denver"));
  EXPECT_FALSE(local_to_utc("2019-07-01 12:00:00.1234567", "America/Denver"));
  EXPECT_FALSE(local_to_utc("2019-02-30 12:00:00", "America/Denver"));
  EXPECT_FALSE(local_to_utc("2019-07-01 24:00:00", "America/Denver"));
  EXPECT_FALSE(local_to_utc("2019-07-01 12:00", "America/Denver"));
}

}  // namespace
}  // namespace pychron::ingest
