#pragma once

// Time breaks in a list of analyses (data browser search and display design,
// section 3.5): where a spectrometer ran nothing for longer than a limit. The
// browser draws a separator row at each.

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "pychron/processing/source.hpp"

namespace pychron::processing {

struct TimeBreak {
  std::size_t above = 0;         // index in the rows: the separator is drawn above this row
  double gap_seconds = 0.0;      // longer than the threshold
  std::size_t session_runs = 0;  // analyses of the same spectrometer newer than the gap, back to
                                 // the previous gap or to the first row
  double session_start = 0.0;    // timestamp of the oldest of those
  double session_end = 0.0;      // timestamp of the newest of those
  friend bool operator==(const TimeBreak&, const TimeBreak&) = default;
};

// `rows` newest first. A break lies between two analyses of one spectrometer
// that are consecutive in time among `rows` and more than `threshold_seconds`
// apart; it is reported at the older of the two, so the breaks of a prefix of
// `rows` are those of the whole list that lie in the prefix. Sorted by
// `above`. None when the threshold is zero or negative.
[[nodiscard]] std::vector<TimeBreak> time_breaks(std::span<const AnalysisSummary> rows, double threshold_seconds);

// "45 min", "14 h 20 min", "3 d 4 h": the two largest units, to the minute.
[[nodiscard]] std::string gap_text(double seconds);

}  // namespace pychron::processing
