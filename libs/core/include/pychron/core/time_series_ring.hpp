#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace pychron {

// Time-bounded history of (t, value) samples. Points older than `span` behind
// the newest sample are dropped, and the buffer never exceeds kMaxPoints.
// Not thread-safe: one owner appends and reads.
class TimeSeriesRing {
 public:
  struct Point {
    double t;
    double value;
  };

  static constexpr std::size_t kMaxPoints = 200000;

  // A non-positive span is clamped to a small positive value.
  explicit TimeSeriesRing(double span_s);

  double span() const noexcept { return span_; }
  void set_span(double span_s);  // trims immediately

  // A sample older than the newest one is ignored.
  void push(double t, double value);
  void clear();

  std::size_t size() const noexcept { return points_.size(); }
  bool empty() const noexcept { return points_.empty(); }
  const Point& operator[](std::size_t i) const { return points_[i]; }  // 0 = oldest
  const Point& back() const { return points_.back(); }

  // (min, max) of the values with t0 <= t <= t1, NaN skipped; nullopt if none.
  std::optional<std::pair<double, double>> range(double t0, double t1) const;

 private:
  void trim();

  double span_;
  std::deque<Point> points_;
};

}  // namespace pychron
