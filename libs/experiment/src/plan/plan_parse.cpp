// Effective plan TOML -> resolved, validated MeasurementPlan.

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>

#include "pychron/experiment/conditionals/conditional.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"
#include "plan_toml.hpp"

namespace pychron::experiment::plan {
namespace {

using detail::join;
using detail::Problems;

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

void read_sniff(const Section& s, Sniff& out) {
  s.keys({"enabled", "counts", "integration_s"});
  s.get("enabled", out.enabled);
  s.get("counts", out.counts);
  s.get("integration_s", out.integration_s);
  if (out.enabled) s.at_least("counts", out.counts, 1, "when enabled");
  s.above("integration_s", out.integration_s, 0);
}

void read_peak_center(const Section& s, PeakCenter& out) {
  s.keys({"before", "after", "detector", "isotope", "config"});
  s.get("before", out.before);
  s.get("after", out.after);
  s.get("detector", out.detector);
  s.get("isotope", out.isotope);
  s.get("config", out.config);
  if ((out.before || out.after) && out.isotope.empty())
    s.problems().add(s.at("isotope"), "required when before or after");
}

void read_baseline(const Section& s, Baseline& out) {
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

void read_time_zero(const Section& s, TimeZero& out) {
  const auto* n = s.node("time_zero");
  if (!n) return;
  const std::string expected = "expected 'on_inlet_close', 'on_first_count' or { offset_s = N }";
  if (auto str = n->value_exact<std::string>()) {
    if (*str == "on_inlet_close") out.kind = TimeZeroKind::OnInletClose;
    else if (*str == "on_first_count") out.kind = TimeZeroKind::OnFirstCount;
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

void read_hop(const Section& s, const DetectorsSpec& dets, Hop& hop) {
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
    HopPosition hp;
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

void read_main(const Section& s, const DetectorsSpec& dets, MainSpec& out) {
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

void read_conditionals(const Section& s, PlanConditionals& out) {
  s.keys({"include", "truncations"});
  s.get("include", out.include);
  const auto* n = s.node("truncations");
  if (!n) return;
  const auto* arr = n->as_array();
  if (!arr) return s.problems().add(s.at("truncations"), "expected an array of tables");
  for (std::size_t i = 0; i < arr->size(); ++i) {
    const auto where = join(s.at("truncations"), i);
    const auto* t = arr->get(i)->as_table();
    if (!t) {
      s.problems().add(where, "expected a table");
      continue;
    }
    Section ts(t, where, s.problems());
    ts.keys({"check", "start"});
    auto& tr = out.truncations.emplace_back();
    ts.get("check", tr.check);
    ts.get("start", tr.start);
    if (tr.check.empty()) s.problems().add(ts.at("check"), "required");
    ts.at_least("start", tr.start, 0);
  }
}

void read_whiff(const Section& s, Whiff& out) {
  s.keys({"enabled", "counts", "integration_s", "checks"});
  s.get("enabled", out.enabled);
  s.get("counts", out.counts);
  s.get("integration_s", out.integration_s);
  s.above("integration_s", out.integration_s, 0);
  if (const auto* n = s.node("checks")) {
    const auto* arr = n->as_array();
    if (!arr) return s.problems().add(s.at("checks"), "expected an array of tables");
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const auto where = join(s.at("checks"), i);
      const auto* t = arr->get(i)->as_table();
      if (!t) {
        s.problems().add(where, "expected a table");
        continue;
      }
      Section cs(t, where, s.problems());
      cs.keys({"check", "action"});
      auto& c = out.checks.emplace_back();
      cs.get("check", c.check);
      cs.get("action", c.action);
      if (c.check.empty()) {
        s.problems().add(cs.at("check"), "required");
      } else if (auto e = parse_expression(c.check); !e) {
        s.problems().add(cs.at("check"), e.error().what);
      }
      if (!parse_whiff_action(c.action)) s.problems().add(cs.at("action"), "expected run_remainder, pump or abort");
    }
  }
  if (out.enabled) {
    if (out.counts < 1) s.problems().add(s.at("counts"), "an enabled whiff needs counts >= 1");
    if (out.checks.empty()) s.problems().add(s.at("checks"), "an enabled whiff needs at least one check");
  }
}

void check_detectors(const MeasurementPlan& plan, const ISpectrometerCatalog& spec, Problems& p) {
  const auto check = [&](const std::string& det, const std::string& where) {
    if (!det.empty() && !spec.has_detector(det)) p.add(where, "unknown detector '" + det + "'");
  };
  check(plan.detectors.reference, "detectors.reference");
  for (std::size_t i = 0; i < plan.detectors.exclude.size(); ++i)
    check(plan.detectors.exclude[i], join("detectors.exclude", i));
  check(plan.peak_center.detector, "peak_center.detector");
  check(plan.baseline.detector, "baseline.detector");
  for (std::size_t h = 0; h < plan.main.hops.size(); ++h) {
    const auto& hop = plan.main.hops[h];
    const auto where = join("main.hops", h);
    for (const auto& [iso, det] : hop.positions) check(det, join(join(where, "positions"), iso));
    for (std::size_t i = 0; i < hop.protect.size(); ++i) check(hop.protect[i], join(join(where, "protect"), i));
    if (hop.position) check(hop.position->detector, join(where, "position.detector"));
  }
}

}  // namespace

Result<MeasurementPlan> resolve_plan(std::string_view effective_toml, const PlanResolvers& resolvers,
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
            "conditionals", "whiff", "parameters"});
  MeasurementPlan plan;
  read_info(top.sub("plan"), plan.info);
  top.get("hook", plan.hook);
  read_detectors(top.sub("detectors"), plan.detectors);
  read_equilibration(top.sub("equilibration"), plan.equilibration);
  read_sniff(top.sub("sniff"), plan.sniff);
  read_peak_center(top.sub("peak_center"), plan.peak_center);
  read_baseline(top.sub("baseline"), plan.baseline);
  read_main(top.sub("main"), plan.detectors, plan.main);
  read_fits(top.sub("fits"), plan.fits);
  read_conditionals(top.sub("conditionals"), plan.conditionals);
  read_whiff(top.sub("whiff"), plan.whiff);
  plan.expose = detail::read_expose(root, p);
  if (resolvers.spectrometer) check_detectors(plan, *resolvers.spectrometer, p);

  if (p.any()) return p.error();
  return plan;
}

}  // namespace pychron::experiment::plan
