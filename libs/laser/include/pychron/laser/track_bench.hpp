#pragma once

// The tracking bench (grain simulator design, section 7): the real dragonfly
// stack, on the Chromium simulator in a scratch directory of its own, against
// a simulated grain, scored on a manual clock. It touches no configured
// device. A scenario is a TOML file:
//
//   output_percent = 20        (0, 100]
//   duration = 30              (0, 3600] s
//   seeds = 20                 [1, 10000]: runs of each combination
//   seed = 1                   the first; then seed + 1, ...
//   poll_ms = 100              [10, 1000]
//   grain_offset_mm = [0, 0]
//
//   [grain]     keys of [<device>.sim.grain]
//   [pattern]   keys of a dragonfly pattern file; not kind, not duration
//   [camera]    settle_ms, frames_per_step, noise, px_per_mm, width, height
//
//   [sweep.pattern]            a list of values for any key of the table
//   aggressiveness = [0.5, 1, 2]
//   [sweep.grain]
//   [sweep.camera]
//
// Every key is optional; the top level's defaults are the values shown. There
// is no second reader of grain or pattern keys: each combination is written
// as a cameras.toml and a pattern file for the real loaders, so ranges and
// unknown keys are theirs.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/laser/track_score.hpp"

namespace pychron::laser {

struct TrackTables;  // track_bench.cpp: no toml in this header

struct TrackScenario {
  std::shared_ptr<const TrackTables> tables;
};

struct TrackOptions {
  std::optional<int> seeds;  // replaces the scenario's
  std::filesystem::path trace;   // a CSV of every poll; empty: none
  std::filesystem::path frames;  // every frame of one run; empty: none
  std::optional<std::uint64_t> frames_seed;  // which run of combination 0; empty: the scenario's seed
  bool check_only = false;                   // every check, no run: returns no combos
};

struct TrackCombo {
  std::string label;  // "pattern.aggressiveness=0.5 grain.jump_mm=0.3"; "" with no sweep
  TrackSummary summary;
  int failed = 0;  // runs that ended in a pattern error, scored as far as they got
  std::string first_error;
};

// Config error "<file_name>: <key>: <what>" for the scenario's own keys.
Result<TrackScenario> parse_track_scenario(std::string_view toml, const std::string& file_name);
Result<TrackScenario> load_track_scenario(const std::filesystem::path& file);

// All checks first: more than 10000 runs, a combination that does not load
// and a trace or frames directory that cannot be written are errors before
// any time is spent. Then every combination on the same seeds, best first by
// mean on_pct.
Result<std::vector<TrackCombo>> run_track(const TrackScenario& scenario, const TrackOptions& options);

// A header, then per combination its label, a `mean` and a `worst` line, and
// `failed N` with the first error when any failed.
std::string track_table(const std::vector<TrackCombo>& combos);

}  // namespace pychron::laser
