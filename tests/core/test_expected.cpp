#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "pychron/core/error.hpp"

using namespace pychron;

namespace {

Result<int> parse_positive(int v) {
  if (v <= 0) return fail(ErrorKind::Protocol, "not positive");
  return v;
}

Result<void> check(bool ok) {
  if (!ok) return fail(ErrorKind::Io, "check failed", "dev");
  return {};
}

}  // namespace

TEST(Expected, HoldsValue) {
  auto r = parse_positive(3);
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(static_cast<bool>(r));
  EXPECT_EQ(*r, 3);
  EXPECT_EQ(r.value(), 3);
  EXPECT_EQ(r.value_or(7), 3);
}

TEST(Expected, HoldsError) {
  auto r = parse_positive(-1);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(r.error().what, "not positive");
  EXPECT_EQ(r.value_or(7), 7);
}

TEST(Expected, VoidResult) {
  EXPECT_TRUE(check(true).has_value());
  auto bad = check(false);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().device, "dev");
}

TEST(Expected, MoveOnlyValueAndArrow) {
  Result<std::unique_ptr<std::string>> r = std::make_unique<std::string>("abc");
  ASSERT_TRUE(r);
  EXPECT_EQ((*r)->size(), 3u);
  auto owned = std::move(r).value();
  EXPECT_EQ(*owned, "abc");

  Result<std::string> s = std::string("hello");
  EXPECT_EQ(s->size(), 5u);
}
