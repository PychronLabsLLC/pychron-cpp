#include "pychron/processing/time_breaks.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string_view>

namespace pychron::processing {

namespace {

// The session being walked for one spectrometer: its analyses seen so far.
struct Session {
  std::size_t runs = 0;
  double newest = 0.0;
  double oldest = 0.0;
};

}  // namespace

std::vector<TimeBreak> time_breaks(std::span<const AnalysisSummary> rows, double threshold_seconds) {
  std::vector<TimeBreak> out;
  if (!(threshold_seconds > 0.0)) return out;
  std::map<std::string_view, Session> sessions;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    auto [it, first] = sessions.try_emplace(row.mass_spectrometer, Session{1, row.timestamp, row.timestamp});
    if (first) continue;
    Session& s = it->second;
    const double gap = s.oldest - row.timestamp;
    if (gap > threshold_seconds) {
      out.push_back(TimeBreak{i, gap, s.runs, s.oldest, s.newest});
      s = Session{1, row.timestamp, row.timestamp};
    } else {
      ++s.runs;
      s.oldest = row.timestamp;
    }
  }
  return out;
}

std::string gap_text(double seconds) {
  const auto minutes = static_cast<long long>(std::llround(std::max(0.0, seconds) / 60.0));
  const long long days = minutes / (24 * 60);
  const long long hours = minutes / 60 % 24;
  const long long mins = minutes % 60;
  if (days > 0) return std::to_string(days) + " d" + (hours > 0 ? " " + std::to_string(hours) + " h" : "");
  if (hours > 0) return std::to_string(hours) + " h" + (mins > 0 ? " " + std::to_string(mins) + " min" : "");
  return std::to_string(mins) + " min";
}

}  // namespace pychron::processing
