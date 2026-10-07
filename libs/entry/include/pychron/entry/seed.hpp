#pragma once

// What setup puts in a new store (install defaults design, 2026-10-07): the
// project and samples that blanks, airs and cocktails are of, their special
// identifiers, and reactors with their production ratios. A seed only adds:
// a row or a reactor that is already there is kept exactly as it is.
//
// seed.toml:
//   project = "references"
//   [[samples]]
//   identifier = "bu"  analysis_type = "blank_unknown"  sample = "blank_unknown"  material = "blank"
//   [reactors.Triga]
//   K4039 = [0.00873, 0.00017]     # [value, error], the keys of production_keys()

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::entry {

struct SeedSample {
  std::string identifier, analysis_type, sample, material;
  friend bool operator==(const SeedSample&, const SeedSample&) = default;
};

struct Seed {
  std::string project;
  std::vector<SeedSample> samples;  // in the file's order
  // By name; each value's reactor is its name and its ratios are in production_keys() order.
  std::map<std::string, persistence::ProductionValue> reactors;
};

// Config error (naming `name` and the offender) for: text that is not TOML,
// an unknown key, no project or one valid_project_name refuses, a sample
// without all four fields, an analysis type that has no reference sample
// (unknown, pause, degas, or no type at all), an identifier given twice, a
// ratio key outside production_keys(), a ratio that is not two finite numbers.
Result<Seed> parse_seed(std::string_view toml, std::string_view name = "seed.toml");

struct SeedReport {
  int projects = 0, materials = 0, samples = 0, identifiers = 0, reactors = 0;  // made by this call
  // What was already there, by name: "project references", "material blank",
  // "sample air", "identifier bu", "reactor Triga".
  std::vector<std::string> kept;
  bool changed() const { return projects + materials + samples + identifiers + reactors > 0; }
};

// Puts what `seed` names and the store does not have into the store: the
// project (the one of that name without a principal investigator, else any
// of that name, else a new one without), the materials, the samples, the
// special identifiers (each of its sample; one that exists is kept with
// whatever sample it has or has not), then the reactors in the document
// "reactors.json" (one new revision, when a reactor is added).
//
// The reactors come last: a "reactors.json" that is not a JSON object is a
// Config error that leaves the document alone, after the catalog rows have
// been written. With `dry_run` nothing is written and the report says what
// would be made.
Result<SeedReport> apply_seed(persistence::IStore& store, const Seed& seed, const persistence::Actor& actor,
                              bool dry_run = false);

// One line: "seeded 1 project, 3 materials, 8 samples, 8 identifiers, 1 reactor"
// (only what was made), or "seed: nothing to add (21 already there)".
std::string describe(const SeedReport& report);

}  // namespace pychron::entry
