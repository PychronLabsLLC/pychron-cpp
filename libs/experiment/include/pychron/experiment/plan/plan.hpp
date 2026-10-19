#pragma once

// MeasurementPlan (spec 4.1): declarative measurement, data not code.
// A plan is gases of segments of positions (measurement plan v2 spec); see
// `Gas`. A v1 file ([main] with its hops, [baseline], [peak_center], [sniff],
// [whiff]) is rewritten into that form by the loader: multicollect is the
// degenerate case of one position and one cycle.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
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

// A peak center step with a `min_signal` compares the mean of this many of
// the last signal readings of its label on its detector.
inline constexpr std::size_t kPeakCenterSignalReadings = 10;

// Now: a `time_zero` step only (v2 spec 3.3), time zero is the moment the step runs.
enum class TimeZeroKind { OnInletClose, OnFirstCount, Offset, Now };

struct TimeZero {
  TimeZeroKind kind = TimeZeroKind::OnInletClose;
  double offset_s = 0;  // Offset only
  friend bool operator==(const TimeZero&, const TimeZero&) = default;
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

// Whiff (conditionals spec L26): after the inlet opens, a whiff segment's
// readings on its position's detectors, then the first matching check decides:
// run_remainder (continue), pump (close the inlet, pump the gas away, end the
// measurement) or abort. No match = run_remainder.
struct WhiffCheck {
  std::string check;
  std::string action;  // run_remainder | pump | abort
  friend bool operator==(const WhiffCheck&, const WhiffCheck&) = default;
};

struct ExposeEntry {
  std::string path;   // dotted, indexed paths allowed: "main.hops[0].counts"
  std::string label;  // form label; defaults to path
  friend bool operator==(const ExposeEntry&, const ExposeEntry&) = default;
};

// ---- v2 (measurement plan v2 spec, sections 3 and 4.1) ---------------------------
//
// A plan is gases of segments of positions. A v1 file is rewritten into one
// implicit gas by the parser (desugar, spec 4.1), so `MeasurementPlan::gases`
// is always filled after resolve.

// When a segment runs within the run (spec 3.3, 5.1). Same `when`: file order.
enum class When { DuringExtraction, BeforeInlet, DuringEquilibration, AfterInlet, AfterMain, AfterRun };
std::string_view to_string(When w);
std::optional<When> parse_when(std::string_view s);

// A collection (signal .. whiff) or a step (peak_center .. time_zero).
enum class SegmentKind { Signal, Baseline, Sniff, Whiff, PeakCenter, Set, BlankBeam, Park, Delay, TimeZero };
std::string_view to_string(SegmentKind k);
std::optional<SegmentKind> parse_segment_kind(std::string_view s);
// True for the kinds that carry a step table instead of positions.
bool is_step(SegmentKind k);

enum class PositionKind { Signal, Baseline };

// One detector of a position (spec 3.2). `active = false` keeps the cup in the
// configuration (deflected or protected) without collecting it.
struct CollectEntry {
  std::string label, detector;
  bool active = true;
  std::optional<double> deflection;
  bool protect = false;
  friend bool operator==(const CollectEntry&, const CollectEntry&) = default;
};

// Positioning target, independent of what a position collects.
struct AtLabel {
  std::string label, detector;
  friend bool operator==(const AtLabel&, const AtLabel&) = default;
};
struct AtMass {
  double mass = 0;
  std::string detector;
  friend bool operator==(const AtMass&, const AtMass&) = default;
};
struct AtDac {
  double dac = 0;
  friend bool operator==(const AtDac&, const AtDac&) = default;
};
struct AtHv {
  double hv = 0;
  std::string detector;
  friend bool operator==(const AtHv&, const AtHv&) = default;
};
using At = std::variant<AtLabel, AtMass, AtDac, AtHv>;

struct Position {
  std::vector<CollectEntry> collect;
  std::optional<At> at;  // nullopt: the reference detector's label in `collect`
  PositionKind kind = PositionKind::Signal;
  int counts = 0;
  double settle_s = 0;
  std::optional<double> integration_s;  // nullopt: the segment's
  std::string optics, table;            // empty: none / the segment's table
  friend bool operator==(const Position&, const Position&) = default;
};

// One fit override; unset members fall back to the gas's (or plan's) fits.
struct FitOverride {
  std::optional<reduction::FitKind> fit;
  std::optional<reduction::ErrorType> error;
  std::optional<reduction::OutlierSpec> outliers;
  friend bool operator==(const FitOverride& a, const FitOverride& b) {
    const auto same_outliers = [](const std::optional<reduction::OutlierSpec>& x,
                                  const std::optional<reduction::OutlierSpec>& y) {
      if (x.has_value() != y.has_value()) return false;
      return !x || (x->enabled == y->enabled && x->iterations == y->iterations && x->std_devs == y->std_devs);
    };
    return a.fit == b.fit && a.error == b.error && same_outliers(a.outliers, b.outliers);
  }
};
// Keyed `label@detector`, `label` (every detector of that label) or `default`.
using FitOverrides = std::map<std::string, FitOverride>;

// ---- step tables (spec 3.3) ----

struct PeakCenterStep {
  std::string label;
  std::vector<std::string> detectors;  // the job runs once per detector
  std::vector<std::string> extra_detectors;
  std::string config = "default";
  std::string write_table;  // empty: no per-run field table
  // Skipped when the mean of the last kPeakCenterSignalReadings signal
  // readings of `label` on its detector is below this. 0: no minimum.
  double min_signal = 0;
  friend bool operator==(const PeakCenterStep&, const PeakCenterStep&) = default;
};

struct SetStep {
  std::map<std::string, double> deflection, gain, source;
  std::optional<double> hv, cdd_voltage, integration_s;
  friend bool operator==(const SetStep&, const SetStep&) = default;
};

struct ParkStep {
  std::string label;
  std::optional<double> mass;
  std::string detector;
  friend bool operator==(const ParkStep&, const ParkStep&) = default;
};

// A segment: positions cycled `cycles` times, or one step. A `time_zero`
// step uses `time_zero` with kind Now or Offset. `table` is the default
// field table of its positions.
struct Segment {
  SegmentKind kind = SegmentKind::Signal;
  When when = When::AfterInlet;
  int cycles = 1;
  double integration_s = 1;
  std::optional<TimeZero> time_zero;
  std::string table;
  std::vector<Position> positions;
  FitOverrides fits;
  std::vector<Truncation> truncations;
  std::vector<WhiffCheck> checks;  // whiff only
  std::optional<PeakCenterStep> peak_center;
  std::optional<SetStep> set;
  std::optional<bool> blank_beam;
  std::optional<ParkStep> park;
  std::optional<double> delay_s;
  friend bool operator==(const Segment&, const Segment&) = default;
};

// What a gas's prepare script is handed (spec 11.2): a TOML value.
using PrepareValue = std::variant<bool, std::int64_t, double, std::string>;

// A gas's prepare (spec 11.2): an extraction script run before its inlet,
// with variables over the run's own.
struct Prepare {
  std::string script;
  std::map<std::string, PrepareValue> variables;
  friend bool operator==(const Prepare&, const Prepare&) = default;
};

// A gas's pump-out (spec 11.3): after its last segment, `valve` open for
// `time_s`, then closed.
struct PumpOut {
  std::string valve;
  double time_s = 0;
  friend bool operator==(const PumpOut&, const PumpOut&) = default;
};

// A gas (spec 3.4). `implicit`: built from a plan without `[[gas]]`.
struct Gas {
  std::string name;
  Equilibration equilibration;
  bool source_dark_phase = false;
  std::optional<TimeZero> time_zero;
  std::optional<Fits> fits;
  std::vector<Segment> segments;
  bool implicit = false;
  std::optional<Prepare> prepare;
  std::optional<PumpOut> pump_out;
  friend bool operator==(const Gas&, const Gas&) = default;
};

// [baseline.modifier.<detector>] (spec 3.5): `variable` an expression over
// the run's intercepts, `model` a CSV of (variable, baseline[, error]) points
// to fit by weighted least squares (a path relative to the plan library) or
// an expression in `x`, `error` the name of an error model (optional). The
// record keeps what it gives; the reduction applies it.
struct BaselineModifier {
  std::string variable, model, error;
  friend bool operator==(const BaselineModifier&, const BaselineModifier&) = default;
};

// A modifier's model is a CSV file, not an expression.
bool is_model_file(std::string_view model);

struct MeasurementPlan {
  PlanInfo info;
  DetectorsSpec detectors;
  Equilibration equilibration;
  Fits fits;
  PlanConditionals conditionals;
  std::vector<ExposeEntry> expose;
  std::optional<std::string> hook;  // "scripts/measurement_hooks/<name>.py"
  // Always filled after resolve; a v1 file is one implicit gas desugared from
  // its [main], [baseline], [peak_center], [sniff] and [whiff] tables.
  std::vector<Gas> gases;
  std::map<std::string, BaselineModifier> baseline_modifiers;  // by detector
  friend bool operator==(const MeasurementPlan&, const MeasurementPlan&) = default;
};

// Where a position puts the magnet (spec 3.2): its `at`, else the label of
// its entry on the reference detector (or on one of that detector's modes,
// "H2(CDD)" for "H2"). The reference is the plan's; when the plan names none,
// `spectrometer_reference` (spectrometer.toml's reference_detector), and a
// position that does not collect on it is positioned by its first active
// entry. nullopt when no reference is known at all and the position has no
// `at` (the position is collected wherever the magnet is), and when the
// plan's own reference is not collected and `at` is missing.
std::optional<At> position_target(const MeasurementPlan& plan, const Position& position,
                                  std::string_view spectrometer_reference = {});

// The entries a position collects: its active ones, in file order.
std::vector<CollectEntry> collecting(const Position& position);

// The gas's segments of one `when`, as indexes, in file order; for
// after_inlet its whiff segments first (spec 5.1).
std::vector<std::size_t> segments_at(const Gas& gas, When when);

// The gas's first after_inlet signal segment (v1's [main]); nullptr when it
// has none.
const Segment* main_segment(const Gas& gas);

// The fit of one series (spec 6.1): the segment's `fits` (when `segment` is
// given) over the gas's (the plan's when the gas has none). Within the gas's
// or plan's tables: `label@detector`, then `label` (signal, sniff, whiff) or
// `detector` (baseline), then `default`; a baseline with none of them is
// averaged. Within the segment's overrides: `label@detector`, `label` or
// `detector`, `default`; each member it sets replaces the gas's. `kind` is
// the series kind ("signal", "baseline", ...): "baseline" reads the baseline
// table, every other kind the signal table.
reduction::FitSpec fit_for(const MeasurementPlan& plan, const Gas& gas, const Segment* segment, std::string_view kind,
                           std::string_view label, std::string_view detector);

// Fit for an isotope (signal) or detector (baseline), falling back to "default".
reduction::FitSpec signal_fit(const Fits& fits, const std::string& isotope);
reduction::FitSpec baseline_fit(const Fits& fits, const std::string& detector);

}  // namespace pychron::experiment::plan
