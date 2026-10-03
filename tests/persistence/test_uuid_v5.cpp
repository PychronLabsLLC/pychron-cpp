#include <gtest/gtest.h>

#include "pychron/persistence/ids.hpp"

using pychron::persistence::Uuid;

TEST(UuidV5, DnsVector) {
  const auto dns = Uuid::parse("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
  ASSERT_TRUE(dns.has_value());
  EXPECT_EQ(Uuid::v5(*dns, "www.example.com").str(), "2ed6657d-e927-568b-95e1-2665a8aea6a2");
}

TEST(UuidV5, VersionAndDeterminism) {
  const auto ns = Uuid::parse("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
  ASSERT_TRUE(ns.has_value());
  const Uuid a = Uuid::v5(*ns, "alpha");
  EXPECT_EQ(a.version(), 5);
  EXPECT_EQ(a.bytes()[8] >> 6, 2);  // RFC 4122 variant bits 10
  EXPECT_EQ(a, Uuid::v5(*ns, "alpha"));
  EXPECT_NE(a, Uuid::v5(*ns, "beta"));
}
