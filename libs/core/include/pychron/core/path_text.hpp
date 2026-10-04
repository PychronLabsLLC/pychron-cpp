#pragma once

// A path as UTF-8 text, and back.
//
// std::filesystem::path::string() gives the platform's narrow encoding: the
// ANSI code page on Windows, where it loses or mangles what the code page
// cannot hold and can throw. Text that leaves the process (a message, a
// settings file, a git argument or environment variable, the name an import
// source's ids are derived from) is UTF-8 on every platform, so that it means
// the same on every machine. On POSIX both functions copy bytes.

#include <filesystem>
#include <string>
#include <string_view>

namespace pychron {

// Never throws; "?" when the path cannot be converted.
inline std::string utf8(const std::filesystem::path& path) noexcept {
  try {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
  } catch (...) {
    return "?";
  }
}

// The path UTF-8 text names. Never throws; empty when the text is not UTF-8.
inline std::filesystem::path path_from_utf8(std::string_view text) noexcept {
  try {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
  } catch (...) {
    return {};
  }
}

}  // namespace pychron
