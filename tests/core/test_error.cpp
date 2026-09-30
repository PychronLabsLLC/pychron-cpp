#include <gtest/gtest.h>

#include <set>
#include <string>

#include "pychron/core/error.hpp"

using namespace pychron;

// Every ErrorKind round-trips through fail()/Result and has a distinct name.
TEST(Error, EveryKindHasDistinctNameAndPropagates) {
  const ErrorKind kinds[] = {ErrorKind::Timeout,      ErrorKind::Io,        ErrorKind::Protocol, ErrorKind::Config,
                             ErrorKind::NotConnected, ErrorKind::Interlock, ErrorKind::Cancelled};
  std::set<std::string> names;
  for (auto k : kinds) {
    Result<int> r = fail(k, "boom", "dev1");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, k);
    const auto name = std::string(to_string(k));
    EXPECT_NE(name, "unknown");
    names.insert(name);
  }
  EXPECT_EQ(names.size(), std::size(kinds));
}

TEST(Error, KindNames) {
  EXPECT_EQ(to_string(ErrorKind::Timeout), "timeout");
  EXPECT_EQ(to_string(ErrorKind::Io), "io");
  EXPECT_EQ(to_string(ErrorKind::Protocol), "protocol");
  EXPECT_EQ(to_string(ErrorKind::Config), "config");
  EXPECT_EQ(to_string(ErrorKind::NotConnected), "not_connected");
  EXPECT_EQ(to_string(ErrorKind::Interlock), "interlock");
  EXPECT_EQ(to_string(ErrorKind::Cancelled), "cancelled");
}

TEST(Error, FormatsWithAndWithoutDevice) {
  EXPECT_EQ(to_string(Error{ErrorKind::Timeout, "no reply after 500 ms", "ig1"}), "ig1: timeout: no reply after 500 ms");
  EXPECT_EQ(to_string(Error{ErrorKind::Interlock, "B is open", ""}), "interlock: B is open");
}

TEST(Error, Equality) {
  EXPECT_EQ((Error{ErrorKind::Io, "x", "d"}), (Error{ErrorKind::Io, "x", "d"}));
  EXPECT_NE((Error{ErrorKind::Io, "x", "d"}), (Error{ErrorKind::Timeout, "x", "d"}));
}
