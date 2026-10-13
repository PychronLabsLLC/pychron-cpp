#pragma once

// Rise rate: how fast one detector's signal climbs, from its readings between
// a start and a stop (the operator has isolated the spectrometer and wants
// the static rise). Two numbers, as legacy pychron gave: the rate between the
// first and last reading, and the slope of a straight line fitted to all of
// them, here against the readings' own times and with its standard error.
// Pure data: nothing here reads a clock or the spectrometer.

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/systems/spectrometer/acquisition.hpp"

namespace pychron::ui {

struct RisePoint {
  double t = 0.0;  // seconds
  double v = 0.0;  // the detector's units
};

struct RiseRate {
  int n = 0;                        // readings used
  double duration_min = 0.0;        // last time - first time
  double rise = 0.0;                // last value - first value
  double endpoints = 0.0;           // rise / duration_min
  double linear = 0.0;              // least-squares slope, units per minute
  std::optional<double> linear_se;  // its standard error; needs three readings
};

// Points whose value is not finite are left out. nullopt with fewer than two
// left or when they span no time. `points` are in time order.
[[nodiscard]] std::optional<RiseRate> rise_rate(std::span<const RisePoint> points);

// One detector's readings from start() on, in a buffer of its own (a run may
// outlast what the strip chart keeps).
class RiseRun {
 public:
  void start(std::string detector);
  void stop();  // keeps what was measured until the next start()
  // Takes the detector's value at the reading's time while running. A
  // reading without the detector, or with no value for it, is skipped. One
  // that is not later than the last kept voids the run: the scan began again.
  void add(const spectrometer::IntensityReading& reading);

  [[nodiscard]] bool running() const noexcept { return running_; }
  [[nodiscard]] const std::string& detector() const noexcept { return detector_; }
  [[nodiscard]] std::optional<RiseRate> current() const;  // nullopt for a void run
  [[nodiscard]] bool saturated() const noexcept { return saturated_; }
  [[nodiscard]] bool restarted() const noexcept { return restarted_; }
  [[nodiscard]] std::optional<TimePoint> first() const;
  [[nodiscard]] std::optional<TimePoint> last() const;

 private:
  std::string detector_;
  std::vector<RisePoint> points_;  // t from first_
  std::optional<TimePoint> first_, last_;
  bool running_ = false;
  bool saturated_ = false;
  bool restarted_ = false;
};

}  // namespace pychron::ui
