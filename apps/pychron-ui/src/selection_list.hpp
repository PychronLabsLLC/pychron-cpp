#pragma once

// The data browser's Selected column: analyses kept across queries, unique by
// uuid, newest first. No Qt.

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

#include "pychron/processing/source.hpp"

namespace pychron::ui {

class SelectionList {
 public:
  // Adds rows not already held; returns how many were added.
  std::size_t add(std::vector<processing::AnalysisSummary> rows);
  // Returns how many were removed.
  std::size_t remove(const std::vector<std::string>& uuids);
  void clear() noexcept;
  // Rows refetched from the source replace the held ones; held uuids missing
  // from `fresh` are dropped, and rows of `fresh` that are not held are
  // ignored. Returns how many were dropped.
  std::size_t replace_from(std::vector<processing::AnalysisSummary> fresh);

  [[nodiscard]] bool contains(const std::string& uuid) const;
  [[nodiscard]] const std::vector<processing::AnalysisSummary>& rows() const noexcept { return rows_; }  // newest first
  [[nodiscard]] std::vector<std::string> uuids() const;                                                   // sorted ascending
  [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }
  [[nodiscard]] bool empty() const noexcept { return rows_.empty(); }

 private:
  std::vector<processing::AnalysisSummary> rows_;
  std::unordered_set<std::string> held_;
};

}  // namespace pychron::ui
