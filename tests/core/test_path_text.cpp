// Paths as UTF-8 text and back (path_text.hpp).

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "pychron/core/path_text.hpp"

using pychron::path_from_utf8;
using pychron::utf8;

TEST(PathText, RoundTripsNonAsciiNames) {
  // "dépôt/Ünïcode répo" as UTF-8 bytes.
  const std::string text = "d\xC3\xA9p\xC3\xB4t/\xC3\x9Cn\xC3\xAF" "code r\xC3\xA9po";
  const std::filesystem::path path = path_from_utf8(text);
  EXPECT_EQ(utf8(path), text);
  EXPECT_EQ(path, std::filesystem::path(std::u8string(text.begin(), text.end())));
  EXPECT_EQ(utf8(path / "a.json"), text + std::string(1, static_cast<char>(std::filesystem::path::preferred_separator)) + "a.json");
  EXPECT_EQ(utf8(std::filesystem::path()), "");
  EXPECT_TRUE(path_from_utf8("").empty());
}
