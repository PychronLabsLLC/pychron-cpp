#include "plan_toml.hpp"

#include <cctype>

namespace pychron::experiment::plan::detail {

std::optional<std::vector<PathSegment>> parse_path(std::string_view path) {
  std::vector<PathSegment> out;
  std::size_t i = 0;
  while (i < path.size()) {
    std::size_t start = i;
    while (i < path.size() && path[i] != '.' && path[i] != '[') ++i;
    if (i == start) return std::nullopt;
    out.push_back({std::string(path.substr(start, i - start)), std::nullopt});
    while (i < path.size() && path[i] == '[') {
      std::size_t close = path.find(']', i);
      if (close == std::string_view::npos || close == i + 1) return std::nullopt;
      std::size_t idx = 0;
      for (std::size_t j = i + 1; j < close; ++j) {
        if (!std::isdigit(static_cast<unsigned char>(path[j]))) return std::nullopt;
        idx = idx * 10 + static_cast<std::size_t>(path[j] - '0');
      }
      // Nested indices ("a[0][1]") become key-less segments.
      if (out.back().index) out.push_back({"", idx});
      else out.back().index = idx;
      i = close + 1;
    }
    if (i < path.size()) {
      if (path[i] != '.' || i + 1 == path.size()) return std::nullopt;
      ++i;
    }
  }
  if (out.empty()) return std::nullopt;
  return out;
}

toml::node* find_node(toml::table& root, const std::vector<PathSegment>& path, std::string* why) {
  auto miss = [&](std::string msg) -> toml::node* {
    if (why) *why = std::move(msg);
    return nullptr;
  };
  toml::node* cur = &root;
  for (const auto& seg : path) {
    if (!seg.key.empty()) {
      auto* t = cur->as_table();
      if (!t) return miss("no such path");
      cur = t->get(seg.key);
      if (!cur) return miss("no such path");
    }
    if (seg.index) {
      auto* a = cur->as_array();
      if (!a) return miss("no such path");
      if (*seg.index >= a->size()) return miss("index " + std::to_string(*seg.index) + " out of range");
      cur = a->get(*seg.index);
    }
  }
  return cur;
}

const toml::node* find_node(const toml::table& root, std::string_view path) {
  auto p = parse_path(path);
  if (!p) return nullptr;
  // find_node does not modify; the const_cast only shares the traversal.
  return find_node(const_cast<toml::table&>(root), *p);
}

bool path_addressable(const toml::table& root, std::string_view path) {
  auto segs = parse_path(path);
  if (!segs) return false;
  auto& mroot = const_cast<toml::table&>(root);
  if (find_node(mroot, *segs)) return true;
  if (segs->back().index) return false;
  segs->pop_back();
  if (segs->empty()) return false;
  auto* parent = find_node(mroot, *segs);
  return parent && parent->is_table();
}

std::vector<ExposeEntry> read_expose(const toml::table& root, Problems& problems) {
  std::vector<ExposeEntry> out;
  const auto* params = root.get("parameters");
  if (!params) return out;
  const auto* pt = params->as_table();
  if (!pt) {
    problems.add("parameters", "expected a table");
    return out;
  }
  for (const auto& [k, v] : *pt)
    if (k.str() != "expose") problems.add("parameters", "unknown key '" + std::string(k.str()) + "'");
  const auto* ex = pt->get("expose");
  if (!ex) return out;
  const auto* arr = ex->as_array();
  if (!arr) {
    problems.add("parameters.expose", "expected an array");
    return out;
  }
  for (std::size_t i = 0; i < arr->size(); ++i) {
    const auto where = join("parameters.expose", i);
    const auto& e = *arr->get(i);
    ExposeEntry entry;
    if (auto s = e.value<std::string>()) {
      entry.path = *s;
    } else if (auto* t = e.as_table()) {
      bool ok = true;
      for (const auto& [k, v] : *t)
        if (k.str() != "path" && k.str() != "label") {
          problems.add(where, "unknown key '" + std::string(k.str()) + "'");
          ok = false;
        }
      auto path = t->get("path") ? t->get("path")->value<std::string>() : std::nullopt;
      if (!path) {
        problems.add(where, "'path' must be a string");
        continue;
      }
      entry.path = *path;
      if (auto* l = t->get("label")) {
        if (auto label = l->value<std::string>()) entry.label = *label;
        else {
          problems.add(where, "'label' must be a string");
          ok = false;
        }
      }
      if (!ok) continue;
    } else {
      problems.add(where, "expected a path string or {path, label} table");
      continue;
    }
    if (!path_addressable(root, entry.path)) {
      problems.add(where, "no such path '" + entry.path + "'");
      continue;
    }
    if (entry.label.empty()) entry.label = entry.path;
    out.push_back(std::move(entry));
  }
  return out;
}

std::string join(std::string_view base, std::string_view key) {
  if (base.empty()) return std::string(key);
  return std::string(base) + "." + std::string(key);
}

std::string join(std::string_view base, std::size_t index) {
  return std::string(base) + "[" + std::to_string(index) + "]";
}

bool is_alias(const toml::node& n) {
  auto* s = n.as_string();
  return s && !s->get().empty() && s->get().front() == '@';
}

}  // namespace pychron::experiment::plan::detail
