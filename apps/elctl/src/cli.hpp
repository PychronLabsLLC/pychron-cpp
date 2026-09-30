#pragma once

// elctl entry point, separated from main() so tests run the real commands
// against string streams.
//
//   elctl [-c <extraction_line.toml>] [--sim] <command> [args...]
//
// Exit codes: 0 ok, 1 the command failed, 2 usage error.

#include <iosfwd>
#include <string>
#include <vector>

namespace elctl {

struct Io {
  std::istream& in;
  std::ostream& out;
  std::ostream& err;
};

enum ExitCode : int { kOk = 0, kFailed = 1, kUsage = 2 };

// `args` excludes the program name.
int run(const std::vector<std::string>& args, Io io);

}  // namespace elctl
