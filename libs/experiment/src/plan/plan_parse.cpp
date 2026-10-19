// Effective plan TOML -> resolved, validated MeasurementPlan.

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "pychron/experiment/conditionals/conditional.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"
#include "plan_desugar.hpp"
#include "plan_parse.hpp"
#include "plan_toml.hpp"

namespace pychron::experiment::plan {
namespace {

using detail::join;
using detail::Problems;
using detail::V1Baseline;
using detail::V1Hop;
using detail::V1HopPosition;
using detail::V1Main;
using detail::V1PeakCenter;
using detail::V1Sniff;
using detail::V1Tables;
using detail::V1Whiff;

// ---- '@' alias resolution -------------------------------------------------------

class AliasPass {
 public:
  AliasPass(const toml::table& original, const PlanResolvers& r, Problems& p) : orig_(original), r_(r), p_(p) {}

  void run(toml::table& t, const std::string& where) {
    std::vector<std::pair<std::string, ParamValue>> repl;
    for (auto&& [k, v] : t) {
      const auto path = join(where, k.str());
      if (path == "conditionals.include") continue;  // conditional-set references, not values
      if (detail::is_alias(v)) {
        if (auto val = resolve(v.as_string()->get(), path)) repl.emplace_back(std::string(k.str()), *val);
      } else {
        descend(v, path);
      }
    }
    for (auto& [k, val] : repl) std::visit([&](auto& x) { t.insert_or_assign(k, x); }, val);
  }

 private:
  void run(toml::array& a, const std::string& where) {
    for (std::size_t i = 0; i < a.size(); ++i) {
      auto& v = *a.get(i);
      const auto path = join(where, i);
      if (detail::is_alias(v)) {
        if (auto val = resolve(v.as_string()->get(), path))
          std::visit([&](auto& x) { a.replace(a.cbegin() + static_cast<std::ptrdiff_t>(i), x); }, *val);
      } else {
        descend(v, path);
      }
    }
  }

  void descend(toml::node& v, const std::string& path) {
    if (auto* t = v.as_table()) run(*t, path);
    else if (auto* a = v.as_array()) run(*a, path);
  }

  std::optional<ParamValue> resolve(const std::string& alias, const std::string& where) {
    std::vector<std::string> chain;
    return follow(alias, where, chain);
  }

  std::optional<ParamValue> follow(const std::string& alias, const std::string& where, std::vector<std::string>& chain) {
    if (std::find(chain.begin(), chain.end(), alias) != chain.end()) {
      std::string msg = "alias cycle: ";
      for (const auto& c : chain) msg += c + " -> ";
      p_.add(where, msg + alias);
      return std::nullopt;
    }
    chain.push_back(alias);
    const std::string key = alias.substr(1);
    if (const auto* n = detail::find_node(orig_, key)) {
      if (detail::is_alias(*n)) return follow(n->as_string()->get(), where, chain);
      if (auto b = n->value_exact<bool>()) return *b;
      if (auto i = n->value_exact<std::int64_t>()) return *i;
      if (auto d = n->value_exact<double>()) return *d;
      if (auto s = n->value_exact<std::string>()) return *s;
      p_.add(where, "alias '" + alias + "' does not name a value");
      return std::nullopt;
    }
    if (r_.extraction_line)
      if (auto v = r_.extraction_line->resolve_alias(key)) return v;
    if (r_.spectrometer)
      if (auto v = r_.spectrometer->resolve_alias(key)) return v;
    p_.add(where, "unresolved alias '" + alias + "'");
    return std::nullopt;
  }

  const toml::table& orig_;
  const PlanResolvers& r_;
  Problems& p_;
};

// ---- typed reading ----------------------------------------------------------------

class Section {
 public:
  Section(const toml::table* t, std::string path, Problems& p) : t_(t), path_(std::move(path)), p_(p) {}

  // A sub-table; a missing key yields an empty section, a non-table an error.
  Section sub(std::string_view key) const {
    const auto* n = node(key);
    if (n && !n->is_table()) p_.add(at(key), "expected a table");
    return Section(n ? n->as_table() : nullptr, at(key), p_);
  }

  void keys(std::initializer_list<std::string_view> allowed) const {
    if (!t_) return;
    for (const auto& [k, v] : *t_)
      if (std::find(allowed.begin(), allowed.end(), k.str()) == allowed.end())
        p_.add(path_, "unknown key '" + std::string(k.str()) + "'");
  }

  bool has(std::string_view key) const { return node(key) != nullptr; }
  const toml::node* node(std::string_view key) const { return t_ ? t_->get(key) : nullptr; }
  const toml::table* table() const { return t_; }
  std::string at(std::string_view key) const { return join(path_, key); }
  const std::string& path() const { return path_; }
  Problems& problems() const { return p_; }

  void get(std::string_view key, std::string& out) const {
    if (const auto* n = node(key)) {
      if (auto v = n->value_exact<std::string>()) out = *v;
      else p_.add(at(key), "expected a string");
    }
  }
  void get(std::string_view key, std::optional<std::string>& out) const {
    if (has(key)) get(key, out.emplace());
  }
  void get(std::string_view key, bool& out) const {
    if (const auto* n = node(key)) {
      if (auto v = n->value_exact<bool>()) out = *v;
      else p_.add(at(key), "expected a boolean");
    }
  }
  void get(std::string_view key, int& out) const {
    if (const auto* n = node(key)) {
      if (auto i = n->value_exact<std::int64_t>()) out = static_cast<int>(*i);
      else if (auto d = n->value_exact<double>(); d && std::isfinite(*d) && std::trunc(*d) == *d)
        out = static_cast<int>(*d);
      else p_.add(at(key), "expected an integer");
    }
  }
  void get(std::string_view key, double& out) const {
    if (const auto* n = node(key)) {
      if (auto i = n->value_exact<std::int64_t>()) out = static_cast<double>(*i);
      else if (auto d = n->value_exact<double>()) out = *d;
      else p_.add(at(key), "expected a number");
    }
  }
  void get(std::string_view key, std::optional<double>& out) const {
    if (has(key)) get(key, out.emplace());
  }
  void get(std::string_view key, std::vector<std::string>& out) const {
    const auto* n = node(key);
    if (!n) return;
    const auto* a = n->as_array();
    bool ok = a != nullptr;
    if (a) {
      out.clear();
      for (const auto& e : *a) {
        if (auto s = e.value_exact<std::string>()) out.push_back(*s);
        else ok = false;
      }
    }
    if (!ok) p_.add(at(key), "expected an array of strings");
  }

  void at_least(std::string_view key, double v, double min, std::string_view suffix = {}) const {
    if (v < min) report(key, ">= ", min, suffix);
  }
  void above(std::string_view key, double v, double min) const {
    if (!(v > min)) report(key, "> ", min, {});
  }

 private:
  void report(std::string_view key, std::string_view op, double bound, std::string_view suffix) const {
    std::string b = std::to_string(bound);
    b.erase(b.find_last_not_of('0') + 1);
    if (b.back() == '.') b.pop_back();
    std::string msg = "must be " + std::string(op) + b;
    if (!suffix.empty()) msg += " " + std::string(suffix);
    p_.add(at(key), msg);
  }

