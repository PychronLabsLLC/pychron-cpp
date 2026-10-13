#include "rise_rate.hpp"

#include <chrono>
#include <cmath>
#include <utility>

namespace pychron::ui {

std::optional<RiseRate> rise_rate(std::span<const RisePoint> points) {
  std::vector<RisePoint> used;
  used.reserve(points.size());
  for (const auto& p : points)
    if (std::isfinite(p.v) && std::isfinite(p.t)) used.push_back(p);
  if (used.size() < 2) return std::nullopt;
  const double t0 = used.front().t;
  const double minutes = (used.back().t - t0) / 60.0;
  if (!(minutes > 0.0)) return std::nullopt;

  const auto n = static_cast<double>(used.size());
  double mean_x = 0.0;
  double mean_y = 0.0;
  for (const auto& p : used) {
    mean_x += (p.t - t0) / 60.0;
    mean_y += p.v;
  }
  mean_x /= n;
  mean_y /= n;
  double sxx = 0.0;
  double sxy = 0.0;
  for (const auto& p : used) {
    const double dx = (p.t - t0) / 60.0 - mean_x;
    sxx += dx * dx;
    sxy += dx * (p.v - mean_y);
  }
  if (!(sxx > 0.0)) return std::nullopt;

  RiseRate out;
  out.n = static_cast<int>(used.size());
  out.duration_min = minutes;
  out.rise = used.back().v - used.front().v;
  out.endpoints = out.rise / minutes;
  out.linear = sxy / sxx;
  if (used.size() >= 3) {
    const double intercept = mean_y - out.linear * mean_x;
    double ssr = 0.0;
    for (const auto& p : used) {
      const double residual = p.v - (intercept + out.linear * ((p.t - t0) / 60.0));
      ssr += residual * residual;
    }
    out.linear_se = std::sqrt(ssr / (n - 2.0) / sxx);
  }
  return out;
}

void RiseRun::start(std::string detector) {
  detector_ = std::move(detector);
  points_.clear();
  first_.reset();
  last_.reset();
  saturated_ = false;
  restarted_ = false;
  running_ = true;
}

void RiseRun::stop() { running_ = false; }

void RiseRun::add(const spectrometer::IntensityReading& reading) {
  if (!running_ || restarted_) return;
  const auto it = reading.reading.values.find(detector_);
  if (it == reading.reading.values.end() || !it->second) return;
  const TimePoint ts = reading.reading.ts;
  if (last_ && ts <= *last_) {
    restarted_ = true;
    return;
  }
  if (!first_) first_ = ts;
  last_ = ts;
  saturated_ = saturated_ || it->second->saturated;
  points_.push_back({std::chrono::duration<double>(ts - *first_).count(), it->second->mean});
}

std::optional<RiseRate> RiseRun::current() const {
  if (restarted_) return std::nullopt;
  return rise_rate(points_);
}

std::optional<TimePoint> RiseRun::first() const { return first_; }
std::optional<TimePoint> RiseRun::last() const { return last_; }

}  // namespace pychron::ui
