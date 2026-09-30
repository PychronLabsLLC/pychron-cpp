#pragma once

// Shared sweep runner (spectrometer spec section 6). One Sweep serves every
// tuning job: it steps one axis from start to stop, settles, acquires one
// reading per step and records the selected detectors. Job code computes
// start/stop and fits the result; nothing here knows about peaks.

#include <functional>
#include <optional>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/spectrometer/params.hpp"
#include "pychron/systems/jobs/cancel.hpp"
#include "pychron/systems/jobs/progress.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"

namespace pychron::jobs {

using spectrometer::ParamId;
using spectrometer::Spectrometer;

// What a sweep steps. Magnet values are native positioner units; HV and CDD
// voltage in volts; source parameters in their own units; deflection in
// driver units (clamped by the Spectrometer, the clamped value is recorded).
struct SweepAxis {
  enum class Kind { Magnet, Hv, Param, CddVoltage, Deflection };

  Kind kind = Kind::Magnet;
  ParamId param{};      // Kind::Param
  DetectorId detector;  // Kind::CddVoltage, Kind::Deflection

  static SweepAxis magnet() { return {}; }
  static SweepAxis hv() { return {Kind::Hv, {}, {}}; }
  static SweepAxis source_param(ParamId id) { return {Kind::Param, id, {}}; }
  static SweepAxis cdd_voltage(DetectorId det) { return {Kind::CddVoltage, {}, std::move(det)}; }
  static SweepAxis deflection(DetectorId det) { return {Kind::Deflection, {}, std::move(det)}; }
};

struct SweepSpec {
  SweepAxis axis;
  double start = 0.0;
  double stop = 0.0;
  double step = 0.0;          // magnitude; direction comes from start -> stop
  Duration integration{};     // zero: keep the engine's current integration
  Duration settle{};          // after every set, before acquiring
  std::vector<DetectorId> record;  // empty: every detector
  bool bidirectional = false;      // forward, then back without repeating the turn point
};

// The x values a spec visits, in order. Config error for a non-finite or
// zero step, or more than Sweep::kMaxPoints steps.
Result<std::vector<double>> sweep_positions(const SweepSpec& spec);

class Sweep {
 public:
  static constexpr std::size_t kMaxPoints = 100000;

  struct Options {
    // Settle waits. Default: std::this_thread::sleep_for. Tests advance a
    // ManualClock instead.
    std::function<void(Duration)> sleep;
    // Protection for the move to the first magnet point; later steps are
    // small and never protect.
    spectrometer::ProtectPolicy first_move_protect = spectrometer::ProtectPolicy::Auto;
  };

  Sweep() = default;
  explicit Sweep(Options options) : options_(std::move(options)) {}

  // Cancelled error (points so far kept in points()) when `cancel` fires;
  // any hardware or acquisition error aborts the sweep likewise.
  Result<std::vector<SweepPoint>> run(Spectrometer& spec, const SweepSpec& sweep, Progress& progress,
                                      CancelToken& cancel);

  // Points measured by the last run(), including a failed or cancelled one.
  const std::vector<SweepPoint>& points() const noexcept { return points_; }

 private:
  Result<double> apply(Spectrometer& spec, const SweepAxis& axis, double x, bool first);
  void sleep(Duration d) const;

  Options options_;
  std::vector<SweepPoint> points_;
};

}  // namespace pychron::jobs
