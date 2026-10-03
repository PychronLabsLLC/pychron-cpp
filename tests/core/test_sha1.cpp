#include "pychron/core/sha1.hpp"

#include <gtest/gtest.h>

#include <string>

#include "pychron/core/sha256.hpp"  // to_hex

using pychron::sha1;
using pychron::to_hex;

TEST(Sha1, KnownVectors) {
  EXPECT_EQ(to_hex(sha1(std::string_view{})), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(to_hex(sha1(std::string_view{"abc"})), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(to_hex(sha1(std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"})),
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}