  const toml::table* t_;
  std::string path_;
  Problems& p_;
};

// Each table of the array of tables at `key`, as a Section at `key[i]`.
// Returns the number of tables read; a missing key reads none.
template <class F>
std::size_t for_each_table(const Section& s, std::string_view key, F&& f) {
  const auto* n = s.node(key);
  if (!n) return 0;
  const auto* arr = n->as_array();
  if (!arr) {
    s.problems().add(s.at(key), "expected an array of tables");
    return 0;
  }
  std::size_t read = 0;
  for (std::size_t i = 0; i < arr->size(); ++i) {
    const auto where = join(s.at(key), i);
    const auto* t = arr->get(i)->as_table();
    if (!t) {
      s.problems().add(where, "expected a table");
      continue;
    }
    f(Section(t, where, s.problems()), i);
    ++read;
  }
  return read;
}

void read_info(const Section& s, PlanInfo& out) {
  s.keys({"name", "instrument_family", "description", "analysis_types"});
  s.get("name", out.name);
  s.get("instrument_family", out.instrument_family);
  s.get("description", out.description);
  s.get("analysis_types", out.analysis_types);
  if (out.name.empty()) s.problems().add(s.at("name"), "required");
  if (out.instrument_family.empty()) s.problems().add(s.at("instrument_family"), "required");
}

void read_detectors(const Section& s, DetectorsSpec& out) {
  s.keys({"reference", "exclude"});
  s.get("reference", out.reference);
  s.get("exclude", out.exclude);
}

void read_equilibration(const Section& s, Equilibration& out) {
  s.keys({"inlet", "outlet", "time_s", "inlet_delay_s", "close_inlet"});
  s.get("inlet", out.inlet);
  s.get("outlet", out.outlet);
  s.get("time_s", out.time_s);
  s.get("inlet_delay_s", out.inlet_delay_s);
  s.get("close_inlet", out.close_inlet);
  s.at_least("time_s", out.time_s, 0);
  s.at_least("inlet_delay_s", out.inlet_delay_s, 0);
}

void read_sniff(const Section& s, V1Sniff& out) {
  s.keys({"enabled", "counts", "integration_s"});
  s.get("enabled", out.enabled);
  s.get("counts", out.counts);
  s.get("integration_s", out.integration_s);
  if (out.enabled) s.at_least("counts", out.counts, 1, "when enabled");
  s.above("integration_s", out.integration_s, 0);
}

void read_peak_center(const Section& s, V1PeakCenter& out) {
  s.keys({"before", "after", "detector", "isotope", "config", "min_signal"});
  s.get("before", out.before);
  s.get("after", out.after);
  s.get("detector", out.detector);
  s.get("isotope", out.isotope);
  s.get("config", out.config);
  s.get("min_signal", out.min_signal);
  s.at_least("min_signal", out.min_signal, 0);
  if ((out.before || out.after) && out.isotope.empty())
    s.problems().add(s.at("isotope"), "required when before or after");
}

void read_baseline(const Section& s, V1Baseline& out) {
  s.keys({"before", "after", "counts", "mass", "detector", "settle_s", "integration_s"});
  s.get("before", out.before);
  s.get("after", out.after);
  s.get("counts", out.counts);
  s.get("mass", out.mass);
  s.get("detector", out.detector);
  s.get("settle_s", out.settle_s);
  s.get("integration_s", out.integration_s);
  if (out.before || out.after) s.at_least("counts", out.counts, 1, "when before or after");
  if (out.mass) s.above("mass", *out.mass, 0);
  s.at_least("settle_s", out.settle_s, 0);
  s.above("integration_s", out.integration_s, 0);
}

// `time_zero` of a v1 [main] or a v2 collection segment or gas, or (`step`)
// of a v2 `time_zero` step, where "now" replaces the two inlet-relative strings.
void read_time_zero(const Section& s, TimeZero& out, bool step = false) {
  const auto* n = s.node("time_zero");
  if (!n) return;
  const std::string expected =
      step ? "expected 'now' or { offset_s = N }" : "expected 'on_inlet_close', 'on_first_count' or { offset_s = N }";
  if (auto str = n->value_exact<std::string>()) {
    if (step && *str == "now") out.kind = TimeZeroKind::Now;
    else if (!step && *str == "on_inlet_close") out.kind = TimeZeroKind::OnInletClose;
    else if (!step && *str == "on_first_count") out.kind = TimeZeroKind::OnFirstCount;
    else s.problems().add(s.at("time_zero"), expected);
  } else if (n->is_table()) {
    Section tz(n->as_table(), s.at("time_zero"), s.problems());
    tz.keys({"offset_s"});
    if (!tz.has("offset_s")) s.problems().add(s.at("time_zero"), expected);
    out.kind = TimeZeroKind::Offset;
    tz.get("offset_s", out.offset_s);
    tz.at_least("offset_s", out.offset_s, 0);
  } else {
    s.problems().add(s.at("time_zero"), expected);
  }
}

void read_hop(const Section& s, const DetectorsSpec& dets, V1Hop& hop) {
  s.keys({"positions", "counts", "settle_s", "protect", "baseline", "mass", "position"});
  auto& p = s.problems();
  const auto pos = s.sub("positions");
  if (pos.table()) {
    std::map<std::string, std::string> by_detector;  // detector -> isotope
    for (const auto& [k, v] : *pos.table()) {
      const std::string iso(k.str());
      auto det = v.value_exact<std::string>();
      if (!det) {
        p.add(pos.at(iso), "expected a string");
        continue;
      }
      if (auto [it, fresh] = by_detector.emplace(*det, iso); !fresh)
        p.add(pos.path(), "detector '" + *det + "' assigned to both '" + it->second + "' and '" + iso + "'");
      hop.positions[iso] = *det;
    }
  }
  if (hop.positions.empty() && (pos.table() || !s.has("positions")))
    p.add(pos.path(), "at least one isotope required");
  s.get("counts", hop.counts);
  s.get("settle_s", hop.settle_s);
  s.get("protect", hop.protect);
  s.get("baseline", hop.baseline);
  s.get("mass", hop.mass);
  s.at_least("counts", hop.counts, 1);
  s.at_least("settle_s", hop.settle_s, 0);
  if (hop.mass) {
    if (!hop.baseline) p.add(s.at("mass"), "only allowed on baseline hops");
    else s.above("mass", *hop.mass, 0);
  }

  if (s.has("position")) {
    const auto ps = s.sub("position");
    ps.keys({"isotope", "detector"});
    V1HopPosition hp;
    ps.get("isotope", hp.isotope);
    ps.get("detector", hp.detector);
    if (hp.detector.empty()) p.add(ps.at("detector"), "required");
    if (!hp.isotope.empty() && !hop.positions.contains(hp.isotope))
      p.add(ps.at("isotope"), "'" + hp.isotope + "' not in hop positions");
    if (hp.isotope.empty() && !(hop.baseline && hop.mass))
      p.add(ps.path(), "needs 'isotope' unless the hop is a baseline hop at 'mass'");
    hop.position = hp;
  } else if (!hop.positions.empty()) {
    const bool has_ref = std::any_of(hop.positions.begin(), hop.positions.end(),
                                     [&](const auto& kv) { return kv.second == dets.reference; });
    if (!has_ref) p.add(s.path(), "reference detector '" + dets.reference + "' not in hop and no 'position'");
  }

  if (!hop.positions.empty()) {
    const bool any_active = std::any_of(hop.positions.begin(), hop.positions.end(), [&](const auto& kv) {
      return std::find(dets.exclude.begin(), dets.exclude.end(), kv.second) == dets.exclude.end();
    });
    if (!any_active) p.add(s.path(), "every detector excluded");
  }
}

void read_main(const Section& s, const DetectorsSpec& dets, V1Main& out) {
  s.keys({"cycles", "integration_s", "time_zero", "hops"});
  s.get("cycles", out.cycles);
  s.get("integration_s", out.integration_s);
  s.at_least("cycles", out.cycles, 1);
  s.above("integration_s", out.integration_s, 0);
  read_time_zero(s, out.time_zero);
  const auto* hops = s.node("hops");
  const auto* arr = hops ? hops->as_array() : nullptr;
  if (hops && !arr) {
    s.problems().add(s.at("hops"), "expected an array of tables");
    return;
  }
  if (!arr || arr->empty()) {
    s.problems().add(s.at("hops"), "at least one hop required");
    return;
  }
  for (std::size_t i = 0; i < arr->size(); ++i) {
    const auto where = join(s.at("hops"), i);
    const auto* t = arr->get(i)->as_table();
    if (!t) {
      s.problems().add(where, "expected a table");
      continue;
    }
    read_hop(Section(t, where, s.problems()), dets, out.hops.emplace_back());
  }
}

void read_fit_map(const Section& s, std::string_view key, std::map<std::string, reduction::FitKind>& out) {
  const auto* n = s.node(key);
  if (!n) return;
  const auto set = [&](const std::string& name, const toml::node& v, const std::string& where) {
    auto str = v.value_exact<std::string>();
    if (!str) return s.problems().add(where, "expected a string");
    if (auto kind = reduction::parse_fit_kind(*str)) out[name] = *kind;
    else s.problems().add(where, "unknown fit '" + *str + "'");
  };
  if (n->is_string()) return set("default", *n, s.at(key));
  const auto* t = n->as_table();
  if (!t) return s.problems().add(s.at(key), "expected a fit name or {isotope = fit} table");
  for (const auto& [k, v] : *t) set(std::string(k.str()), v, join(s.at(key), k.str()));
}

void read_fits(const Section& s, Fits& out) {
  s.keys({"signal", "baseline", "error", "outliers"});
  read_fit_map(s, "signal", out.signal);
  read_fit_map(s, "baseline", out.baseline);
  std::string err;
  s.get("error", err);
  if (err == "sem") out.error = reduction::ErrorType::Sem;
  else if (err == "sd") out.error = reduction::ErrorType::Sd;
  else if (!err.empty()) s.problems().add(s.at("error"), "expected 'sem' or 'sd'");
  const auto o = s.sub("outliers");
  o.keys({"enabled", "iterations", "std_devs"});
  o.get("enabled", out.outliers.enabled);
  o.get("iterations", out.outliers.iterations);
  o.get("std_devs", out.outliers.std_devs);
  o.at_least("iterations", out.outliers.iterations, 0);
  o.above("std_devs", out.outliers.std_devs, 0);
}

void read_truncations(const Section& s, std::vector<Truncation>& out) {
  for_each_table(s, "truncations", [&](const Section& ts, std::size_t) {
    ts.keys({"check", "start"});
    auto& tr = out.emplace_back();
    ts.get("check", tr.check);
    ts.get("start", tr.start);
    if (tr.check.empty()) s.problems().add(ts.at("check"), "required");
    ts.at_least("start", tr.start, 0);
  });
}

void read_conditionals(const Section& s, PlanConditionals& out) {
  s.keys({"include", "truncations"});
  s.get("include", out.include);
  read_truncations(s, out.truncations);
}

void read_checks(const Section& s, std::vector<WhiffCheck>& out) {
  for_each_table(s, "checks", [&](const Section& cs, std::size_t) {
    cs.keys({"check", "action"});
    auto& c = out.emplace_back();
    cs.get("check", c.check);
    cs.get("action", c.action);
    if (c.check.empty()) {
      s.problems().add(cs.at("check"), "required");
    } else if (auto e = parse_expression(c.check); !e) {
      s.problems().add(cs.at("check"), e.error().what);
    }
    if (!parse_whiff_action(c.action)) s.problems().add(cs.at("action"), "expected run_remainder, pump or abort");
  });
}

void read_whiff(const Section& s, V1Whiff& out) {
  s.keys({"enabled", "counts", "integration_s", "checks"});
  s.get("enabled", out.enabled);
  s.get("counts", out.counts);
  s.get("integration_s", out.integration_s);
  s.above("integration_s", out.integration_s, 0);
  read_checks(s, out.checks);
  if (s.has("checks") && !s.node("checks")->is_array()) return;
  if (out.enabled) {
    if (out.counts < 1) s.problems().add(s.at("counts"), "an enabled whiff needs counts >= 1");
    if (out.checks.empty()) s.problems().add(s.at("checks"), "an enabled whiff needs at least one check");
  }
}

// ---- v2: gases, segments, positions (v2 spec 3.2-3.4) ------------------------------

// `key` = { name = number, ... } (a step's deflection, gain or source table).
void read_number_map(const Section& s, std::string_view key, std::map<std::string, double>& out) {
  const auto m = s.sub(key);
  if (!m.table()) return;
  for (const auto& [k, v] : *m.table()) {
    const std::string name(k.str());
    double value = 0;
    m.get(name, value);
    if (v.is_number()) out[name] = value;
  }
}

// `collect`: the list form [{ label, detector, active, deflection, protect }, ...]
// or the shorthand map { label = "detector", ... }. The map is read in the
// order toml++ iterates a table, which is key order, not the order written.
void read_collect(const Section& s, std::vector<CollectEntry>& out) {
  const auto* n = s.node("collect");
  if (!n) return s.problems().add(s.at("collect"), "required");
  if (const auto* map = n->as_table()) {
    const Section m(map, s.at("collect"), s.problems());
    for (const auto& [k, v] : *map) {
      CollectEntry e;
      e.label = std::string(k.str());
      m.get(e.label, e.detector);
      if (!v.is_string()) continue;
      if (e.detector.empty()) m.problems().add(m.at(e.label), "required");
      out.push_back(std::move(e));
    }
  } else if (n->is_array()) {
    for_each_table(s, "collect", [&](const Section& es, std::size_t) {
      es.keys({"label", "detector", "active", "deflection", "protect"});
      CollectEntry e;
      es.get("label", e.label);
      es.get("detector", e.detector);
      es.get("active", e.active);
      es.get("deflection", e.deflection);
      es.get("protect", e.protect);
      if (e.detector.empty()) s.problems().add(es.at("detector"), "required");
      out.push_back(std::move(e));
    });
  } else {
    return s.problems().add(s.at("collect"), "expected { label = \"detector\" } or an array of tables");
  }
  if (out.empty()) s.problems().add(s.at("collect"), "at least one entry required");
}

// `at`: exactly one of label, mass, dac or hv; a detector with all but dac.
void read_at(const Section& s, std::optional<At>& out) {
  if (!s.has("at")) return;
  const auto a = s.sub("at");
  if (!a.table()) return;
  a.keys({"label", "mass", "dac", "hv", "detector"});
  const int forms = static_cast<int>(a.has("label")) + static_cast<int>(a.has("mass")) +
                    static_cast<int>(a.has("dac")) + static_cast<int>(a.has("hv"));
  if (forms != 1) return a.problems().add(a.path(), "expected exactly one of 'label', 'mass', 'dac' or 'hv'");
  std::string detector;
  a.get("detector", detector);
  if (a.has("dac")) {
    if (a.has("detector")) a.problems().add(a.at("detector"), "not allowed with 'dac'");
    AtDac d;
    a.get("dac", d.dac);
    out = d;
    return;
  }
  if (detector.empty()) a.problems().add(a.at("detector"), "required");
  if (a.has("label")) {
    AtLabel l;
    a.get("label", l.label);
    l.detector = detector;
    out = l;
  } else if (a.has("mass")) {
    AtMass m;
    a.get("mass", m.mass);
    a.above("mass", m.mass, 0);
    m.detector = detector;
    out = m;
  } else {
    AtHv h;
    a.get("hv", h.hv);
    h.detector = detector;
    out = h;
  }
}

void read_position(const Section& s, Position& out) {
  s.keys({"collect", "at", "kind", "counts", "settle_s", "integration_s", "optics", "table"});
  read_collect(s, out.collect);
  read_at(s, out.at);
  std::string kind;
  s.get("kind", kind);
  if (kind == "baseline") out.kind = PositionKind::Baseline;
  else if (!kind.empty() && kind != "signal") s.problems().add(s.at("kind"), "expected 'signal' or 'baseline'");
  s.get("counts", out.counts);
  s.get("settle_s", out.settle_s);
  s.get("integration_s", out.integration_s);
  s.get("optics", out.optics);
  s.get("table", out.table);
  s.at_least("counts", out.counts, 1);
  s.at_least("settle_s", out.settle_s, 0);
  if (out.integration_s) s.above("integration_s", *out.integration_s, 0);
}

std::optional<reduction::ErrorType> read_error_type(const Section& s) {
  std::string err;
  s.get("error", err);
  if (err == "sem") return reduction::ErrorType::Sem;
  if (err == "sd") return reduction::ErrorType::Sd;
  if (!err.empty()) s.problems().add(s.at("error"), "expected 'sem' or 'sd'");
  return std::nullopt;
}

// A segment's `fits`: { "<label@detector>" | "<label>" | "default" = { fit, error, outliers } }.
void read_fit_overrides(const Section& s, FitOverrides& out) {
  const auto f = s.sub("fits");
  if (!f.table()) return;
  for (const auto& [k, v] : *f.table()) {
    const std::string key(k.str());
    const auto o = f.sub(key);
    if (!o.table()) continue;
    o.keys({"fit", "error", "outliers"});
    FitOverride fo;
    std::string fit;
    o.get("fit", fit);
    if (auto kind = reduction::parse_fit_kind(fit)) fo.fit = *kind;
    else if (!fit.empty()) o.problems().add(o.at("fit"), "unknown fit '" + fit + "'");
    fo.error = read_error_type(o);
    if (o.has("outliers")) {
      const auto ol = o.sub("outliers");
      ol.keys({"enabled", "iterations", "std_devs"});
      auto& spec = fo.outliers.emplace();
      ol.get("enabled", spec.enabled);
      ol.get("iterations", spec.iterations);
      ol.get("std_devs", spec.std_devs);
      ol.at_least("iterations", spec.iterations, 0);
      ol.above("std_devs", spec.std_devs, 0);
    }
    out[key] = fo;
  }
}

void read_peak_center_step(const Section& s, const DetectorsSpec& dets, PeakCenterStep& out) {
  s.keys({"label", "detector", "detectors", "extra_detectors", "config", "write_table", "min_signal"});
  s.get("label", out.label);
  const bool one = s.has("detector");
  const bool several = s.has("detectors");
  if (one && several) s.problems().add(s.path(), "'detector' and 'detectors' cannot both appear");
  if (one) {
    std::string det;
    s.get("detector", det);
    out.detectors = {det};
  }
  s.get("detectors", out.detectors);
  if (!one && !several && !dets.reference.empty()) out.detectors = {dets.reference};  // as v1
  s.get("extra_detectors", out.extra_detectors);
  s.get("config", out.config);
  s.get("write_table", out.write_table);
  s.get("min_signal", out.min_signal);
  s.at_least("min_signal", out.min_signal, 0);
  if (out.label.empty()) s.problems().add(s.at("label"), "required");
}

void read_set_step(const Section& s, SetStep& out) {
  s.keys({"deflection", "gain", "hv", "cdd_voltage", "source", "integration_s"});
  read_number_map(s, "deflection", out.deflection);
  read_number_map(s, "gain", out.gain);
  read_number_map(s, "source", out.source);
  s.get("hv", out.hv);
  s.get("cdd_voltage", out.cdd_voltage);
  s.get("integration_s", out.integration_s);
  if (out.integration_s) s.above("integration_s", *out.integration_s, 0);
}

void read_park_step(const Section& s, ParkStep& out) {
  s.keys({"mass", "label", "detector"});
  if (s.has("mass") == s.has("label")) s.problems().add(s.path(), "expected exactly one of 'mass' or 'label'");
  s.get("label", out.label);
  s.get("mass", out.mass);
  if (out.mass) s.above("mass", *out.mass, 0);
  s.get("detector", out.detector);
  if (out.detector.empty()) s.problems().add(s.at("detector"), "required");
}

// A segment of a collection kind reads positions, cycles and the rest; a
// step reads the one table named after its kind.
void read_segment(const Section& s, const DetectorsSpec& dets, Segment& out) {
  s.keys({"kind", "when", "cycles", "integration_s", "time_zero", "table", "positions", "fits", "conditionals",
          "checks", "peak_center", "set", "blank_beam", "park", "delay"});
  auto& p = s.problems();
  if (s.has("kind")) {
    std::string kind;
    s.get("kind", kind);
    if (auto k = parse_segment_kind(kind)) out.kind = *k;
    else
      p.add(s.at("kind"),
            "expected signal, baseline, sniff, whiff, peak_center, set, blank_beam, park, delay or time_zero");
  }
  if (s.has("when")) {
    std::string when;
    s.get("when", when);
    if (auto w = parse_when(when)) out.when = *w;
    else
      p.add(s.at("when"),
            "expected during_extraction, before_inlet, during_equilibration, after_inlet, after_main or after_run");
  }
  const bool step = is_step(out.kind);
  const std::string kind_name(to_string(out.kind));

  if (step) {
    for (const std::string_view key : {"positions", "cycles", "integration_s", "table", "fits", "conditionals",
                                       "checks"})
      if (s.has(key)) p.add(s.at(key), "not allowed on a " + kind_name + " step");
  } else {
    s.get("cycles", out.cycles);
    s.get("integration_s", out.integration_s);
    s.get("table", out.table);
    s.at_least("cycles", out.cycles, 1);
    s.above("integration_s", out.integration_s, 0);
    const auto read = for_each_table(
        s, "positions", [&](const Section& ps, std::size_t) { read_position(ps, out.positions.emplace_back()); });
    if (read == 0 && (!s.has("positions") || s.node("positions")->is_array()))
      p.add(s.at("positions"), "at least one position required");
    read_fit_overrides(s, out.fits);
    if (s.has("conditionals")) {
      const auto c = s.sub("conditionals");
      c.keys({"truncations"});
      read_truncations(c, out.truncations);
    }
    if (s.has("checks") && out.kind != SegmentKind::Whiff) p.add(s.at("checks"), "only allowed on whiff segments");
    else read_checks(s, out.checks);
  }

  // Each step table belongs to the step of its name, and that step needs it.
  const auto step_table = [&](std::string_view key, SegmentKind kind) {
    if (s.has(key) && out.kind != kind) {
      p.add(s.at(key), "only allowed on a " + std::string(key) + " step");
      return false;
    }
    if (!s.has(key) && out.kind == kind) p.add(s.at(key), "required");
    return s.has(key);
  };
  if (step_table("peak_center", SegmentKind::PeakCenter))
    if (const auto t = s.sub("peak_center"); t.table()) read_peak_center_step(t, dets, out.peak_center.emplace());
  if (step_table("set", SegmentKind::Set))
    if (const auto t = s.sub("set"); t.table()) read_set_step(t, out.set.emplace());
  if (step_table("blank_beam", SegmentKind::BlankBeam)) {
    bool on = false;
    s.get("blank_beam", on);
    out.blank_beam = on;
  }
  if (step_table("park", SegmentKind::Park))
    if (const auto t = s.sub("park"); t.table()) read_park_step(t, out.park.emplace());
  if (step_table("delay", SegmentKind::Delay)) {
    if (const auto t = s.sub("delay"); t.table()) {
      t.keys({"seconds"});
      if (!t.has("seconds")) p.add(t.at("seconds"), "required");
      auto& seconds = out.delay_s.emplace();
      t.get("seconds", seconds);
      t.at_least("seconds", seconds, 0);
    }
  }
  // `time_zero` is the table of a time_zero step and also a key of a
  // collection segment (an inlet-relative time zero).
  if (out.kind == SegmentKind::TimeZero) {
    if (!s.has("time_zero")) p.add(s.at("time_zero"), "required");
    else read_time_zero(s, out.time_zero.emplace(), /*step=*/true);
  } else if (s.has("time_zero")) {
    if (step) p.add(s.at("time_zero"), "only allowed on a collection segment or a time_zero step");
    else read_time_zero(s, out.time_zero.emplace());
  }
}

void read_segments(const Section& s, std::string_view key, const DetectorsSpec& dets, std::vector<Segment>& out) {
  for_each_table(s, key, [&](const Section& ss, std::size_t) { read_segment(ss, dets, out.emplace_back()); });
}

// A [[gas]]. Equilibration values it does not give are the top-level
// [equilibration] table's; its `fits` overrides the plan's [fits] key by key.
void read_gas(const Section& s, const MeasurementPlan& plan, Gas& out) {
  s.keys({"name", "inlet", "outlet", "equilibration", "source_dark_phase", "time_zero", "fits", "segment"});
  s.get("name", out.name);
  if (out.name.empty()) s.problems().add(s.at("name"), "required");
  out.equilibration = plan.equilibration;
  s.get("inlet", out.equilibration.inlet);
  s.get("outlet", out.equilibration.outlet);
  if (s.has("equilibration")) {
    const auto e = s.sub("equilibration");
    e.keys({"time_s", "inlet_delay_s", "close_inlet"});
    e.get("time_s", out.equilibration.time_s);
    e.get("inlet_delay_s", out.equilibration.inlet_delay_s);
    e.get("close_inlet", out.equilibration.close_inlet);
    e.at_least("time_s", out.equilibration.time_s, 0);
    e.at_least("inlet_delay_s", out.equilibration.inlet_delay_s, 0);
  }
  s.get("source_dark_phase", out.source_dark_phase);
  if (s.has("time_zero")) read_time_zero(s, out.time_zero.emplace());
  out.fits = plan.fits;
  if (s.has("fits")) read_fits(s.sub("fits"), *out.fits);
  read_segments(s, "segment", plan.detectors, out.segments);
  if (out.segments.empty() && (!s.has("segment") || s.node("segment")->is_array()))
    s.problems().add(s.at("segment"), "at least one segment required");
}

// v2 form: [[gas]], or top-level [[segment]] as one implicit gas. The v1 block
// tables that add segments are sugar for [main] only: in a plan of segments
// that segment is written out, so its place among the others is the file's.
void read_v2(const Section& top, const V1Tables& v1, MeasurementPlan& plan) {
  auto& p = top.problems();
  if (top.has("main") && top.has("segment")) p.add("", "[main] and [[segment]] cannot both appear");
  if (top.has("main") && top.has("gas")) p.add("", "[main] and [[gas]] cannot both appear");
  if (top.has("gas") && top.has("segment")) p.add("", "[[gas]] and top-level [[segment]] cannot both appear");
  const auto sugar = [&](bool on, std::string_view path, std::string_view kind) {
    if (on) p.add(path, "only with [main]; in a plan of segments write a " + std::string(kind) + " segment");
  };
  sugar(v1.sniff.enabled, "sniff.enabled", "sniff");
  sugar(v1.whiff.enabled, "whiff.enabled", "whiff");
  sugar(v1.peak_center.before, "peak_center.before", "peak_center");
  sugar(v1.peak_center.after, "peak_center.after", "peak_center");
  sugar(v1.baseline.before, "baseline.before", "baseline");
  sugar(v1.baseline.after, "baseline.after", "baseline");

  if (top.has("gas")) {
    std::set<std::string> names;
    for_each_table(top, "gas", [&](const Section& gs, std::size_t) {
      auto& gas = plan.gases.emplace_back();
      read_gas(gs, plan, gas);
      if (!gas.name.empty() && !names.insert(gas.name).second)
        p.add(gs.at("name"), "gas '" + gas.name + "' named twice");
    });
    if (plan.gases.empty() && top.node("gas")->is_array()) p.add("gas", "at least one gas required");
    return;
  }
  Gas gas;
  gas.implicit = true;
  gas.equilibration = plan.equilibration;
  gas.fits = plan.fits;
  read_segments(top, "segment", plan.detectors, gas.segments);
  if (gas.segments.empty() && top.node("segment")->is_array()) p.add("segment", "at least one segment required");
  plan.gases.push_back(std::move(gas));
}

void check_detectors(const MeasurementPlan& plan, const V1Tables& v1, const ISpectrometerCatalog& spec, Problems& p) {
  const auto check = [&](const std::string& det, const std::string& where) {
    if (!det.empty() && !spec.has_detector(det)) p.add(where, "unknown detector '" + det + "'");
  };
  check(plan.detectors.reference, "detectors.reference");
  for (std::size_t i = 0; i < plan.detectors.exclude.size(); ++i)
    check(plan.detectors.exclude[i], join("detectors.exclude", i));
  check(v1.peak_center.detector, "peak_center.detector");
  check(v1.baseline.detector, "baseline.detector");
  for (std::size_t h = 0; h < v1.main.hops.size(); ++h) {
    const auto& hop = v1.main.hops[h];
    const auto where = join("main.hops", h);
    for (const auto& [iso, det] : hop.positions) check(det, join(join(where, "positions"), iso));
    for (std::size_t i = 0; i < hop.protect.size(); ++i) check(hop.protect[i], join(join(where, "protect"), i));
    if (hop.position) check(hop.position->detector, join(where, "position.detector"));
  }
}

// ---- v2 rules (spec 3.7) -------------------------------------------------------

// `[detectors] exclude` on a plan of segments: the excluded detectors leave
// every collect, except that an entry that protects its detector stays, not
// collected (active = false), so the cup is still protected during the move.
// A position that collected something and now collects nothing is an error,
// as a v1 hop with every detector excluded is.
void apply_exclude(MeasurementPlan& plan, Problems& p) {
  const auto& ex = plan.detectors.exclude;
  if (ex.empty()) return;
  const auto excluded = [&](const CollectEntry& e) { return std::find(ex.begin(), ex.end(), e.detector) != ex.end(); };
  for (std::size_t g = 0; g < plan.gases.size(); ++g) {
    auto& gas = plan.gases[g];
    for (std::size_t s = 0; s < gas.segments.size(); ++s) {
      auto& positions = gas.segments[s].positions;
      for (std::size_t j = 0; j < positions.size(); ++j) {
        auto& collect = positions[j].collect;
        const bool had_active = std::any_of(collect.begin(), collect.end(), [](const auto& e) { return e.active; });
        for (auto& e : collect)
          if (e.protect && excluded(e)) e.active = false;
        std::erase_if(collect, [&](const CollectEntry& e) { return !e.protect && excluded(e); });
        const bool active = std::any_of(collect.begin(), collect.end(), [](const auto& e) { return e.active; });
        if (had_active && !active) {
          const auto seg = gas.implicit ? join("segment", s) : join(join(join("gas", g), "segment"), s);
          p.add(join(join(seg, "positions"), j), "position has no detector left after exclude");
        }
      }
    }
  }
}

// The order segments run in: gases in file order; in a gas, by `when`, then
// file order (spec 5.1).
std::vector<std::pair<std::size_t, std::size_t>> run_order(const MeasurementPlan& plan) {
  std::vector<std::pair<std::size_t, std::size_t>> out;
  for (std::size_t g = 0; g < plan.gases.size(); ++g) {
    const auto& segs = plan.gases[g].segments;
    std::vector<std::size_t> idx(segs.size());
    for (std::size_t s = 0; s < idx.size(); ++s) idx[s] = s;
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
      return static_cast<int>(segs[a].when) < static_cast<int>(segs[b].when);
    });
    for (const auto s : idx) out.emplace_back(g, s);
  }
  return out;
}

