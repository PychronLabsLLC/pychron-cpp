#pragma once

// elctl exp: run and check experiment queues (experiment spec 10.3).
//
//   elctl [-c extraction_line.toml] [--sim] exp validate <experiment.toml> [options]
//   elctl [-c extraction_line.toml] [--sim] exp run <experiment.toml> [options] [--from N | --resume] [--dry-run]
//
// Options:
//   --lab <dir>             plans/, scripts/, conditionals/, identifiers.toml
//                           (default: the queue file's directory, or its parent when that is queues/)
//   --data <dir>            records/, spool/, executor_state.json (default: <lab>/data)
//   --spectrometer <file>   spectrometer.toml (default: <lab>/spectrometer.toml if present)
//   --canvas <file>         canvas.toml (default: next to the -c config, if present)
//   --sim-speed <x>         with --sim: run simulated time x times faster than real time
//   --db <url>              write the runs to this database (default: the install's; none: records are files)
//   --sim-save              a simulated session may write to the database; without it its runs go to a
//                           database only when that is the install's own file on this computer
//
// Interrupts while running: the first stops after the current run, the
// second cancels it, the third aborts.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "cli.hpp"

namespace pychron {
struct LinkReconnected;
namespace experiment::executor {
struct ExecutorHolding;
}
namespace experiment::measurement {
struct MeasurementGap;
}
}  // namespace pychron

namespace elctl {

struct ExpGlobals {
  std::filesystem::path config;
  bool sim = false;
  // From --install: used where the corresponding exp option is not given.
  std::filesystem::path lab, spectrometer, canvas, data;
  // The install's database url, password included (empty: none, or built without the store).
  std::string database;
  // Why the install's database url could not be made (database is then empty); `exp run` refuses to start.
  std::string database_error;
  // The install names a database and this build has no store to write it with: `exp run` says so.
  bool database_not_built = false;
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
// What `exp run` prints of a link's outage (spec 4.9), one line each:
//   holding: <reason> (<links>) until <time>     the time the hold limit ends the queue, UTC
//   gap in <block>: <n> s                        the readings stopped for n s
//   reconnected <transport> after <n> s          a link came back
// (`resumed` is printed as the queue leaves a hold.) `until` is the
// hold's end as wall time, on the line's clock.
std::string holding_line(const pychron::experiment::executor::ExecutorHolding& hold,
                         std::chrono::system_clock::time_point until);
std::string gap_line(const pychron::experiment::measurement::MeasurementGap& gap);
std::string reconnected_line(const pychron::LinkReconnected& link);

void answer_interrupts(int& handled, int count, const std::function<void(const std::string&)>& say,
                       const QueueRequests& queue);

}  // namespace elctl
