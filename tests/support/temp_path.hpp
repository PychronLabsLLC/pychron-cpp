// A name under the system's temporary directory that no other test has: not
// another case of this process, and not another process. ctest runs one
// process per test case, many at once, and CI runs several build trees on one
// machine, so a name made of the clock and a counter of the process is not
// enough: two processes started together read the same clock and both count
// from zero.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

namespace pychron::testing {

// `<temp>/<prefix><token><suffix>`, where the token is 64 bits from the
// operating system and a count of the calls this process has made. Nothing is
// created: the caller makes the directory or the file, and removes it.
[[nodiscard]] inline std::filesystem::path unique_temp_path(std::string_view prefix, std::string_view suffix = {}) {
  static std::atomic<unsigned> serial{0};
  std::random_device rd;
  const std::uint64_t token = (static_cast<std::uint64_t>(rd()) << 32) | rd();
  std::ostringstream name;
  name << prefix << std::hex << token << '_' << std::dec << serial.fetch_add(1) << suffix;
  return std::filesystem::temp_directory_path() / name.str();
}

}  // namespace pychron::testing