// Every rule of spec 3.7 a plan's gases must keep, checked after the v1
// desugar so a v1 file goes through the same rules. A v1 file's own reader
// already holds it to the reference, exclude and detector-name rules, and the
// desugar copies its labels into baseline, sniff and whiff segments and its
// peak center into up to two steps: for it only the label masses of its
// signal hops and its peak center are checked here, under the v1 paths.
class V2Rules {
 public:
  // `v1`: the plan's v1 tables when it is a v1 file, else null.
  V2Rules(const MeasurementPlan& plan, const PlanResolvers& r, const V1Tables* v1, Problems& p)
      : plan_(plan), cat_(r.spectrometer), has_extraction_(r.has_extraction), v1_(v1), p_(p) {}

  void run() {
    if (v1_ != nullptr) return v1_masses();
    for (std::size_t g = 0; g < plan_.gases.size(); ++g) {
      const auto& gas = plan_.gases[g];
      for (std::size_t s = 0; s < gas.segments.size(); ++s) segment(g, s);
      time_zero(g);
    }
    tables();
    during_extraction();
  }

 private:
  std::string seg_path(std::size_t g, std::size_t s) const {
    if (plan_.gases[g].implicit) return join("segment", s);
    return join(join(join("gas", g), "segment"), s);
  }

