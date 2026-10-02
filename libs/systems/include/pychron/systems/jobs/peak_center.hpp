#pragma once

// Peak center (spectrometer spec section 6): a pure fit plus a thin runner.
//
// find_peak_center() ports pychron's calculate_peak_center
// (pychron/core/stats/peak_detection.py): sort by x; the maximum must reach
// min_peak_height and sit at least 4 points from either end; walk down from
// the maximum on each side to the first point below (1 - percent/100) of it
// (the low side falls back to the second point, the high side to the last,
// as pychron does); the center is midway between the two; optionally the
// plateau test rejects a center whose 4 neighbouring points spread by more
// than 5 with a slope under 1. tests/data/scans holds scans whose expected
// results were produced by pychron's own code (generate.py).
//
// The runner (run_peak_center / peak_center_job) follows pychron's
// BasePeakCenter + IonOpticsManager:
//   1. the center comes from the field table (isotope on the detector, with
//      corrections) unless the config gives one;
//   2. per try: move to the scan's middle, read the reference intensity, move
//      to the start and wait (up to baseline_timeout) for the signal to fall
//      below intensity * (1 - percent/100) / 2; sweep start -> end; fit;
//   3. a failed try recenters on the scan's maximum; the next window is
//      `window` if the signal was strong (> 5 x min_peak_height), else
//      window * (1 + 0.1 i);
//   4. success: dac_offset is added; a center further than
//      peak_shift_threshold from the start is rejected; otherwise the center
//      is uncorrected (deflection, HV) into a table value, written with
//      Spectrometer::update_table (propagated to other detectors per config)
//      and the magnet is positioned on the isotope through the new table.
// A failed centering is a result with ok = false, not an error: errors are
// for hardware faults and cancellation. Every try's points are kept.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/systems/jobs/job.hpp"
#include "pychron/systems/jobs/sweep.hpp"

namespace pychron::jobs {

// ---- pure fit -----------------------------------------------------------------

struct PeakShape {
  double low_x = 0, center_x = 0, high_x = 0;
  double low_y = 0, center_y = 0, high_y = 0;  // center_y is the maximum (pychron)
  double max_x = 0, max_y = 0;
  // Peak-center width ratio at 95% (pychron calculate_resolution) and the
  // 5%/95% edge resolving powers (calculate_resolving_power); nullopt when
  // they cannot be computed.
  std::optional<double> resolution, resolving_power_low, resolving_power_high;
};

struct PeakFitOptions {
  double percent = 80;           // the edges are where the signal falls below (100 - percent)% of the max
  double min_peak_height = 1.0;
  bool test_peak_flat = true;
  bool ignore_max = false;       // skip the min_peak_height check
};

// Fails (ErrorKind::Config, pychron's messages) when no valid peak is found.
// Resolution and resolving powers are filled in when computable.
Result<PeakShape> find_peak_center(std::vector<std::pair<double, double>> points, const PeakFitOptions& options = {});

std::optional<double> peak_resolution(const std::vector<std::pair<double, double>>& points);
std::pair<std::optional<double>, std::optional<double>> peak_resolving_power(
    const std::vector<std::pair<double, double>>& points);

// ---- runner -------------------------------------------------------------------

struct PeakCenterConfig {
  std::string name = "default";
  DetectorId detector;                    // empty: the reference detector
  std::string isotope = "Ar40";
  std::vector<DetectorId> additional_detectors;  // fitted too, for display
  std::optional<double> center;           // native start value; default: from the field table
  double window = 0.015;                  // half-width, native units
  double step = 0.0005;
  double percent = 80;
  double min_peak_height = 1.0;
  bool test_peak_flat = true;
  int n_tries = 2;
  bool increase = true;                   // sweep direction
  Duration integration{};                 // zero: keep the current integration
  Duration settle{};                      // per step
  Duration baseline_timeout = std::chrono::seconds(10);
  double dac_offset = 0;                  // added to the fitted center
  double peak_shift_threshold = 0;        // 0: no limit
  bool update_table = true;
  bool propagate = true;                  // update the other detectors' columns (pychron update_others)

  friend bool operator==(const PeakCenterConfig&, const PeakCenterConfig&) = default;
};

// TOML: one table per named config, e.g.
//   [default]
//   isotope = "Ar40"
//   window = 0.015
//   step = 0.0005
//   integration_s = 1.0
// Unknown keys are errors.
Result<std::map<std::string, PeakCenterConfig>> parse_peak_center_configs(std::string_view toml_text,
                                                                         std::string_view file = "peak_center.toml");

struct DetectorPeak {
  DetectorId detector;
  std::optional<PeakShape> shape;
  std::string error;  // why there is no shape
};

struct PeakCenterTry {
  double start = 0, end = 0;
  std::vector<SweepPoint> points;
  bool baseline_reached = false;
};

struct PeakCenterResult {
  PeakCenterConfig config;
  bool ok = false;
  std::string message;                 // why it failed, or a summary
  DetectorId detector;
  std::string isotope;
  double initial_center = 0;           // native
  std::optional<double> center;        // native, fitted (+ dac_offset)
  std::optional<double> table_value;   // uncorrected center written to the table
  bool table_updated = false;
  std::optional<PeakShape> shape;      // reference detector
  std::vector<DetectorPeak> additional;
  std::vector<PeakCenterTry> tries;    // every try's sweep, kept on failure too
};

// Published by run_peak_center when a bus is given (spec: PeakCenterDone).
struct PeakCenterDone {
  PeakCenterResult result;
};

struct PeakCenterOptions {
  Sweep::Options sweep;                // sleep injection for tests
  SignalBus* bus = nullptr;
};

// Errors: Config (bad config, unknown detector/isotope), hardware errors,
// Cancelled. A failed fit is ok = false.
Result<PeakCenterResult> run_peak_center(Spectrometer& spectrometer, const PeakCenterConfig& config,
                                         Progress& progress, CancelToken& cancel,
                                         const PeakCenterOptions& options = {});

// Kind "peak_center": result is PeakCenterResult.
JobSpec peak_center_job(PeakCenterConfig config, PeakCenterOptions options = {});

}  // namespace pychron::jobs
