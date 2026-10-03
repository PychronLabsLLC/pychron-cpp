#include "pychron/vision/autocenter.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pychron::vision {

namespace {

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

}  // namespace

Autocenter::Autocenter(ITargetFinder& finder, CameraStageMap map, double px_per_mm, AutocenterParams params)
    : finder_(finder), map_(map), px_per_mm_(px_per_mm), params_(params) {}

void Autocenter::reset() {
  iteration_ = 0;
  total_mm_ = 0;
  prev_offset_mm_ = -1;
  grow_count_ = 0;
}

AutocenterStep Autocenter::step(std::span<const FrameView> frames) {
  AutocenterStep out;
  out.action = AutocenterStep::Action::Failed;
  out.iteration = iteration_++;

  auto fail = [&](const char* why, Vec2 offset = {}) {
    out.action = AutocenterStep::Action::Failed;
    out.move_mm = {};
    out.offset_mm = offset;
    out.reason = why;
    return out;
  };

  const bool params_ok = map_.valid() && std::isfinite(px_per_mm_) && px_per_mm_ > 0 &&
                         std::isfinite(params_.hole_radius_mm) && params_.hole_radius_mm > 0 &&
                         std::isfinite(params_.crop_scale) && params_.crop_scale > 0 &&
                         finite(params_.aim_offset_px) && std::isfinite(params_.tolerance_mm) &&
                         std::isfinite(params_.max_step_mm) && params_.max_step_mm > 0 &&
                         std::isfinite(params_.max_total_mm);
  if (!params_ok || frames.empty()) return fail("invalid");

  const double radius_px = params_.hole_radius_mm * px_per_mm_;
  const int side = static_cast<int>(std::lround(params_.crop_scale * 2.0 * radius_px));
  if (side <= 0) return fail("invalid");

  FinderParams fp;
  fp.mode = FinderMode::Hole;
  fp.expected_radius_px = radius_px;
  fp.mask_radius_px = 0.5 * side;

  std::vector<double> xs, ys;
  for (const FrameView& f : frames) {
    const Rect r = centered_rect(f, side, params_.aim_offset_px);
    const Frame c = crop(f, r);
    if (c.width <= 0 || c.height <= 0) continue;
    const auto targets = finder_.find(c.view(), fp);
    if (targets.empty()) continue;
    // Crop coordinates back to the frame, then to an offset from the aim point.
    const double aim_x = (f.width - 1) / 2.0 + params_.aim_offset_px.x;
    const double aim_y = (f.height - 1) / 2.0 + params_.aim_offset_px.y;
    const double ox = std::max(r.x, 0) + targets.front().center_px.x - aim_x;
    const double oy = std::max(r.y, 0) + targets.front().center_px.y - aim_y;
    if (!std::isfinite(ox) || !std::isfinite(oy)) continue;
    xs.push_back(ox);
    ys.push_back(oy);
  }

  if (xs.size() * 2 <= frames.size()) return fail("no_target");

  const Vec2 offset_mm = map_.to_mm({median(xs), median(ys)});
  if (!finite(offset_mm)) return fail("invalid");
  out.offset_mm = offset_mm;
  const double mag = std::hypot(offset_mm.x, offset_mm.y);

  if (mag < params_.tolerance_mm) {
    out.action = AutocenterStep::Action::Converged;
    return out;
  }

  if (iteration_ - 1 >= params_.max_iterations) return fail("max_iterations", offset_mm);

  grow_count_ = (prev_offset_mm_ >= 0 && mag > prev_offset_mm_) ? grow_count_ + 1 : 0;
  prev_offset_mm_ = mag;
  if (grow_count_ >= 2) return fail("runaway", offset_mm);

  const double k = mag > params_.max_step_mm ? params_.max_step_mm / mag : 1.0;
  const Vec2 move{offset_mm.x * k, offset_mm.y * k};
  const double len = std::hypot(move.x, move.y);
  if (total_mm_ + len > params_.max_total_mm) return fail("max_total", offset_mm);
  total_mm_ += len;

  out.action = AutocenterStep::Action::Move;
  out.move_mm = move;
  return out;
}

}  // namespace pychron::vision
