#include "follow_plot_model.hpp"

#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace pychron::ui {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

template <std::size_t... I>
std::array<TimeSeriesRing, sizeof...(I)> make_rings(std::index_sequence<I...>) {
  return {{((void)I, TimeSeriesRing(FollowPlotModel::kSpanSeconds))...}};
}

}  // namespace

FollowPlotModel::FollowPlotModel() : rings_(make_rings(std::make_index_sequence<kFollowLines>{})) {}

void FollowPlotModel::append(const laser::FollowSample& s) {
  const auto& step = s.step;
  if (!empty_ && step.elapsed_s < latest_x_) clear();
  const double t = step.elapsed_s;

  std::array<double, kFollowLines> v{};
  v.fill(kNaN);
  auto put = [&v](FollowLine line, double value) { v[static_cast<std::size_t>(line)] = value; };

  put(FollowLine::StageX, step.stage_mm.x);
  put(FollowLine::StageY, step.stage_mm.y);
  if (step.seen) {
    put(FollowLine::DeviationSeen, std::hypot(step.seen->offset_mm.x, step.seen->offset_mm.y));
    put(FollowLine::SizeSeen, 2 * std::sqrt(step.seen->area_mm2 / std::numbers::pi));
  }
  using Reason = vision::DragonflyStep::Reason;
  if (step.reason != Reason::Miss && step.reason != Reason::Search)
    put(FollowLine::LuminositySeen, step.saturation);
  if (s.grain) {
    const auto& g = *s.grain;
    put(FollowLine::GrainX, g.grain_mm.x);
    put(FollowLine::GrainY, g.grain_mm.y);
    put(FollowLine::DeviationTrue, std::hypot(g.grain_mm.x - g.beam_mm.x, g.grain_mm.y - g.beam_mm.y));
    put(FollowLine::LuminosityTrue, g.truth.brightness);
    if (g.truth.radius_mm > 0) put(FollowLine::SizeTrue, 2 * g.truth.radius_mm);
  }

  for (std::size_t i = 0; i < kFollowLines; ++i) {
    rings_[i].push(t, v[i]);
    if (std::isfinite(v[i])) any_[i] = true;
  }
  latest_x_ = t;
  empty_ = false;
}

bool FollowPlotModel::has(FollowLine line) const { return any_[static_cast<std::size_t>(line)]; }

void FollowPlotModel::clear() {
  for (auto& r : rings_) r.clear();
  any_.fill(false);
  latest_x_ = 0;
  empty_ = true;
}

}  // namespace pychron::ui
