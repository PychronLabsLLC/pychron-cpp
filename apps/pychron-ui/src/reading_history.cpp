#include "reading_history.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace pychron::ui {

namespace {

double seconds(Duration d) { return std::chrono::duration<double>(d).count(); }

constexpr double kBreak = std::numeric_limits<double>::quiet_NaN();

}  // namespace

ReadingHistory::ReadingHistory(std::vector<std::string> names, Duration gap) : gap_s_(seconds(gap)) {
  for (auto& name : names) kept_.try_emplace(std::move(name));
}

void ReadingHistory::add(const std::string& name, double value, const std::string& units, TimePoint ts) {
  const auto it = kept_.find(name);
  if (it == kept_.end()) return;
  Kept& k = it->second;
  // Time that went back (the line was started again) or other units: what is
  // held cannot be drawn on one axis with what comes now.
  if (k.newest && (ts < *k.newest || units != k.units)) {
    k.ring.clear();
    k.origin.reset();
    k.newest.reset();
  }
  if (!k.origin) k.origin = ts;
  const double t = seconds(ts - *k.origin);
  if (k.newest) {
    const double before = seconds(*k.newest - *k.origin);
    if (t - before > gap_s_) k.ring.push((before + t) / 2, kBreak);
  }
  k.ring.push(t, std::isfinite(value) ? value : kBreak);
  k.newest = ts;
  k.units = units;
  ++k.revision;
}

ReadingHistory::Series ReadingHistory::series(const std::string& name, Duration range) const {
  Series out;
  const auto it = kept_.find(name);
  if (it == kept_.end() || !it->second.newest) return out;
  const Kept& k = it->second;
  out.units = QString::fromStdString(k.units);
  out.newest = k.newest;
  const double newest = seconds(*k.newest - *k.origin);
  const double from = newest - seconds(range);
  // The first point at or after `from`: the ring is in time order.
  std::size_t lo = 0;
  std::size_t hi = k.ring.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (k.ring[mid].t < from) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  out.points.reserve(k.ring.size() - lo);
  for (std::size_t i = lo; i < k.ring.size(); ++i) out.points.push_back({k.ring[i].t - newest, k.ring[i].value});
  return out;
}

std::optional<TimePoint> ReadingHistory::oldest(const std::string& name) const {
  const auto it = kept_.find(name);
  if (it == kept_.end() || !it->second.origin || it->second.ring.empty()) return std::nullopt;
  const Kept& k = it->second;
  return *k.origin + std::chrono::duration_cast<Duration>(std::chrono::duration<double>(k.ring[0].t));
}

unsigned ReadingHistory::revision(const std::string& name) const {
  const auto it = kept_.find(name);
  return it == kept_.end() ? 0U : it->second.revision;
}

bool ReadingHistory::has(const std::string& name) const { return kept_.contains(name); }

}  // namespace pychron::ui
