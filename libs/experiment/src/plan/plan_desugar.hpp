#pragma once

// v1 -> v2 rewrite (measurement plan v2 spec, section 4.1). Internal to the
// plan loader: resolve_plan reads a v1 file's block tables into `V1Tables`,
// then desugar_v1 rewrites them into one implicit gas, so every v1 file
// reaches the rules and the engine as gases of segments. Nothing outside the
// loader sees these tables.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/plan/plan.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"

namespace pychron::experiment::plan::detail {

struct V1Sniff {
  bool enabled = false;
  int counts = 0;
  double integration_s = 1;
};

struct V1PeakCenter {
  bool before = false, after = false;
  std::string detector;  // empty = detectors.reference
  std::string isotope;
  std::string config = "default";
  double min_signal = 0;  // the after_run step's minimum
};

struct V1Baseline {
  bool before = false, after = false;
  int counts = 0;
  std::optional<double> mass;
  std::string detector;  // empty = detectors.reference
  double settle_s = 0;
  double integration_s = 1;
};

// Explicit magnet position for a hop whose isotopes do not include the reference detector.
struct V1HopPosition {
  std::string isotope;  // may be empty for a baseline hop at `mass`
  std::string detector;
};

struct V1Hop {
  std::map<std::string, std::string> positions;  // isotope -> detector
  int counts = 0;
  double settle_s = 0;
  std::vector<std::string> protect;
  bool baseline = false;       // series kind `baseline`
  std::optional<double> mass;  // baseline hops only: position at this mass
  std::optional<V1HopPosition> position;
};

struct V1Main {
  int cycles = 1;
  double integration_s = 1;
  TimeZero time_zero;
  std::vector<V1Hop> hops;
};

struct V1Whiff {
  bool enabled = false;
  int counts = 0;
  double integration_s = 1;
  std::vector<WhiffCheck> checks;
};

// The v1 block tables of a plan file, as read.
struct V1Tables {
  V1Sniff sniff;
  V1PeakCenter peak_center;
  V1Baseline baseline;
  V1Main main;
  V1Whiff whiff;
};

// One implicit gas built from `v1` and the plan-level tables of `plan`
// (detectors, equilibration, fits), which must have parsed without problems.
// `catalog` (may be null) names the isotope configured on a protected
// detector that no hop collects. Fails (Config, "<path>: <why>") when a hop
// is left with nothing to collect.
Result<std::vector<Gas>> desugar_v1(const MeasurementPlan& plan, const V1Tables& v1,
                                    const ISpectrometerCatalog* catalog);

}  // namespace pychron::experiment::plan::detail
