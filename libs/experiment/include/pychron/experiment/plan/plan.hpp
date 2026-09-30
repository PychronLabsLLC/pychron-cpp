#pragma once

// MeasurementPlan (spec 4.1): declarative measurement, data not code.
// A measurement is `main.cycles` repetitions of an ordered hop list;
// multicollect is the degenerate case of one hop and one cycle.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pychron/reduction/fits.hpp"

namespace pychron::experiment::plan {

struct PlanInfo {
  std::string name;
  std::string instrument_family;
  std::string description;
  std::vector<std::string> analysis_types;
  friend bool operator==(const PlanInfo&, const PlanInfo&) = default;
};

struct DetectorsSpec {
  std::string reference;             // default magnet-positioning detector for every hop
  std::vector<std::string> exclude;  // dropped from every hop
  friend bool operator==(const DetectorsSpec&, const DetectorsSpec&) = default;
};

struct Equilibration {
  std::string inlet, outlet;  // resolved valve names
  double time_s = 15;
  double inlet_delay_s = 0;
  bool close_inlet = true;
  friend bool operator==(const Equilibration&, const Equilibration&) = default;
};

struct Sniff {
  bool enabled = false;
  int counts = 0;
  double integration_s = 1;
  friend bool operator==(const Sniff&, const Sniff&) = default;
};

struct PeakCenter {
  bool before = false, after = false;
  std::string detector;  // empty = detectors.reference
  std::string isotope;
  std::string config = "default";
  friend bool operator==(const PeakCenter&, const PeakCenter&) = default;
};

struct Baseline {
  bool before = false, after = false;
  int counts = 0;
  std::optional<double> mass;
  std::string detector;  // empty = detectors.reference
  double settle_s = 0;
  double integration_s = 1;
  friend bool operator==(const Baseline&, const Baseline&) = default;
};

// Explicit magnet position for a hop whose isotopes do not include the reference detector.
struct HopPosition {
  std::string isotope;  // may be empty for a baseline hop at `mass`
  std::string detector;
  friend bool operator==(const HopPosition&, const HopPosition&) = default;
};

struct Hop {
  std::map<std::string, std::string> positions;  // isotope -> detector
  int counts = 0;
  double settle_s = 0;
  std::vector<std::string> protect;
  bool baseline = false;          // series kind `baseline`
  std::optional<double> mass;     // baseline hops only: position at this mass
  std::optional<HopPosition> position;
  friend bool operator==(const Hop&, const Hop&) = default;
};

enum class TimeZeroKind { OnInletClose, OnFirstCount, Offset };

struct TimeZero {
  TimeZeroKind kind = TimeZeroKind::OnInletClose;
  double offset_s = 0;  // Offset only
  friend bool operator==(const TimeZero&, const TimeZero&) = default;
};

struct MainSpec {
  int cycles = 1;
  double integration_s = 1;
  TimeZero time_zero;
  std::vector<Hop> hops;
  friend bool operator==(const MainSpec&, const MainSpec&) = default;
};

struct Fits {
  // Key "default" applies to every isotope/detector without its own entry.
  std::map<std::string, reduction::FitKind> signal{{"default", reduction::FitKind::Linear}};
  std::map<std::string, reduction::FitKind> baseline{{"default", reduction::FitKind::Average}};
  reduction::ErrorType error = reduction::ErrorType::Sem;
  reduction::OutlierSpec outliers;
  friend bool operator==(const Fits& a, const Fits& b) {
    return a.signal == b.signal && a.baseline == b.baseline && a.error == b.error &&
           a.outliers.enabled == b.outliers.enabled && a.outliers.iterations == b.outliers.iterations &&
           a.outliers.std_devs == b.outliers.std_devs;
  }
};

struct Truncation {
  std::string check;  // conditional expression, e.g. "Ar40 > 8e5"
  int start = 0;      // first count at which the check applies
  friend bool operator==(const Truncation&, const Truncation&) = default;
};

struct PlanConditionals {
  std::vector<std::string> include;  // conditional set references, kept verbatim
  std::vector<Truncation> truncations;
  friend bool operator==(const PlanConditionals&, const PlanConditionals&) = default;
};

struct Whiff {
  bool enabled = false;
  friend bool operator==(const Whiff&, const Whiff&) = default;
};

struct ExposeEntry {
  std::string path;   // dotted, indexed paths allowed: "main.hops[0].counts"
  std::string label;  // form label; defaults to path
  friend bool operator==(const ExposeEntry&, const ExposeEntry&) = default;
};

struct MeasurementPlan {
  PlanInfo info;
  DetectorsSpec detectors;
  Equilibration equilibration;
  Sniff sniff;
  PeakCenter peak_center;
  Baseline baseline;
  MainSpec main;
  Fits fits;
  PlanConditionals conditionals;
  Whiff whiff;
  std::vector<ExposeEntry> expose;
  std::optional<std::string> hook;  // "scripts/measurement_hooks/<name>.py"
  friend bool operator==(const MeasurementPlan&, const MeasurementPlan&) = default;
};

// Where the magnet puts which mass for a hop: an isotope (or explicit mass) on a detector.
struct HopTarget {
  std::string isotope;
  std::optional<double> mass;
  std::string detector;
  friend bool operator==(const HopTarget&, const HopTarget&) = default;
};

// The reference-detector / explicit-position rule. nullopt when the hop has
// neither (a validation error in a loaded plan).
std::optional<HopTarget> hop_target(const MeasurementPlan& plan, const Hop& hop);

// Target of the before/after baseline block: baseline.mass on baseline.detector
// (or the reference). nullopt without a mass: the baseline is taken where the
// magnet already is.
std::optional<HopTarget> baseline_target(const MeasurementPlan& plan);

struct ActiveDetector {
  std::string isotope, detector;
  friend bool operator==(const ActiveDetector&, const ActiveDetector&) = default;
};

// Detectors collecting during the hop: positions minus detectors.exclude, isotope order.
std::vector<ActiveDetector> active_detectors(const MeasurementPlan& plan, const Hop& hop);

// Fit for an isotope (signal) or detector (baseline), falling back to "default".
reduction::FitSpec signal_fit(const Fits& fits, const std::string& isotope);
reduction::FitSpec baseline_fit(const Fits& fits, const std::string& detector);

}  // namespace pychron::experiment::plan
