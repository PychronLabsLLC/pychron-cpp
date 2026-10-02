#include "pychron/core/time_series_ring.hpp"

#include <algorithm>
#include <cmath>

namespace pychron {

namespace {
constexpr double kMinSpan = 1e-9;
}

TimeSeriesRing::TimeSeriesRing(double span_s) : span_(std::max(span_s, kMinSpan)) {}

void TimeSeriesRing::set_span(double span_s) {
  span_ = std::max(span_s, kMinSpan);
  trim();
}

void TimeSeriesRing::push(double t, double value) {
  if (!points_.empty() && t < points_.back().t) return;
  points_.push_back({t, value});
  trim();
}

void TimeSeriesRing::clear() { points_.clear(); }

void TimeSeriesRing::trim() {
  if (points_.empty()) return;
  const double cutoff = points_.back().t - span_;
  while (points_.front().t < cutoff) points_.pop_front();
  while (points_.size() > kMaxPoints) points_.pop_front();
}

std::optional<std::pair<double, double>> TimeSeriesRing::range(double t0, double t1) const {
  std::optional<std::pair<double, double>> out;
  for (const Point& p : points_) {
    if (p.t < t0 || p.t > t1 || std::isnan(p.value)) continue;
    if (!out) {
      out = std::make_pair(p.value, p.value);
    } else {
      out->first = std::min(out->first, p.value);
      out->second = std::max(out->second, p.value);
    }
  }
  return out;
}

}  // namespace pychron
