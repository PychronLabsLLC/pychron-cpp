#include "pychron/experiment/plan/parameters.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

#include "plan_toml.hpp"
#include "pychron/core/number.hpp"

namespace pychron::experiment::plan {

namespace {

std::string trim(std::string_view s) {
  const auto b = s.find_first_not_of(" \t");
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(" \t");
  return std::string(s.substr(b, e - b + 1));
}

std::optional<toml::table> parse(const PlanTemplate& tmpl) {
  auto parsed = toml::parse(tmpl.text, tmpl.source);
  if (!parsed) return std::nullopt;
  return std::move(parsed).table();
}

bool string_array(const toml::array& a) {
  return std::all_of(a.begin(), a.end(), [](const toml::node& n) { return n.is_string(); });
}

// A leaf parameter for `node`, or nullopt when the node is not editable as one value.
std::optional<PlanParameter> leaf(const toml::node& node, const std::string& path) {
  PlanParameter p;
  p.path = path;
  p.label = path;
  if (auto b = node.value<bool>(); node.is_boolean() && b) {
    p.kind = ParamKind::Bool;
    p.value = *b;
  } else if (node.is_integer()) {
    p.kind = ParamKind::Int;
    p.value = *node.value<std::int64_t>();
  } else if (node.is_floating_point()) {
    p.kind = ParamKind::Float;
    p.value = *node.value<double>();
  } else if (node.is_string()) {
    const std::string s = *node.value<std::string>();
    p.kind = detail::is_alias(node) ? ParamKind::Alias : ParamKind::String;
    p.value = s;
  } else if (const auto* a = node.as_array(); a && string_array(*a)) {
    std::string joined;
    for (const auto& e : *a) joined += (joined.empty() ? "" : ", ") + *e.value<std::string>();
    p.kind = ParamKind::List;
    p.value = joined;
  } else {
    return std::nullopt;
  }
  return p;
}

// Every leaf at or under `node`.
void collect(const toml::node& node, const std::string& path, std::vector<PlanParameter>& out) {
  if (auto p = leaf(node, path)) {
    out.push_back(std::move(*p));
    return;
  }
  if (const auto* t = node.as_table()) {
    for (const auto& [k, v] : *t) collect(v, path.empty() ? std::string(k.str()) : path + "." + std::string(k.str()), out);
  } else if (const auto* a = node.as_array()) {
    for (std::size_t i = 0; i < a->size(); ++i) {
      const toml::node& e = *a->get(i);
      if (e.is_table() || e.is_array()) collect(e, path + "[" + std::to_string(i) + "]", out);
    }
  }
}

bool exposed(std::string_view path, const std::vector<ExposeEntry>& expose) {
  for (const auto& e : expose) {
    if (path == e.path) return true;
    if (path.size() > e.path.size() && path.starts_with(e.path) &&
        (path[e.path.size()] == '.' || path[e.path.size()] == '['))
      return true;
  }
  return false;
}

std::optional<double> number(std::string_view text) { return parse_double(trim(text)); }

std::optional<std::int64_t> integer(std::string_view text) {
  const std::string t = trim(text);
  if (t.empty()) return std::nullopt;
  std::int64_t v = 0;
  auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
  if (ec != std::errc{} || end != t.data() + t.size()) return std::nullopt;
  return v;
}

std::optional<bool> boolean(std::string_view text) {
  std::string t = trim(text);
  std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (t == "true" || t == "yes" || t == "1") return true;
  if (t == "false" || t == "no" || t == "0") return false;
  return std::nullopt;
}

}  // namespace

std::string_view to_string(ParamKind kind) noexcept {
  switch (kind) {
    case ParamKind::Bool: return "bool";
    case ParamKind::Int: return "int";
    case ParamKind::Float: return "float";
    case ParamKind::String: return "string";
    case ParamKind::List: return "list";
    case ParamKind::Alias: return "alias";
  }
  return "string";
}

Result<std::vector<PlanParameter>> plan_parameters(const PlanTemplate& tmpl, bool all) {
  auto root = parse(tmpl);
  if (!root) return fail(ErrorKind::Config, tmpl.source + ": does not parse");
  std::vector<PlanParameter> out;
  if (!all) {
    std::set<std::string> seen;
    for (const auto& e : tmpl.expose) {
      const toml::node* n = detail::find_node(*root, e.path);
      if (n == nullptr) continue;  // parse_plan_template already rejected unaddressable paths
      std::vector<PlanParameter> found;
      collect(*n, e.path, found);
      for (auto& p : found) {
        if (!seen.insert(p.path).second) continue;
        if (p.path == e.path && !e.label.empty()) p.label = e.label;
        p.exposed = true;
        out.push_back(std::move(p));
      }
    }
    return out;
  }
  for (const auto& [k, v] : *root) {
    const std::string key(k.str());
    if (key == "plan" || key == "parameters") continue;
    collect(v, key, out);
  }
  for (auto& p : out) {
    p.exposed = exposed(p.path, tmpl.expose);
    for (const auto& e : tmpl.expose)
      if (e.path == p.path && !e.label.empty()) p.label = e.label;
  }
  return out;
}

PlanInfo plan_info(const PlanTemplate& tmpl) {
  PlanInfo info;
  info.name = tmpl.name;
  auto root = parse(tmpl);
  if (!root) return info;
  const auto& plan = (*root)["plan"];
  info.instrument_family = plan["instrument_family"].value_or(std::string());
  info.description = plan["description"].value_or(std::string());
  if (const auto* types = plan["analysis_types"].as_array())
    for (const auto& t : *types)
      if (auto s = t.value<std::string>()) info.analysis_types.push_back(*s);
  return info;
}

std::vector<std::string> matching_plans(const PlanLibrary& plans, std::string_view family, AnalysisType type) {
  std::vector<std::string> out;
  const std::string type_name(experiment::to_string(type));
  for (const auto& name : plans.names()) {
    const PlanTemplate* t = plans.find(name);
    if (t == nullptr) continue;
    const PlanInfo info = plan_info(*t);
    if (!family.empty() && info.instrument_family != family) continue;
    if (!info.analysis_types.empty() &&
        std::find(info.analysis_types.begin(), info.analysis_types.end(), type_name) == info.analysis_types.end())
      continue;
    out.push_back(name);
  }
  return out;
}

std::vector<std::string> plan_families(const PlanLibrary& plans) {
  std::set<std::string> out;
  for (const auto& name : plans.names())
    if (const PlanTemplate* t = plans.find(name))
      if (auto f = plan_info(*t).instrument_family; !f.empty()) out.insert(f);
  return {out.begin(), out.end()};
}

std::optional<ParamValue> parse_param(ParamKind kind, std::string_view text) {
  switch (kind) {
    case ParamKind::Bool:
      if (auto b = boolean(text)) return *b;
      return std::nullopt;
    case ParamKind::Int:
      if (auto i = integer(text)) return *i;
      return std::nullopt;
    case ParamKind::Float:
      if (auto d = number(text)) return *d;
      return std::nullopt;
    case ParamKind::String:
    case ParamKind::List: return std::string(trim(text));
    case ParamKind::Alias: {
      if (auto i = integer(text)) return *i;
      if (auto d = number(text)) return *d;
      const std::string t = trim(text);
      if (t == "true" || t == "false") return t == "true";
      if (t.empty()) return std::nullopt;
      return t;
    }
  }
  return std::nullopt;
}

std::string format_param(const ParamValue& value) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::int64_t>) return std::to_string(v);
        else if constexpr (std::is_same_v<T, double>) {
          char buf[32];
          auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
          return ec == std::errc{} ? std::string(buf, end) : std::to_string(v);
        } else return v;
      },
      value);
}

bool same_param(const ParamValue& a, const ParamValue& b) {
  auto as_number = [](const ParamValue& v) -> std::optional<double> {
    if (auto* i = std::get_if<std::int64_t>(&v)) return static_cast<double>(*i);
    if (auto* d = std::get_if<double>(&v)) return *d;
    return std::nullopt;
  };
  if (auto x = as_number(a), y = as_number(b); x && y) return *x == *y;
  return a == b;
}

ParamOverrides overrides_for(const PlanTemplate& tmpl, const ParamOverrides& overrides, bool advanced) {
  ParamOverrides out;
  for (const auto& [path, value] : overrides)
    if (render_effective_plan(tmpl, {{path, value}}, LoadOptions{advanced})) out.emplace(path, value);
  return out;
}

}  // namespace pychron::experiment::plan
