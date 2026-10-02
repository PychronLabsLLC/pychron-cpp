#pragma once

// elctl exp: run and check experiment queues (experiment spec 10.3).
//
//   elctl [-c extraction_line.toml] [--sim] exp validate <experiment.toml> [options]
//   elctl [-c extraction_line.toml] [--sim] exp run <experiment.toml> [options] [--from N | --resume] [--dry-run]
//
// Options:
//   --lab <dir>             plans/, scripts/, conditionals/, identifiers.toml
//                           (default: the queue file's directory)
//   --data <dir>            records/, spool/, executor_state.json (default: <lab>/data)
//   --spectrometer <file>   spectrometer.toml (default: <lab>/spectrometer.toml if present)
//   --canvas <file>         canvas.toml (default: next to the -c config, if present)
//   --sim-speed <x>         with --sim: run simulated time x times faster than real time
//
// Interrupts while running: the first stops after the current run, the
// second cancels it, the third aborts.

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "cli.hpp"

namespace elctl {

struct ExpGlobals {
  std::filesystem::path config;
  bool sim = false;
};

int exp_command(const std::vector<std::string>& args, const ExpGlobals& globals, Io io);

// Incremented by main()'s SIGINT handler; `exp run` escalates on each.
std::atomic<int>& interrupt_count();

}  // namespace elctl
