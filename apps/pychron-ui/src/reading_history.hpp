#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QString>

#include "pychron/core/clock.hpp"
#include "pychron/core/time_series_ring.hpp"

namespace pychron::ui {

// A day of timed readings for each of a fixed set of names (a gauge, a
// heater, an input). It knows nothing of what a name is. Filled and read on
// one thread. A point's time is its reading's own: no clock is read here.
class ReadingHistory {
 public:
  static constexpr double kSpanSeconds = 24.0 * 3600.0;

  // `gap` is the longest two readings may be apart and still be joined by a line.
  // Only `names` are kept: a reading of any other is ignored.
  ReadingHistory(std::vector<std::string> names, Duration gap);

  // A reading earlier than the series' newest, or in other units than it
  // holds, empties that series first. A value that is no number is a break.
  void add(const std::string& name, double value, const std::string& units, TimePoint ts);

  struct Series {
    std::vector<TimeSeriesRing::Point> points;  // t: seconds before `newest` (<= 0); NaN value: a break
    QString units;
    std::optional<TimePoint> newest;            // of the series; nullopt when it has none
  };
  // The readings of `name` no older than `range` before its newest one.
  [[nodiscard]] Series series(const std::string& name, Duration range) const;
  [[nodiscard]] std::optional<TimePoint> oldest(const std::string& name) const;
  [[nodiscard]] unsigned revision(const std::string& name) const;  // moves with every change to that series
  [[nodiscard]] bool has(const std::string& name) const;

 private:
  struct Kept {
    TimeSeriesRing ring{kSpanSeconds};  // t: seconds from `origin`
    std::optional<TimePoint> origin;    // the first reading held since the series was last emptied
    std::optional<TimePoint> newest;
    std::string units;
    unsigned revision = 0;
  };

  std::map<std::string, Kept> kept_;
  double gap_s_;
};

}  // namespace pychron::ui
