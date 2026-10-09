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
#include <functional>
#include <string>
#include <vector>

#include "cli.hpp"

namespace elctl {

struct ExpGlobals {
  std::filesystem::path config;
  bool sim = false;
  // From --install: used where the corresponding exp option is not given.
  std::filesystem::path lab, spectrometer, canvas, data;
};

int exp_command(const std::vector<std::string>& args, const ExpGlobals& globals, Io io);

// Incremented by main()'s SIGINT handler; `exp run` escalates on each.
std::atomic<int>& interrupt_count();

// What an interrupt asks of the running queue, weakest first.
struct QueueRequests {
  std::function<void()> stop, cancel, abort;
};

// Answers the interrupts numbered from `handled` up to `count`, and leaves
// `handled` at `count`. Each is said; only the strongest is asked of the
// queue. Two that arrive together are one request to cancel: a stop asked
// for first is honoured at once by a queue between runs, which has then
// ended as stopped before the cancel reaches it.
void answer_interrupts(int& handled, int count, const std::function<void(const std::string&)>& say,
                       const QueueRequests& queue);

}  // namespace elctl
