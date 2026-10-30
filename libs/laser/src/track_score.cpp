#include "pychron/laser/track_score.hpp"

#include <algorithm>
#include <cmath>

namespace pychron::laser {

namespace {
constexpr double kSpan = 1.0;  // seconds unbroken to lock or to lose
constexpr double kEps = 1e-9;

double pct(long part, long whole) {
  return whole > 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}
}  // namespace

TrackScorer::TrackScorer(double beam_radius_mm, double full_heat, double duration_s)
    : radius_mm_(beam_radius_mm), full_heat_(full_heat), lock_s_(duration_s) {}

void TrackScorer::sample(double t_s, double distance_mm, double heat) {
  using enum State;
  if (!any_) {
    any_ = true;
    t0_ = t_s;
  }
  ++n_;
  sum_sq_ += distance_mm * distance_mm;
  max_mm_ = std::max(max_mm_, distance_mm);
  heat_sum_ += full_heat_ > 0 ? heat / full_heat_ : 0.0;

  if (distance_mm <= radius_mm_) {
    ++n_on_;
    off_run_ = false;
    if (!on_run_) {
      on_run_ = true;
      on_start_ = t_s;
    }
    if (state_ != Locked && t_s - on_start_ >= kSpan - kEps) {
      if (!ever_locked_) {
        ever_locked_ = true;
        lock_s_ = on_start_ + kSpan - t0_;
      } else {
        back_sum_ += on_start_ + kSpan - loss_start_;
      }
      state_ = Locked;
    }
  } else {
    on_run_ = false;
    if (!off_run_) {
      off_run_ = true;
      off_start_ = t_s;
    }
    if (state_ == Locked && t_s - off_start_ >= kSpan - kEps) {
      ++lost_;
      loss_start_ = off_start_;
      state_ = Lost;
    }
  }
}

void TrackScorer::look(vision::DragonflyStep::Reason reason) {
  using R = vision::DragonflyStep::Reason;
  switch (reason) {
    case R::Track:
    case R::PerimeterClamp:
      ++track_;
      break;
    case R::Deadband:
    case R::Miss:
      ++hold_;
      break;
    case R::Saturated:
      ++sat_;
      break;
    case R::Search:
      ++search_;
      break;
    case R::Elapsed:
    case R::Invalid:
      break;
  }
}

void TrackScorer::moved() { ++moves_; }

TrackScore TrackScorer::finish(double end_s) const {
  TrackScore s;
  s.on_pct = pct(n_on_, n_);
  s.rms_mm = n_ > 0 ? std::sqrt(sum_sq_ / static_cast<double>(n_)) : 0.0;
  s.max_mm = max_mm_;
  s.lock_s = lock_s_;
  s.lost = lost_;
  double back = back_sum_;
  if (state_ == State::Lost) back += end_s - loss_start_;
  s.back_s = lost_ > 0 ? back / lost_ : 0.0;
  s.moves = moves_;
  s.heat_pct = n_ > 0 ? 100.0 * heat_sum_ / static_cast<double>(n_) : 0.0;
  const long looks = track_ + hold_ + sat_ + search_;
  s.track_pct = pct(track_, looks);
  s.hold_pct = pct(hold_, looks);
  s.sat_pct = pct(sat_, looks);
  s.search_pct = pct(search_, looks);
  return s;
}

TrackSummary summarize(std::span<const TrackScore> runs) {
  TrackSummary out;
  if (runs.empty()) return out;
  const auto n = static_cast<double>(runs.size());
  out.worst = runs.front();
  for (const TrackScore& r : runs) {
    out.mean.on_pct += r.on_pct / n;
    out.mean.rms_mm += r.rms_mm / n;
    out.mean.max_mm += r.max_mm / n;
    out.mean.lock_s += r.lock_s / n;
    out.mean.back_s += r.back_s / n;
    out.mean.heat_pct += r.heat_pct / n;
    out.mean.track_pct += r.track_pct / n;
    out.mean.hold_pct += r.hold_pct / n;
    out.mean.sat_pct += r.sat_pct / n;
    out.mean.search_pct += r.search_pct / n;
    out.mean.lost += r.lost / n;
    out.mean.moves += r.moves / n;
    TrackScore& w = out.worst;
    w.on_pct = std::min(w.on_pct, r.on_pct);
    w.heat_pct = std::min(w.heat_pct, r.heat_pct);
    w.rms_mm = std::max(w.rms_mm, r.rms_mm);
    w.max_mm = std::max(w.max_mm, r.max_mm);
    w.lock_s = std::max(w.lock_s, r.lock_s);
    w.lost = std::max(w.lost, r.lost);
    w.back_s = std::max(w.back_s, r.back_s);
    w.moves = std::max(w.moves, r.moves);
    w.track_pct = std::max(w.track_pct, r.track_pct);
    w.hold_pct = std::max(w.hold_pct, r.hold_pct);
    w.sat_pct = std::max(w.sat_pct, r.sat_pct);
    w.search_pct = std::max(w.search_pct, r.search_pct);
  }
  return out;
}

}  // namespace pychron::laser
