#pragma once

// Vendor-blind magnet move protocol (spectrometer spec section 4.4):
//
//   1. protect(det, true) for every planned detector, then blank(true)
//   2. optional AF demagnetization: a decaying sinusoid of set() calls
//   3. set(target); poll moving() until false (bounded by max_wait), or wait
//      `settle` when the positioner never reports motion (skipped when
//      |delta| < epsilon)
//   4. unblank, then unprotect in reverse order. On any failure the cleanup
//      still runs and the first error is returned: a detector is never left
//      protected by accident.
//
// The Spectrometer decides *what* to protect (MovePlan); this only runs it.
// Waiting goes through an injected sleep so tests drive a ManualClock.

#include <functional>
#include <optional>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/spectrometer/roles.hpp"
#include "pychron/devices/spectrometer/types.hpp"

namespace pychron::spectrometer {

struct AfDemagSettings {
  bool enabled = false;
  Duration period = std::chrono::milliseconds(500);
  Duration duration = std::chrono::seconds(10);
  double start_amplitude = 0.5;
  double threshold = 0.5;  // only moves with |delta| >= threshold demagnetize
};

// Samples per demag period.
inline constexpr int kAfDemagStepsPerPeriod = 8;

// set() values of the demag sweep around `target`, one every
// period / kAfDemagStepsPerPeriod, amplitude decaying linearly from
// start_amplitude to zero over `duration`, clamped to `limits`. Empty when
// disabled or the move is below threshold. Does not include the final
// set(target).
std::vector<double> af_demag_trajectory(double from, double target, const AfDemagSettings& af, Limits limits);

struct MovePlan {
  double from = 0.0;
  double to = 0.0;
  std::vector<ChannelId> protect;  // detector channels to protect for the move
  bool blank = false;              // blank the beam (needs an IBeamBlank)
  AfDemagSettings af_demag;
  Duration settle{};               // used when the positioner never reports motion
  bool wait_moving = true;
  Duration max_wait = std::chrono::seconds(30);
  Duration poll_interval = std::chrono::milliseconds(50);
  double epsilon = 1e-6;           // |delta| below this skips the settle wait
  Limits limits{1.0, 0.0};         // AF demag clamp; invalid (the default) = the positioner's limits
};

struct MoveDeps {
  IMassPositioner& positioner;
  IDetectorControl* control = nullptr;
  IBeamBlank* beam_blank = nullptr;
  const Clock& clock;
  std::function<void(Duration)> sleep;
  // Told after every successful protect()/unprotect so runtime state follows.
  std::function<void(const ChannelId&, bool)> on_protect;
};

struct MoveOutcome {
  double from = 0.0;
  double to = 0.0;
  Duration elapsed{};
  std::vector<ChannelId> protected_channels;  // protected during the move
  bool blanked = false;
  std::vector<double> demag;                  // demag setpoints issued
};

Result<MoveOutcome> execute_move(const MovePlan& plan, const MoveDeps& deps);

// Blocks until `clock` reaches now + d (returns at once for d <= 0).
void sleep_on(const Clock& clock, Duration d);

}  // namespace pychron::spectrometer
