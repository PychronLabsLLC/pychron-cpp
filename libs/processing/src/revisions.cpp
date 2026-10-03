#include "pychron/processing/revisions.hpp"

#include <algorithm>
#include <map>

namespace pychron::processing {

std::string_view to_string(RevisionKind kind) noexcept {
  switch (kind) {
    case RevisionKind::Intercepts: return "intercepts";
    case RevisionKind::Baselines: return "baselines";
    case RevisionKind::Blanks: return "blanks";
    case RevisionKind::IcFactors: return "icfactors";
    case RevisionKind::Tags: return "tags";
    case RevisionKind::Annotation: return "annotation";
    case RevisionKind::Signals: return "signals";
  }
  return "";
}

std::optional<RevisionKind> parse_revision_kind(std::string_view text) noexcept {
  for (auto k : kRevisionKinds)
    if (to_string(k) == text) return k;
  return std::nullopt;
}

std::string_view title(RevisionKind kind) noexcept {
  switch (kind) {
    case RevisionKind::Intercepts: return "Intercepts";
    case RevisionKind::Baselines: return "Baselines";
    case RevisionKind::Blanks: return "Blanks";
    case RevisionKind::IcFactors: return "IC factors";
    case RevisionKind::Tags: return "Tag";
    case RevisionKind::Annotation: return "Comment";
    case RevisionKind::Signals: return "Signals";
  }
  return "";
}

std::string_view to_string(DiffState state) noexcept {
  switch (state) {
    case DiffState::Same: return "same";
    case DiffState::Changed: return "changed";
    case DiffState::Added: return "added";
    case DiffState::Removed: return "removed";
  }
  return "";
}

int RevisionDiff::changed_rows() const {
  return static_cast<int>(std::count_if(rows.begin(), rows.end(), [](const DiffRow& r) { return r.state != DiffState::Same; }));
}

RevisionDiff diff_revisions(const RevisionTable& before, const RevisionTable& after) {
  RevisionDiff d;
  d.columns = before.columns;
  for (const auto& c : after.columns)
    if (std::find(d.columns.begin(), d.columns.end(), c) == d.columns.end()) d.columns.push_back(c);
  // Cells of a row re-indexed onto the union of columns.
  auto align = [&](const RevisionTable& t, const RevisionTable::Row& row) {
    std::vector<std::string> out(d.columns.size());
    for (std::size_t i = 0; i < t.columns.size() && i < row.cells.size(); ++i) {
      const auto at = std::find(d.columns.begin(), d.columns.end(), t.columns[i]) - d.columns.begin();
      out[static_cast<std::size_t>(at)] = row.cells[i];
    }
    return out;
  };
  std::map<std::string, const RevisionTable::Row*> after_rows;
  for (const auto& r : after.rows) after_rows.emplace(r.key, &r);
  std::map<std::string, bool> seen;
  for (const auto& r : before.rows) {
    DiffRow row;
    row.key = r.key;
    row.before = align(before, r);
    seen[r.key] = true;
    if (auto it = after_rows.find(r.key); it != after_rows.end()) {
      row.after = align(after, *it->second);
      row.state = DiffState::Same;
    } else {
      row.after.assign(d.columns.size(), {});
      row.state = DiffState::Removed;
    }
    row.changed.resize(d.columns.size());
    for (std::size_t i = 0; i < d.columns.size(); ++i) {
      row.changed[i] = row.before[i] != row.after[i];
      if (row.changed[i] && row.state == DiffState::Same) row.state = DiffState::Changed;
    }
    d.rows.push_back(std::move(row));
  }
  for (const auto& r : after.rows) {
    if (seen.count(r.key)) continue;
    DiffRow row;
    row.key = r.key;
    row.state = DiffState::Added;
    row.before.assign(d.columns.size(), {});
    row.after = align(after, r);
    row.changed.resize(d.columns.size());
    for (std::size_t i = 0; i < d.columns.size(); ++i) row.changed[i] = !row.after[i].empty();
    d.rows.push_back(std::move(row));
  }
  return d;
}

}  // namespace pychron::processing
