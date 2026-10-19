#include "selection_list.hpp"

#include <algorithm>
#include <iterator>

namespace pychron::ui {

namespace {

// Newest first; ties by uuid descending so the order is total (as the
// source's own browse order).
bool newer(const processing::AnalysisSummary& a, const processing::AnalysisSummary& b) {
  if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
  return a.uuid > b.uuid;
}

}  // namespace

std::size_t SelectionList::add(std::vector<processing::AnalysisSummary> rows) {
  std::size_t added = 0;
  for (auto& r : rows) {
    if (!held_.insert(r.uuid).second) continue;
    rows_.push_back(std::move(r));
    ++added;
  }
  if (added > 0) std::sort(rows_.begin(), rows_.end(), newer);
  return added;
}

std::size_t SelectionList::remove(const std::vector<std::string>& uuids) {
  const std::unordered_set<std::string> gone(uuids.begin(), uuids.end());
  const auto first_removed = std::remove_if(rows_.begin(), rows_.end(), [&](const auto& r) { return gone.contains(r.uuid); });
  const auto removed = static_cast<std::size_t>(std::distance(first_removed, rows_.end()));
  rows_.erase(first_removed, rows_.end());
  for (const auto& u : gone) held_.erase(u);
  return removed;
}

void SelectionList::clear() noexcept {
  rows_.clear();
  held_.clear();
}

std::size_t SelectionList::replace_from(std::vector<processing::AnalysisSummary> fresh) {
  std::vector<processing::AnalysisSummary> kept;
  kept.reserve(rows_.size());
  std::unordered_set<std::string> seen;
  for (auto& r : fresh) {
    if (!held_.contains(r.uuid) || !seen.insert(r.uuid).second) continue;
    kept.push_back(std::move(r));
  }
  const std::size_t dropped = rows_.size() - kept.size();
  std::sort(kept.begin(), kept.end(), newer);
  rows_ = std::move(kept);
  held_ = std::move(seen);
  return dropped;
}

bool SelectionList::contains(const std::string& uuid) const {
  return held_.contains(uuid);
}

std::vector<std::string> SelectionList::uuids() const {
  std::vector<std::string> out(held_.begin(), held_.end());
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace pychron::ui
