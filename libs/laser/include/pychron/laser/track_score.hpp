#pragma once

// Score of a beam against a simulated grain (grain simulator design, section
// 7): how long the beam stayed on the grain, how far off it was, when it
// locked and lost it, how hot it kept the grain, and why the finder moved or
// held. Pure arithmetic on what the bench feeds it each poll.

#include <span>

#include "pychron/vision/dragonfly.hpp"

namespace pychron::laser {

struct TrackScore {
  double on_pct = 0, rms_mm = 0, max_mm = 0, lock_s = 0;
  int lost = 0;
  double back_s = 0;
  int moves = 0;
  double heat_pct = 0, track_pct = 0, hold_pct = 0, sat_pct = 0, search_pct = 0;
};

// On grain: distance <= beam radius. Locked: on grain for 1 s unbroken. Lost:
// after a lock, off grain for 1 s unbroken, dated from when it went off.
class TrackScorer {
 public:
  TrackScorer(double beam_radius_mm, double full_heat, double duration_s);

  // Every poll, t rising. Spacing of samples is free.
  void sample(double t_s, double distance_mm, double heat);
  // The reason of a look the finder made (Elapsed and Invalid are not binned).
  void look(vision::DragonflyStep::Reason reason);
  void moved();

  [[nodiscard]] TrackScore finish(double end_s) const;

 private:
  enum class State { Seeking, Locked, Lost };

  double radius_mm_;
  double full_heat_;

  State state_ = State::Seeking;
  bool any_ = false;
  double t0_ = 0;
  bool on_run_ = false, off_run_ = false;
  double on_start_ = 0, off_start_ = 0;
  double lock_s_;
  bool ever_locked_ = false;
  double loss_start_ = 0;
  int lost_ = 0;
  double back_sum_ = 0;

  long n_ = 0, n_on_ = 0;
  double sum_sq_ = 0, max_mm_ = 0, heat_sum_ = 0;
  int moves_ = 0;
  long track_ = 0, hold_ = 0, sat_ = 0, search_ = 0;
};

struct TrackSummary {
  TrackScore mean, worst;
};

// Mean over runs, and the worst of each column: the lowest on_pct and
// heat_pct, the highest of every other field. Integer fields of the mean are
// rounded. No runs: zeros.
[[nodiscard]] TrackSummary summarize(std::span<const TrackScore> runs);

}  // namespace pychron::laser
