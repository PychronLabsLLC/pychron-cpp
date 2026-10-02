#pragma once

// Datasets (design section 7.3): reduced analyses with their grouping and
// exclusion state. Immutable once built; units produce new datasets.

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pychron/processing/reduced.hpp"

namespace pychron::processing {

// legacy tab_id / graph_id / group_id / subgroup (pipeline/grouping.py).
struct GroupPath {
  int tab = 0, graph = 0, group = 0, subgroup = 0;
  friend auto operator<=>(const GroupPath&, const GroupPath&) = default;
};

// One precedence instead of legacy's tag / temp_status / otemp_status /
// temp_selected: user > filter > tag.
struct ExclusionState {
  std::optional<bool> user;  // true: the user excluded it; false: the user forced it in
  bool filter = false;       // a filter unit in omit mode excluded it
  bool tag = false;          // its tag is in the dataset's excluded-tag set

  bool excluded() const noexcept {
    if (user) return *user;
    return filter || tag;
  }
  bool included() const noexcept { return !excluded(); }
  friend bool operator==(const ExclusionState&, const ExclusionState&) = default;
};

struct DatasetItem {
  ReducedPtr analysis;
  GroupPath path;
  ExclusionState exclusion;
};

class Dataset {
 public:
  Dataset() = default;
  explicit Dataset(std::vector<DatasetItem> items) : items_(std::move(items)) {}

  const std::vector<DatasetItem>& items() const noexcept { return items_; }
  std::vector<DatasetItem>& mutable_items() noexcept { return items_; }  // for units building a new dataset
  std::size_t size() const noexcept { return items_.size(); }
  bool empty() const noexcept { return items_.empty(); }

  // Group names by group index (legend labels); graph names by graph index.
  std::vector<std::string> group_names, graph_names;

  // Distinct graph indices in ascending order, and the items of one graph
  // grouped by group index (ascending), each group in item order.
  std::vector<int> graphs() const;
  std::vector<std::pair<int, std::vector<const DatasetItem*>>> groups_of_graph(int graph) const;

  std::string group_name(int group) const;
  std::string graph_name(int graph) const;

  // Content fingerprint (design 7.2): analysis uuids, grouping, exclusion and
  // the reduction settings tag set by the reduce unit.
  std::string fingerprint() const;
  std::string reduction_tag;  // e.g. "arar-1/default/decay0"

 private:
  std::vector<DatasetItem> items_;
};

using DatasetPtr = std::shared_ptr<const Dataset>;

// Tags excluded by default (legacy EXCLUDE_TAGS without "skip").
const std::set<std::string>& default_excluded_tags();

}  // namespace pychron::processing