  bool known(const std::string& det, const std::string& where) {
    if (cat_ == nullptr || det.empty() || cat_->has_detector(det)) return true;
    p_.add(where, "unknown detector '" + det + "'");
    return false;
  }

  void mass(const std::string& label, const std::string& where) {
    if (cat_ != nullptr && !label.empty() && !cat_->mass_of(label))
      p_.add(where, "label '" + label + "' has no mass");
  }

  void deflection(const std::string& det, const std::string& where) {
    if (cat_ != nullptr && cat_->has_detector(det) && !cat_->supports_deflection(det))
      p_.add(where, "detector '" + det + "' has no deflection control");
  }

  // The reference detector is in `collect` itself or as one of its modes
  // ("H2(CDD)" is collector H2 in CDD mode, spec 3.6).
  bool collects_reference(const Position& pos) const {
    const auto& ref = plan_.detectors.reference;
    return std::any_of(pos.collect.begin(), pos.collect.end(), [&](const CollectEntry& e) {
      return e.detector == ref || (e.detector.starts_with(ref) && e.detector.size() > ref.size() &&
                                   e.detector[ref.size()] == '(');
    });
  }

  void segment(std::size_t g, std::size_t s) {
    const auto& seg = plan_.gases[g].segments[s];
    const auto path = seg_path(g, s);
    for (std::size_t j = 0; j < seg.positions.size(); ++j) position(seg.positions[j], join(join(path, "positions"), j));
    if (seg.peak_center) {
      const auto at = join(path, "peak_center");
      mass(seg.peak_center->label, join(at, "label"));
      for (std::size_t i = 0; i < seg.peak_center->detectors.size(); ++i)
        known(seg.peak_center->detectors[i], join(join(at, "detectors"), i));
      for (std::size_t i = 0; i < seg.peak_center->extra_detectors.size(); ++i)
        known(seg.peak_center->extra_detectors[i], join(join(at, "extra_detectors"), i));
    }
    if (seg.set) {
      const auto at = join(path, "set");
      for (const auto& kv : seg.set->deflection) {
        const auto where = join(join(at, "deflection"), kv.first);
        if (known(kv.first, where)) deflection(kv.first, where);
      }
      for (const auto& kv : seg.set->gain) known(kv.first, join(join(at, "gain"), kv.first));
    }
    if (seg.park) {
      const auto at = join(path, "park");
      known(seg.park->detector, join(at, "detector"));
      mass(seg.park->label, join(at, "label"));
    }
  }

