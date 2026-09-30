#pragma once

// Progress reporting for tuning jobs. A job reports (done, total, message)
// and, for sweeps, the point just measured so a live plot can follow along.
// The JobRunner forwards every update to the SignalBus as JobProgress.

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "pychron/core/clock.hpp"
#include "pychron/devices/spectrometer/types.hpp"

namespace pychron::jobs {

using spectrometer::DetectorId;

// One sweep step: the axis value actually applied and the mean intensity of
// each recorded detector that produced a value.
struct SweepPoint {
  double x = 0.0;
  std::map<DetectorId, double> y;
  TimePoint ts{};

  friend bool operator==(const SweepPoint&, const SweepPoint&) = default;
};

struct ProgressUpdate {
  std::size_t done = 0;
  std::size_t total = 0;  // 0: unknown
  std::string message;
  std::optional<SweepPoint> point;

  // done / total in [0, 1]; 0 when total is unknown.
  double fraction() const noexcept;
};

// Thread-safe: the job reports on its own thread while UI reads last().
class Progress {
 public:
  using Sink = std::function<void(const ProgressUpdate&)>;

  Progress() = default;
  explicit Progress(Sink sink) : sink_(std::move(sink)) {}

  // Stores the update and forwards it to the sink (outside the lock).
  void report(ProgressUpdate update);
  ProgressUpdate last() const;

 private:
  Sink sink_;
  mutable std::mutex mutex_;
  ProgressUpdate last_;
};

}  // namespace pychron::jobs
