#pragma once

// Runs a program to completion: argv (argv[0] looked up on PATH), its stdin
// from a string, extra environment variables on top of the inherited ones,
// and stdout and stderr captured together. A program that outlives the
// timeout is killed. posix_spawnp on POSIX, CreateProcessW on Windows; no
// shell is involved, so arguments are passed as they are. With stdout_file
// set, the child's stdout goes to that file (created or truncated, no size
// limit) and only stderr is captured.

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron {

struct ProcessSpec {
  std::vector<std::string> argv;
  std::string input;                                      // the child's stdin
  std::vector<std::pair<std::string, std::string>> env;   // set (or replaced) in the child
  std::chrono::milliseconds timeout{std::chrono::seconds(60)};
  std::optional<std::filesystem::path> stdout_file;       // stdout here instead of in `output`
};

struct ProcessResult {
  int exit_code = 0;   // 128 + signal when killed by a signal (POSIX)
  std::string output;  // stdout and stderr interleaved, at most the first 64 KiB (stderr only with stdout_file)
};

// Io when the program cannot be started; Timeout when it was killed for
// running too long. A non-zero exit code is not an error here.
Result<ProcessResult> run_process(const ProcessSpec& spec);

// The running program's directory (resolved through symlinks); empty when it
// cannot be found.
std::filesystem::path executable_dir();

}  // namespace pychron
