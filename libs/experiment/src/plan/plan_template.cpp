// Template parsing, parameters.expose enforcement and effective-plan rendering.

#include <cmath>
#include <sstream>
#include <variant>

#include "pychron/experiment/plan/plan_loader.hpp"
#include "plan_toml.hpp"

namespace pychron::experiment::plan {
namespace {

using detail::Problems;

Result<toml::table> parse_toml(std::string_view text, std::string_view source) {
  auto parsed = toml::parse(text, source);
  if (!parsed) {
    const auto& e = parsed.error();
    return fail(ErrorKind::Config, std::string(source) + ": parse error: " + std::string(e.description()) + " (line " +
                                       std::to_string(e.source().begin.line) + ")");
  }
  return std::move(parsed).table();
}

bool is_exposed(std::string_view path, const std::vector<ExposeEntry>& expose) {
  for (const auto& e : expose) {
    if (path == e.path) return true;
    if (path.size() > e.path.size() && path.starts_with(e.path) &&
        (path[e.path.size()] == '.' || path[e.path.size()] == '['))
      return true;
  }
  return false;
}

std::vector<std::string> split_list(const std::string& s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    auto end = s.find(',', start);
    if (end == std::string::npos) end = s.size();
    auto item = s.substr(start, end - start);
    const auto b = item.find_first_not_of(" \t");
    const auto e = item.find_last_not_of(" \t");
    if (b != std::string::npos) out.push_back(item.substr(b, e - b + 1));
    start = end + 1;
  }
  return out;
}

// Where an override lands: a table key or an array element.
struct Slot {
  toml::table* table = nullptr;
  std::string key;
  toml::array* array = nullptr;
  std::size_t index = 0;

  const toml::node* current() const { return table ? table->get(key) : array->get(index); }
  template <class T>
  void put(T&& value) {
    if (table) table->insert_or_assign(key, std::forward<T>(value));
    else array->replace(array->cbegin() + static_cast<std::ptrdiff_t>(index), std::forward<T>(value));
  }
};

std::optional<Slot> locate(toml::table& root, std::vector<detail::PathSegment> segs, std::string& why) {
  auto last = segs.back();
  segs.pop_back();
  toml::node* parent = segs.empty() ? &root : detail::find_node(root, segs, &why);
  if (!parent) return std::nullopt;
  why = "no such path";
  if (last.index) {
    toml::node* n = parent;
    if (!last.key.empty()) n = parent->is_table() ? parent->as_table()->get(last.key) : nullptr;
    auto* arr = n ? n->as_array() : nullptr;
    if (!arr) return std::nullopt;
    if (*last.index >= arr->size()) {
      why = "index " + std::to_string(*last.index) + " out of range";
      return std::nullopt;
    }
    return Slot{nullptr, {}, arr, *last.index};
  }
  auto* t = parent->as_table();
  if (!t) return std::nullopt;
  // New keys may only be added to tables that already exist (e.g. fits.signal.Ar36).
  if (!t->get(last.key) && segs.empty()) return std::nullopt;
  return Slot{t, last.key, nullptr, 0};
}

// Type-checked write of `v` into `slot`; returns an error message on mismatch.
std::optional<std::string> assign(Slot& slot, const ParamValue& v) {
  const toml::node* cur = slot.current();
  const auto put_scalar = [&] { std::visit([&](const auto& x) { slot.put(x); }, v); };
  if (!cur || detail::is_alias(*cur)) {
    put_scalar();
    return std::nullopt;
  }
  switch (cur->type()) {
    case toml::node_type::boolean:
      if (!std::holds_alternative<bool>(v)) return "expected a boolean";
      slot.put(std::get<bool>(v));
      return std::nullopt;
    case toml::node_type::integer:
      if (auto* i = std::get_if<std::int64_t>(&v)) slot.put(*i);
      else if (auto* d = std::get_if<double>(&v); d && std::isfinite(*d) && std::trunc(*d) == *d)
        slot.put(static_cast<std::int64_t>(*d));
      else return "expected an integer";
      return std::nullopt;
    case toml::node_type::floating_point:
      if (auto* i = std::get_if<std::int64_t>(&v)) slot.put(static_cast<double>(*i));
      else if (auto* d = std::get_if<double>(&v)) slot.put(*d);
      else return "expected a number";
      return std::nullopt;
    case toml::node_type::string:
      if (!std::holds_alternative<std::string>(v)) return "expected a string";
      slot.put(std::get<std::string>(v));
      return std::nullopt;
    case toml::node_type::array: {
      const auto* s = std::get_if<std::string>(&v);
      const auto& arr = *cur->as_array();
      bool strings = true;
      for (const auto& e : arr) strings = strings && e.is_string();
      if (!s || !strings) return "expected a comma-separated list";
      toml::array out;
      for (auto& item : split_list(*s)) out.push_back(std::move(item));
      slot.put(std::move(out));
      return std::nullopt;
    }
    default:
      return "a table cannot be overridden; set its keys";
  }
}

std::string format(const toml::table& t) {
  std::ostringstream ss;
  // Full float precision: the effective plan is part of the record and must round-trip exactly.
  ss << toml::toml_formatter{t} << '\n';
  return ss.str();
}

}  // namespace

Result<PlanTemplate> parse_plan_template(std::string_view text, std::string_view source) {
  auto tbl = parse_toml(text, source);
  if (!tbl) return fail(tbl.error());
  Problems problems(source);
  PlanTemplate t;
  t.source = std::string(source);
  t.text = std::string(text);
  if (auto n = (*tbl)["plan"]["name"].value<std::string>()) t.name = *n;
  t.expose = detail::read_expose(*tbl, problems);
  if (problems.any()) return problems.error();
  return t;
}

Result<std::string> render_effective_plan(const PlanTemplate& tmpl, const ParamOverrides& overrides,
                                          const LoadOptions& options) {
  auto tbl = parse_toml(tmpl.text, tmpl.source);
  if (!tbl) return fail(tbl.error());
  Problems problems(tmpl.source);
  for (const auto& [path, value] : overrides) {
    auto segs = detail::parse_path(path);
    if (!segs) {
      problems.add(path, "invalid path");
      continue;
    }
    if (!options.advanced && !is_exposed(path, tmpl.expose)) {
      problems.add(path, "not exposed in parameters.expose");
      continue;
    }
    std::string why;
    auto slot = locate(*tbl, *segs, why);
    if (!slot) {
      problems.add(path, why);
      continue;
    }
    if (auto err = assign(*slot, value)) problems.add(path, *err);
  }
  if (problems.any()) return problems.error();
  return format(*tbl);
}

Result<LoadedPlan> load_plan(const PlanTemplate& tmpl, const ParamOverrides& overrides,
                             const PlanResolvers& resolvers, const LoadOptions& options) {
  auto text = render_effective_plan(tmpl, overrides, options);
  if (!text) return fail(text.error());
  auto plan = resolve_plan(*text, resolvers, tmpl.source);
  if (!plan) return fail(plan.error());
  return LoadedPlan{std::move(*plan), std::move(*text)};
}

}  // namespace pychron::experiment::plan
