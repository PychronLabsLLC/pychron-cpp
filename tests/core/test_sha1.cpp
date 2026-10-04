#include "pychron/core/sha1.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

#include "pychron/core/sha256.hpp"  // to_hex

using pychron::sha1;
using pychron::to_hex;

TEST(Sha1, KnownVectors) {
  EXPECT_EQ(to_hex(sha1(std::string_view{})), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(to_hex(sha1(std::string_view{"abc"})), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(to_hex(sha1(std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"})),
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

// More than one 64-byte block: the NIST 112-byte message (two blocks of
// message, a third of padding) and the million 'a's.
TEST(Sha1, MultiBlockVectors) {
  const std::string two_blocks = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
                                 "klmnopqrlmnopqrsmnopqrstnopqrstu";
  ASSERT_EQ(two_blocks.size(), 112u);
  EXPECT_EQ(to_hex(sha1(two_blocks)), "a49b2446a02c645bf419f995b67091253a04a259");
  EXPECT_EQ(to_hex(sha1(std::string(1000000, 'a'))), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

// Lengths on either side of where the padding needs a block of its own
// (55 and 56 bytes left over) and of a whole block.
TEST(Sha1, PaddingBoundaries) {
  const std::pair<std::size_t, const char*> cases[] = {
      {55, "c1c8bbdc22796e28c0e15163d20899b65621d65a"},
      {56, "c2db330f6083854c99d4b5bfb6e8f29f201be699"},
      {63, "03f09f5b158a7a8cdad920bddc29b81c18a551f5"},
      {64, "0098ba824b5c16427bd7a1122a5a442a25ec644d"},
      {65, "11655326c708d70319be2610e8a57d9a5b959d3b"},
      {119, "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56"},
      {120, "f34c1488385346a55709ba056ddd08280dd4c6d6"},
  };
  for (const auto& [length, hex] : cases) EXPECT_EQ(to_hex(sha1(std::string(length, 'a'))), hex) << length;
}