  void position(const Position& pos, const std::string& path) {
    std::set<std::string> seen;
    const bool at_mass = pos.at && std::holds_alternative<AtMass>(*pos.at);
    for (std::size_t k = 0; k < pos.collect.size(); ++k) {
      const auto& e = pos.collect[k];
      const auto where = join(join(path, "collect"), k);
      if (!e.detector.empty() && !seen.insert(e.detector).second)
        p_.add(join(path, "collect"), "detector '" + e.detector + "' appears twice in position");
      if (e.label.empty()) p_.add(where, "collect entry needs a label");
      else if (!at_mass) mass(e.label, where);
      if (known(e.detector, where) && e.deflection) deflection(e.detector, join(where, "deflection"));
    }
    if (!pos.optics.empty() && cat_ != nullptr && !cat_->has_optics(pos.optics))
      p_.add(join(path, "optics"), "unknown optics '" + pos.optics + "'");
    at(pos, path);
  }

  void at(const Position& pos, const std::string& path) {
    const auto where = join(path, "at");
    if (!pos.at) {
      const auto& ref = plan_.detectors.reference;
      // With no reference in the plan, the spectrometer's positions it.
      if (!ref.empty() && !collects_reference(pos))
        p_.add(path, "reference detector '" + ref + "' not in collect: needs 'at'");
      return;
    }
    std::visit(
        [&](const auto& a) {
          using T = std::decay_t<decltype(a)>;
          if constexpr (std::is_same_v<T, AtLabel>) {
            known(a.detector, join(where, "detector"));
            mass(a.label, join(where, "label"));
          } else if constexpr (std::is_same_v<T, AtMass>) {
            known(a.detector, join(where, "detector"));
          } else if constexpr (std::is_same_v<T, AtDac>) {
            if (cat_ != nullptr && !cat_->positions_by_dac())
              p_.add(join(where, "dac"), "the spectrometer cannot position by DAC");
          } else {
            known(a.detector, join(where, "detector"));
            if (cat_ != nullptr && !cat_->positions_by_hv())
              p_.add(join(where, "hv"), "the spectrometer cannot position by HV");
          }
        },
        *pos.at);
  }

