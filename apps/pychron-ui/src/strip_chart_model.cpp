#include "strip_chart_model.hpp"
#include "theme.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>

namespace pychron::ui {

namespace {

constexpr double kMinScanWidth = 1.0;
constexpr double kRingFactor = 1.8;
constexpr double kPad = 0.10;
constexpr double kLogFloor = 1e-6;
constexpr std::chrono::milliseconds kAutoscaleInterval{500};

}  // namespace

QColor StripChartModel::palette_color(std::size_t index) {
  const auto& series = theme().series;
  return series[index % series.size()];
}

StripChartModel::StripChartModel(std::vector<DetectorSeries> detectors)
    : detectors_(std::move(detectors)) {
  rings_.reserve(detectors_.size());
  for (std::size_t i = 0; i < detectors_.size(); ++i) {
    if (!detectors_[i].color.isValid()) detectors_[i].color = palette_color(i);
    rings_.emplace_back(kRingFactor * scan_width_);
    index_[detectors_[i].name] = i;
  }
}

void StripChartModel::append(const spectrometer::IntensityReading& r) {
  const auto& row = r.reading;
  bool known = false;
  for (const auto& kv : row.values) known = known || index_.count(kv.first) > 0;
  if (!known) return;

  if (origin_ && row.ts < last_ts_) clear();
  if (!origin_) origin_ = row.ts;
  last_ts_ = row.ts;
  latest_x_ = std::chrono::duration<double>(row.ts - *origin_).count();

  for (const auto& [name, value] : row.values) {
    auto it = index_.find(name);
    if (it == index_.end()) continue;
    rings_[it->second].push(
        latest_x_, value ? value->mean : std::numeric_limits<double>::quiet_NaN());
  }
}

void StripChartModel::clear() {
  for (auto& ring : rings_) ring.clear();
  origin_.reset();
  latest_x_ = 0.0;
  last_auto_.reset();
}

void StripChartModel::set_scan_width(double seconds) {
  scan_width_ = std::max(seconds, kMinScanWidth);
  for (auto& ring : rings_) ring.set_span(kRingFactor * scan_width_);
  last_auto_.reset();
}

void StripChartModel::set_visible(std::size_t i, bool v) {
  if (i >= detectors_.size()) return;
  detectors_[i].visible = v;
  last_auto_.reset();
}

void StripChartModel::set_scale(YScale s) {
  scale_ = s;
  last_auto_.reset();
  if (s == YScale::Log && manual_.lo <= 0.0) manual_ = {kLogFloor, std::max(manual_.hi, 1.0)};
}

void StripChartModel::set_autoscale(bool on) {
  if (autoscale_ && !on) manual_ = auto_;  // manual fields start at the live range
  autoscale_ = on;
  last_auto_.reset();
}

bool StripChartModel::set_manual_y(double lo, double hi) {
  if (!(lo < hi)) return false;  // also rejects NaN
  if (scale_ == YScale::Log && lo <= 0.0) return false;
  manual_ = {lo, hi};
  return true;
}

AxisRange StripChartModel::x_range() const {
  const double w = scan_width_;
  if (latest_x_ < w) return {0.0, 1.05 * w};
  return {latest_x_ - w, latest_x_ + 0.05 * w};
}

AxisRange StripChartModel::compute_autoscale() const {
  const bool log = scale_ == YScale::Log;
  const AxisRange xr = x_range();
  double lo = std::numeric_limits<double>::infinity();
  double hi = -lo;
  bool any = false;
  for (std::size_t i = 0; i < detectors_.size(); ++i) {
    if (!detectors_[i].visible) continue;
    const TimeSeriesRing& ring = rings_[i];
    if (!log) {
      if (auto r = ring.range(xr.lo, xr.hi)) {
        lo = std::min(lo, r->first);
        hi = std::max(hi, r->second);
        any = true;
      }
      continue;
    }
    // TimeSeriesRing::range has no positive filter, so scan for them here.
    for (std::size_t k = 0; k < ring.size(); ++k) {
      const auto& p = ring[k];
      if (p.t < xr.lo || p.t > xr.hi || !(p.value > 0.0)) continue;
      lo = std::min(lo, p.value);
      hi = std::max(hi, p.value);
      any = true;
    }
  }

  if (!any) {
    AxisRange keep = auto_;
    if (log && keep.lo <= 0.0) keep = {kLogFloor, std::max(keep.hi, 1.0)};
    return keep;
  }
  if (log) return {lo * (1.0 - kPad), hi * (1.0 + kPad)};  // proportional pad keeps lo > 0
  double pad = kPad * (hi - lo);
  if (pad <= 0.0) pad = lo != 0.0 ? kPad * std::fabs(lo) : 1.0;
  return {lo - pad, hi + pad};
}

AxisRange StripChartModel::y_range(TimePoint now) {
  if (!autoscale_) return manual_;
  if (!last_auto_ || now < *last_auto_ || now - *last_auto_ >= kAutoscaleInterval) {
    auto_ = compute_autoscale();
    last_auto_ = now;
  }
  return auto_;
}

}  // namespace pychron::ui
