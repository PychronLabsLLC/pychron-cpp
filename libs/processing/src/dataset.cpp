#include "pychron/processing/dataset.hpp"

#include <map>

#include "pychron/core/sha256.hpp"

namespace pychron::processing {

std::vector<int> Dataset::graphs() const {
  std::set<int> g;
  for (const auto& it : items_) g.insert(it.path.graph);
  return {g.begin(), g.end()};
}

std::vector<std::pair<int, std::vector<const DatasetItem*>>> Dataset::groups_of_graph(int graph) const {
  std::map<int, std::vector<const DatasetItem*>> by;
  for (const auto& it : items_)
    if (it.path.graph == graph) by[it.path.group].push_back(&it);
  return {by.begin(), by.end()};
}

std::string Dataset::group_name(int group) const {
  if (group >= 0 && static_cast<std::size_t>(group) < group_names.size() && !group_names[group].empty())
    return group_names[group];
  return "Group " + std::to_string(group + 1);
}

std::string Dataset::graph_name(int graph) const {
  if (graph >= 0 && static_cast<std::size_t>(graph) < graph_names.size()) return graph_names[graph];
  return {};
}

std::string Dataset::fingerprint() const {
  std::string text = "dataset/1\n" + reduction_tag + "\n";
  for (const auto& it : items_) {
    text += it.analysis && it.analysis->analysis ? it.analysis->analysis->uuid : std::string("-");
    text += ' ' + std::to_string(it.path.tab) + ',' + std::to_string(it.path.graph) + ',' +
            std::to_string(it.path.group) + ',' + std::to_string(it.path.subgroup);
    text += ' ';
    text += it.exclusion.user ? (*it.exclusion.user ? 'X' : 'I') : '-';
    text += it.exclusion.filter ? 'f' : '-';
    text += it.exclusion.tag ? 't' : '-';
    text += '\n';
  }
  for (const auto& n : group_names) text += "g " + n + '\n';
  for (const auto& n : graph_names) text += "G " + n + '\n';
  const auto digest = sha256(text);
  return pychron::to_hex(digest);
}

const std::set<std::string>& default_excluded_tags() {
  static const std::set<std::string> tags{"omit", "invalid", "outlier"};
  return tags;
}

}  // namespace pychron::processing