  // At most one time zero in a gas (its own, or one collection segment's),
  // and a segment's only on the gas's first after_inlet signal segment. A
  // time_zero step may come anywhere.
  void time_zero(std::size_t g) {
    const auto& gas = plan_.gases[g];
    const auto first = std::find_if(gas.segments.begin(), gas.segments.end(), [](const Segment& s) {
      return s.kind == SegmentKind::Signal && s.when == When::AfterInlet;
    });
    int count = gas.time_zero ? 1 : 0;
    for (std::size_t s = 0; s < gas.segments.size(); ++s) {
      const auto& seg = gas.segments[s];
      if (is_step(seg.kind) || !seg.time_zero) continue;
      ++count;
      if (first == gas.segments.end() || &seg != &*first)
        p_.add(join(seg_path(g, s), "time_zero"), "time_zero belongs on the first after_inlet signal segment");
    }
    if (count > 1) {
      const auto where = gas.implicit ? std::string() : join(join("gas", g), "time_zero");
      p_.add(where, gas.implicit ? "time_zero set twice in gas" : "time_zero set twice in gas '" + gas.name + "'");
    }
  }

  // A table is the spectrometer's, or written by a peak_center step that runs
  // before the position that uses it.
  void tables() {
    std::set<std::string> later;
    for (const auto& gas : plan_.gases)
      for (const auto& seg : gas.segments)
        if (seg.peak_center && !seg.peak_center->write_table.empty()) later.insert(seg.peak_center->write_table);
    std::set<std::string> written;
    const auto use = [&](const std::string& name, const std::string& where) {
      if (name.empty() || written.contains(name)) return;
      if (cat_ != nullptr && cat_->has_table(name)) return;
      if (later.contains(name)) p_.add(where, "table '" + name + "' is used before it is written");
      else if (cat_ != nullptr) p_.add(where, "unknown table '" + name + "'");
    };
    for (const auto& [g, s] : run_order(plan_)) {
      const auto& seg = plan_.gases[g].segments[s];
      const auto path = seg_path(g, s);
      use(seg.table, join(path, "table"));
      for (std::size_t j = 0; j < seg.positions.size(); ++j)
        use(seg.positions[j].table, join(join(join(path, "positions"), j), "table"));
      if (seg.peak_center && !seg.peak_center->write_table.empty()) written.insert(seg.peak_center->write_table);
    }
  }

