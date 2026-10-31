#pragma once

// FollowPlotModel: the data behind the follow plots, with no widgets. One
// TimeSeriesRing per line, x = seconds since the dragonfly started
// (DragonflyStep::elapsed_s). Plain C++ so it is testable without a window.
//
// A value that is not there in a row (no blob, a miss, a real laser's true
// lines, a point grain's size) is a NaN point at that x (TimeSeriesRing::range
// skips NaN) so the view breaks the line there. An elapsed_s below the previous
// row's is a new run: the lines are cleared first.

#include <array>
#include <cstddef>
#include <optional>

#include "pychron/core/time_series_ring.hpp"
#include "pychron/laser/laser_system.hpp"

namespace pychron::ui {

enum class FollowLine {
  StageX,
  GrainX,
  StageY,
  GrainY,
  DeviationSeen,
  DeviationTrue,
  LuminositySeen,
  LuminosityTrue,
  SizeSeen,
  SizeTrue
};
inline constexpr std::size_t kFollowLines = 10;

// The circle a run's stage is held inside, stage mm.
struct FollowFence {
  double x = 0;
  double y = 0;
  double radius_mm = 0;
};

class FollowPlotModel {
 public:
  static constexpr double kSpanSeconds = 600;  // ten minutes

  FollowPlotModel();

  void append(const laser::FollowSample& s);
  const TimeSeriesRing& series(FollowLine line) const { return rings_[static_cast<std::size_t>(line)]; }
  // Any finite value in this run: false for a real laser's true lines and a
  // point grain's true size, which the view leaves out.
  bool has(FollowLine line) const;
  double latest_x() const { return latest_x_; }  // 0 when empty
  // Of the latest row that gives one: the radius in force, not the pattern's own.
  const std::optional<FollowFence>& fence() const { return fence_; }
  void clear();

 private:
  std::array<TimeSeriesRing, kFollowLines> rings_;
  std::array<bool, kFollowLines> any_{};
  std::optional<FollowFence> fence_;
  double latest_x_ = 0;
  bool empty_ = true;
};

}  // namespace pychron::ui
