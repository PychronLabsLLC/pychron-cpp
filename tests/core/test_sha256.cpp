#include "pychron/core/sha256.hpp"

#include <gtest/gtest.h>

#include <string>

using pychron::Sha256;
using pychron::sha256;
using pychron::to_hex;

TEST(Sha256, KnownVectors) {
  EXPECT_EQ(to_hex(sha256(std::string_view{})), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(to_hex(sha256(std::string_view{"abc"})),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(to_hex(sha256(std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"})),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, StreamingMatchesOneShotAcrossBlockBoundaries) {
  std::string data;
  for (int i = 0; i < 1000; ++i) data.push_back(static_cast<char>(i * 7));
  const auto expected = sha256(std::string_view{data});
  for (std::size_t chunk : {1u, 3u, 55u, 56u, 63u, 64u, 65u, 127u, 999u}) {
    Sha256 h;
    for (std::size_t off = 0; off < data.size(); off += chunk)
      h.update(std::string_view{data}.substr(off, chunk));
    EXPECT_EQ(h.finish(), expected) << "chunk " << chunk;
  }
}

TEST(Sha256, MillionAs) {
  Sha256 h;
  const std::string block(1000, 'a');
  for (int i = 0; i < 1000; ++i) h.update(std::string_view{block});
  EXPECT_EQ(to_hex(h.finish()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}