  // No leak watch while heating (owner, 2026-10-15), and an analysis type
  // without an extraction has no time for such a segment.
  void during_extraction() {
    bool reported_types = false;
    for (std::size_t g = 0; g < plan_.gases.size(); ++g) {
      const auto& segs = plan_.gases[g].segments;
      for (std::size_t s = 0; s < segs.size(); ++s) {
        if (segs[s].when != When::DuringExtraction) continue;
        const auto where = join(seg_path(g, s), "when");
        if (segs[s].kind == SegmentKind::Signal || segs[s].kind == SegmentKind::Sniff)
          p_.add(where, "no signal or sniff segment during_extraction");
        if (reported_types || !has_extraction_) continue;
        for (const auto& type : plan_.info.analysis_types) {
          if (has_extraction_(type)) continue;
          p_.add(where, "during_extraction needs an analysis type with an extraction; '" + type + "' has none");
          reported_types = true;
        }
      }
    }
  }

  void v1_masses() {
    for (std::size_t h = 0; h < v1_->main.hops.size(); ++h) {
      const auto& hop = v1_->main.hops[h];
      if (hop.baseline && hop.mass) continue;
      for (const auto& kv : hop.positions) mass(kv.first, join(join(join("main.hops", h), "positions"), kv.first));
    }
    const auto& pc = v1_->peak_center;
    if (pc.before || pc.after) mass(pc.isotope, "peak_center.isotope");
  }

  const MeasurementPlan& plan_;
  const ISpectrometerCatalog* cat_;
  std::function<bool(std::string_view)> has_extraction_;
  const V1Tables* v1_;
  Problems& p_;
};

}  // namespace

