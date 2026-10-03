#include "pychron/vision/dragonfly.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "pychron/vision/kernel.hpp"

namespace pychron::vision {

namespace {

using Step = DragonflyStep;

bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
bool positive(double v) { return std::isfinite(v) && v > 0; }
bool non_negative(double v) { return std::isfinite(v) && v >= 0; }

}  // namespace

Dragonfly::Dragonfly(ITargetFinder& finder, CameraStageMap map, double px_per_mm, DragonflyParams params)
    : finder_(finder),
      map_(map),
      px_per_mm_(px_per_mm),
      params_(params),
      // A bad base is reported by step() as Invalid; keep the member usable.
      spiral_(params.spiral, positive(params.spiral_base_mm) ? params.spiral_base_mm : 1.0) {}

void Dragonfly::start(TimePoint now, Vec2 stage_pos_mm) {
  started_ = true;
  start_time_ = now;
  start_pos_ = stage_pos_mm;
  anchor_ = {};
  misses_ = 0;
  have_move_ = false;
  prev_ring_ = 0;
  ring_inside_ = false;
  spiral_.reset();
}

bool Dragonfly::params_ok() const {
  return map_.valid() && positive(px_per_mm_) && positive(params_.perimeter_radius_mm) &&
         positive(params_.max_step_mm) && positive(params_.spiral_base_mm) &&
         positive(params_.target_radius_mm) && non_negative(params_.aggressiveness) &&
         non_negative(params_.move_threshold_mm) && std::isfinite(params_.saturation_threshold) &&
         params_.saturation_threshold > 0 && params_.saturation_threshold <= 1 &&
         params_.frames_per_step >= 1 && params_.miss_frames_before_search >= 1;
}

Result<DragonflyStep> Dragonfly::step(std::span<const FrameView> frames, TimePoint now, Vec2 stage_pos_mm) {
  Step out;

  // Every Hold carries `current`, so it must be finite: positions that are
  // finite but whose difference overflows are rejected here, once.
  const Vec2 current{stage_pos_mm.x - start_pos_.x, stage_pos_mm.y - start_pos_.y};
  if (!started_ || !finite(start_pos_) || !finite(stage_pos_mm) || !finite(current) || !params_ok()) {
    out.action = Step::Action::Hold;
    out.reason = Step::Reason::Invalid;
    if (started_ && finite(current)) out.target_mm = current;
    return out;
  }
  out.target_mm = current;

  if (now - start_time_ >= params_.total_duration) {
    out.action = Step::Action::Done;
    out.reason = Step::Reason::Elapsed;
    return out;
  }

  if (have_move_) {
    for (const FrameView& f : frames) {
      if (f.timestamp < last_move_now_) return fail(ErrorKind::Protocol, "stale frame");
    }
  }

  // Crop and mask are sized from the target, centred on the image centre.
  const double diameter_px = 2.0 * params_.target_radius_mm * px_per_mm_;
  constexpr double kMaxSidePx = 1.0e5;
  const double side_d = 2.5 * diameter_px;
  if (!std::isfinite(side_d) || side_d < 1.0 || side_d > kMaxSidePx) {
    out.action = Step::Action::Hold;
    out.reason = Step::Reason::Invalid;
    return out;
  }
  const int side = static_cast<int>(std::lround(side_d));

  FinderParams fp;
  fp.mode = FinderMode::Glow;
  fp.expected_radius_px = params_.target_radius_mm * px_per_mm_;
  fp.mask_radius_px = 1.05 * diameter_px;

  struct Hit {
    Vec2 offset_px;
    double sat;
  };
  std::vector<Hit> hits;
  // Frames where the glow fills the mask: the finder's contrast gate finds no
  // blob, but the masked median is bright. They count as saturated, not as misses.
  std::vector<double> flooded;
  for (const FrameView& f : frames) {
    const Rect r = centered_rect(f, side);
    // Where the clamped crop really starts; the frame centre is the reference.
    const int x0 = std::max(r.x, 0), y0 = std::max(r.y, 0);
    const Frame c = crop(f, r);
    if (c.width <= 0 || c.height <= 0) continue;
    const auto targets = finder_.find(c.view(), fp);
    if (targets.empty()) {
      if (c.pixel_depth == 0) continue;
      const double m = static_cast<double>(median_in_mask(c.view(), fp.mask_radius_px)) /
                       static_cast<double>(c.pixel_depth);
      if (m >= params_.saturation_threshold) flooded.push_back(std::min(m, 1.0));
      continue;
    }
    const Vec2 off{x0 + targets.front().center_px.x - (f.width - 1) / 2.0,
                   y0 + targets.front().center_px.y - (f.height - 1) / 2.0};
    if (!finite(off)) continue;
    const double s = saturation(targets.front());
    hits.push_back({off, std::isfinite(s) ? std::clamp(s, 0.0, 1.0) : 0.0});
  }

  auto project = [&](Vec2 t, Step::Reason& reason) {
    const double len = std::hypot(t.x, t.y);
    if (len > params_.perimeter_radius_mm) {
      const double k = params_.perimeter_radius_mm / len;
      t = {t.x * k, t.y * k};
      reason = Step::Reason::PerimeterClamp;
    }
    return t;
  };
  auto hold = [&](Step::Reason why) {
    out.action = Step::Action::Hold;
    out.reason = why;
    out.target_mm = current;
  };
  // A projected move that goes nowhere is a Hold; commanding it would only
  // re-send the stage to where it already is.
  auto finish_move = [&](Vec2 t) {
    if (!finite(t)) {
      out.action = Step::Action::Hold;
      out.reason = Step::Reason::Invalid;
      out.target_mm = finite(current) ? current : Vec2{};
      return;
    }
    if (out.reason == Step::Reason::PerimeterClamp &&
        std::hypot(t.x - current.x, t.y - current.y) < params_.move_threshold_mm) {
      hold(Step::Reason::PerimeterClamp);
      return;
    }
    out.action = Step::Action::Move;
    out.target_mm = t;
    have_move_ = true;
    last_move_now_ = now;
  };
  auto reanchor = [&](Vec2 at) {
    anchor_ = at;
    prev_ring_ = 0;
    ring_inside_ = false;
    spiral_.reset();
  };

  if (hits.empty() && flooded.empty()) {
    misses_ = std::min(misses_ + 1, params_.miss_frames_before_search);
    if (misses_ < params_.miss_frames_before_search) {
      hold(Step::Reason::Miss);
      return out;
    }
    // The spiral advances even when the point is projected, so a search at
    // the perimeter keeps moving around it. A whole ring outside the
    // perimeter restarts the search from the anchor instead of circling the rim.
    Vec2 p = spiral_.next();
    if (spiral_.ring() != prev_ring_ && prev_ring_ >= 1 && !ring_inside_) {
      spiral_.reset();
      p = spiral_.next();
    }
    if (spiral_.ring() != prev_ring_) ring_inside_ = false;
    prev_ring_ = spiral_.ring();
    const Vec2 raw{anchor_.x + p.x, anchor_.y + p.y};
    if (std::hypot(raw.x, raw.y) <= params_.perimeter_radius_mm) ring_inside_ = true;
    out.reason = Step::Reason::Search;
    finish_move(project(raw, out.reason));
    return out;
  }

  misses_ = 0;
  double sat_sum = 0;
  for (const Hit& h : hits) sat_sum += h.sat;
  for (double v : flooded) sat_sum += v;
  out.saturation = sat_sum / static_cast<double>(hits.size() + flooded.size());

  if (hits.empty() || out.saturation >= params_.saturation_threshold) {
    hold(Step::Reason::Saturated);
    reanchor(current);
    return out;
  }

  double wsum = 0, wx = 0, wy = 0, ux = 0, uy = 0;
  for (const Hit& h : hits) {
    wsum += h.sat;
    wx += h.sat * h.offset_px.x;
    wy += h.sat * h.offset_px.y;
    ux += h.offset_px.x;
    uy += h.offset_px.y;
  }
  const double n = static_cast<double>(hits.size());
  const Vec2 mean_px = wsum > 0 ? Vec2{wx / wsum, wy / wsum} : Vec2{ux / n, uy / n};

  Vec2 corr = map_.to_mm(mean_px);
  corr = {corr.x * params_.aggressiveness, corr.y * params_.aggressiveness};
  if (!finite(corr)) {
    hold(Step::Reason::Invalid);
    return out;
  }
  const double mag = std::hypot(corr.x, corr.y);
  if (mag < params_.move_threshold_mm) {
    hold(Step::Reason::Deadband);
    reanchor(current);
    return out;
  }
  if (mag > params_.max_step_mm) {
    const double k = params_.max_step_mm / mag;
    corr = {corr.x * k, corr.y * k};
  }

  out.reason = Step::Reason::Track;
  const Vec2 t = project({current.x + corr.x, current.y + corr.y}, out.reason);
  finish_move(t);
  if (finite(t)) reanchor(out.action == Step::Action::Move ? t : current);
  return out;
}

}  // namespace pychron::vision