namespace detail {

namespace {

// Where a v1 file puts each kind of segment (desugar, spec 4.1): the only
// placements the engine runs until the phases are built.
bool v1_placement(const Segment& s) {
  switch (s.kind) {
    case SegmentKind::Signal:
    case SegmentKind::Whiff: return s.when == When::AfterInlet;
    case SegmentKind::Baseline: return s.when == When::BeforeInlet || s.when == When::AfterMain;
    case SegmentKind::Sniff: return s.when == When::DuringEquilibration;
    case SegmentKind::PeakCenter: return s.when == When::BeforeInlet || s.when == When::AfterRun;
    default: return true;  // the other steps are refused by kind
  }
}

// The position executor runs deflection, protect, inactive entries, every
// `at` form, a position's integration and table (spec 5.2); optics wait for a
// driver that applies them (phase 8). A sniff or whiff reads where the
// magnet is, so its positions get none of the per-position settings yet.
void unbuilt_position(const Position& pos, const std::string& at, bool in_place, std::vector<std::string>& out) {
  const auto add = [&](const std::string& path, std::string_view feature) {
    out.push_back(path + ": " + std::string(feature));
  };
  if (!pos.optics.empty()) add(at, "optics");
  if (!in_place) return;
  for (std::size_t k = 0; k < pos.collect.size(); ++k) {
    const auto& e = pos.collect[k];
    const auto where = join(join(at, "collect"), k);
    if (e.deflection) add(where, "deflection");
    if (!e.active && !e.protect) add(where, "active = false without protect");
  }
  if (!pos.table.empty()) add(at, "table");
  if (pos.integration_s) add(at, "integration_s");
}

void unbuilt_segment(const Segment& s, const std::string& at, std::vector<std::string>& out) {
  const auto add = [&](const std::string& path, std::string_view feature) {
    out.push_back(path + ": " + std::string(feature));
  };
  switch (s.kind) {
    case SegmentKind::Set:
    case SegmentKind::BlankBeam:
    case SegmentKind::Park:
    case SegmentKind::Delay:
    case SegmentKind::TimeZero: add(at, std::string(to_string(s.kind)) + " step"); break;
    case SegmentKind::PeakCenter:
      if (s.peak_center) {
        if (s.peak_center->detectors.size() > 1) add(join(at, "peak_center"), "peak_center with several detectors");
        if (!s.peak_center->extra_detectors.empty()) add(join(at, "peak_center"), "extra_detectors");
        if (!s.peak_center->write_table.empty()) add(join(at, "peak_center"), "write_table");
      }
      break;
    default: break;
  }
  // The engine takes one sniff or whiff reading block, at one place.
  if (s.kind == SegmentKind::Sniff || s.kind == SegmentKind::Whiff) {
    const std::string kind(to_string(s.kind));
    if (s.positions.size() > 1) add(join(at, "positions"), kind + " segment with several positions");
    for (std::size_t j = 0; j < s.positions.size(); ++j)
      if (s.positions[j].at) add(join(join(join(at, "positions"), j), "at"), "at on a " + kind + " segment");
    if (s.cycles != 1) add(join(at, "cycles"), kind + " segment with cycles");
  }
  if (!v1_placement(s))
    add(join(at, "when"), "when = '" + std::string(to_string(s.when)) + "' for a " + std::string(to_string(s.kind)) +
                              " segment");
  const bool in_place = s.kind == SegmentKind::Sniff || s.kind == SegmentKind::Whiff;
  if (in_place && !s.table.empty()) add(at, "table");
  if (!s.truncations.empty()) add(join(at, "conditionals"), "segment conditionals");
  for (std::size_t j = 0; j < s.positions.size(); ++j)
    unbuilt_position(s.positions[j], join(join(at, "positions"), j), in_place, out);
}

}  // namespace

std::vector<std::string> unbuilt_features(const MeasurementPlan& plan) {
  std::vector<std::string> out;
  for (std::size_t g = 0; g < plan.gases.size(); ++g) {
    const auto& gas = plan.gases[g];
    // An implicit gas is the file's top-level segments (or its v1 tables).
    const std::string at = gas.implicit ? std::string() : join("gas", g);
    if (g == 1) out.push_back(at + ": more than one gas");
    if (gas.source_dark_phase) out.push_back(join(at, "source_dark_phase") + ": source_dark_phase");
    const auto seg_at = [&](std::size_t i) { return at.empty() ? join("segment", i) : join(join(at, "segment"), i); };
    for (std::size_t i = 0; i < gas.segments.size(); ++i) unbuilt_segment(gas.segments[i], seg_at(i), out);
    // What the engine runs of a gas until its phases are built (spec 5.1):
    // one sniff, one whiff ahead of main, and a main segment to run.
    const auto* main = main_segment(gas);
    bool sniff = false, whiff = false;
    for (std::size_t i = 0; i < gas.segments.size(); ++i) {
      const auto& seg = gas.segments[i];
      if (seg.kind == SegmentKind::Sniff) {
        if (sniff) out.push_back(seg_at(i) + ": a second sniff segment");
        sniff = true;
      } else if (seg.kind == SegmentKind::Whiff) {
        if (whiff) out.push_back(seg_at(i) + ": a second whiff segment");
        if (main != nullptr && &seg > main) out.push_back(seg_at(i) + ": whiff after the signal segment");
        whiff = true;
      }
    }
    if (main == nullptr) out.push_back((at.empty() ? std::string("segment") : join(at, "segment")) +
                                       ": no after_inlet signal segment");
  }
  return out;
}

Result<MeasurementPlan> resolve_plan_ungated(std::string_view effective_toml, const PlanResolvers& resolvers,
                                             std::string_view source) {
  auto parsed = toml::parse(effective_toml, source);
  if (!parsed) {
    const auto& e = parsed.error();
    return fail(ErrorKind::Config, std::string(source) + ": parse error: " + std::string(e.description()) + " (line " +
                                       std::to_string(e.source().begin.line) + ")");
  }
  Problems p(source);
  const toml::table& original = parsed.table();
  toml::table root = original;
  AliasPass(original, resolvers, p).run(root, "");

  const Section top(&root, "", p);
  top.keys({"plan", "hook", "detectors", "equilibration", "sniff", "peak_center", "baseline", "main", "fits",
            "conditionals", "whiff", "parameters", "segment", "gas"});
  const bool v2 = top.has("segment") || top.has("gas");
  MeasurementPlan plan;
  read_info(top.sub("plan"), plan.info);
  top.get("hook", plan.hook);
  read_detectors(top.sub("detectors"), plan.detectors);
  read_equilibration(top.sub("equilibration"), plan.equilibration);
  V1Tables v1;
  read_sniff(top.sub("sniff"), v1.sniff);
  read_peak_center(top.sub("peak_center"), v1.peak_center);
  read_baseline(top.sub("baseline"), v1.baseline);
  if (!v2 || top.has("main")) read_main(top.sub("main"), plan.detectors, v1.main);
  read_fits(top.sub("fits"), plan.fits);
  read_conditionals(top.sub("conditionals"), plan.conditionals);
  read_whiff(top.sub("whiff"), v1.whiff);
  plan.expose = detail::read_expose(root, p);
  if (v2) read_v2(top, v1, plan);
  // v1 -> v2 (spec 4.1) once the v1 sections read cleanly, before the rules.
  if (!v2 && !p.any()) {
    if (auto gases = detail::desugar_v1(plan, v1, resolvers.spectrometer)) plan.gases = std::move(*gases);
    else p.add("", gases.error().what);
  }
  if (v2) apply_exclude(plan, p);
  if (resolvers.spectrometer) check_detectors(plan, v1, *resolvers.spectrometer, p);
  V2Rules(plan, resolvers, v2 ? nullptr : &v1, p).run();

  if (p.any()) return p.error();
  return plan;
}

}  // namespace detail

// Until each phase of the v2 plan lands, a plan that uses what it builds is
// refused here, so a plan that resolves always runs as written.
Result<MeasurementPlan> resolve_plan(std::string_view effective_toml, const PlanResolvers& resolvers,
                                     std::string_view source) {
  auto plan = detail::resolve_plan_ungated(effective_toml, resolvers, source);
  if (!plan) return plan;
  const auto unbuilt = detail::unbuilt_features(*plan);
  if (unbuilt.empty()) return plan;
  Problems p(source);
  for (const auto& feature : unbuilt) p.add("", feature + ": not run by this build yet");
  return p.error();
}

}  // namespace pychron::experiment::plan
