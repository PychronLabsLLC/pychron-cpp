#include "pychron/experiment/record/serialize.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <sstream>
#include <variant>

#include <toml++/toml.hpp>

#include "pychron/experiment/record/sha256.hpp"

namespace pychron::experiment::record {

double icfactor(const Results& r, const std::string& det) {
  const auto it = r.icfactors.find(det);
  return it == r.icfactors.end() ? 1.0 : it->second;
}

namespace {

// ---- writing --------------------------------------------------------------

template <class T>
toml::array num_array(const std::vector<T>& v) {
  toml::array a;
  for (const auto& x : v) {
    if constexpr (std::is_same_v<T, float>)
      a.push_back(static_cast<double>(x));
    else
      a.push_back(x);
  }
  return a;
}

toml::array str_array(const std::vector<std::string>& v) {
  toml::array a;
  for (const auto& x : v) a.push_back(x);
  return a;
}

toml::table dmap(const std::map<std::string, double>& m) {
  toml::table t;
  for (const auto& [k, v] : m) t.insert_or_assign(k, v);
  return t;
}

toml::table trace_table(const Trace& tr) {
  toml::table t;
  t.insert_or_assign("t", num_array(tr.t));
  t.insert_or_assign("v", num_array(tr.v));
  if (!tr.sigma.empty()) t.insert_or_assign("sigma", num_array(tr.sigma));
  return t;
}

std::string_view error_name(reduction::ErrorType e) { return e == reduction::ErrorType::Sd ? "sd" : "sem"; }

toml::table fit_table(const reduction::FitSpec& f) {
  toml::table t;
  t.insert_or_assign("kind", std::string(reduction::to_string(f.kind)));
  t.insert_or_assign("error", std::string(error_name(f.error)));
  t.insert_or_assign("degree", f.degree);
  t.insert_or_assign("outliers_enabled", f.outliers.enabled);
  t.insert_or_assign("outliers_iterations", f.outliers.iterations);
  t.insert_or_assign("outliers_std_devs", f.outliers.std_devs);
  return t;
}

toml::table to_table(const AnalysisRecord& r) {
  toml::table root;

  toml::table id;
  id.insert_or_assign("uuid", r.identity.uuid);
  id.insert_or_assign("identifier", r.identity.identifier);
  id.insert_or_assign("aliquot", r.identity.aliquot);
  id.insert_or_assign("step", r.identity.step);
  id.insert_or_assign("analysis_type", r.identity.analysis_type);
  id.insert_or_assign("timestamp", r.identity.timestamp);
  id.insert_or_assign("run_index", r.identity.run_index);
  id.insert_or_assign("queue_uuid", r.identity.queue_uuid);
  root.insert_or_assign("identity", std::move(id));

  toml::table sm;
  sm.insert_or_assign("sample", r.sample.sample);
  sm.insert_or_assign("project", r.sample.project);
  sm.insert_or_assign("material", r.sample.material);
  sm.insert_or_assign("irradiation", r.sample.irradiation);
  sm.insert_or_assign("level", r.sample.level);
  sm.insert_or_assign("position", r.sample.position);
  sm.insert_or_assign("pi", r.sample.pi);
  sm.insert_or_assign("note", r.sample.note);
  root.insert_or_assign("sample", std::move(sm));

  toml::table in;
  in.insert_or_assign("mass_spectrometer", r.instrument.mass_spectrometer);
  in.insert_or_assign("extract_device", r.instrument.extract_device);
  in.insert_or_assign("laboratory", r.instrument.laboratory);
  in.insert_or_assign("analyst", r.instrument.analyst);
  in.insert_or_assign("software_version", r.instrument.software_version);
  in.insert_or_assign("software_git_sha", r.instrument.software_git_sha);
  root.insert_or_assign("instrument", std::move(in));

  toml::table ex, spec, act;
  spec.insert_or_assign("value", r.extraction.spec.value);
  spec.insert_or_assign("duration", r.extraction.spec.duration);
  spec.insert_or_assign("cleanup", r.extraction.spec.cleanup);
  spec.insert_or_assign("units", r.extraction.spec.units);
  spec.insert_or_assign("pattern", r.extraction.spec.pattern);
  spec.insert_or_assign("positions", num_array(r.extraction.spec.positions));
  ex.insert_or_assign("spec", std::move(spec));
  const auto& a = r.extraction.actuals;
  act.insert_or_assign("value", a.value);
  act.insert_or_assign("duration", a.duration);
  act.insert_or_assign("cleanup", a.cleanup);
  act.insert_or_assign("beam_diameter", a.beam_diameter);
  act.insert_or_assign("positions", num_array(a.positions));
  act.insert_or_assign("pattern", a.pattern);
  act.insert_or_assign("pid_params", dmap(a.pid_params));
  toml::table series;
  for (const auto& [k, tr] : a.series) series.insert_or_assign(k, trace_table(tr));
  act.insert_or_assign("series", std::move(series));
  act.insert_or_assign("snapshot_refs", str_array(a.snapshot_refs));
  toml::array polys;
  for (const auto& poly : a.grain_polygons) {
    toml::array pts;
    for (const auto& [x, y] : poly) pts.push_back(toml::array{x, y});
    polys.push_back(std::move(pts));
  }
  act.insert_or_assign("grain_polygons", std::move(polys));
  act.insert_or_assign("pipette_counts", a.pipette_counts);
  if (a.manometer_pressure) act.insert_or_assign("manometer_pressure", *a.manometer_pressure);
  ex.insert_or_assign("actuals", std::move(act));
  root.insert_or_assign("extraction", std::move(ex));

  toml::table ms, plan;
  plan.insert_or_assign("template", r.measurement.plan.template_name);
  plan.insert_or_assign("version", r.measurement.plan.version);
  plan.insert_or_assign("effective_plan_toml", r.measurement.plan.effective_plan_toml);
  toml::table ov;
  for (const auto& [k, v] : r.measurement.plan.overrides) ov.insert_or_assign(k, v);
  plan.insert_or_assign("overrides", std::move(ov));
  ms.insert_or_assign("plan", std::move(plan));
  if (r.measurement.hook) {
    toml::table h;
    h.insert_or_assign("name", r.measurement.hook->name);
    h.insert_or_assign("sha", r.measurement.hook->sha);
    ms.insert_or_assign("hook", std::move(h));
  }
  toml::table scripts;
  for (const auto& [k, s] : r.measurement.scripts) {
    toml::table st;
    st.insert_or_assign("name", s.name);
    st.insert_or_assign("sha", s.sha);
    st.insert_or_assign("text_ref", s.text_ref);
    scripts.insert_or_assign(k, std::move(st));
  }
  ms.insert_or_assign("scripts", std::move(scripts));
  root.insert_or_assign("measurement", std::move(ms));

  toml::table sp;
  sp.insert_or_assign("state_hash", r.spectrometer.state_hash);
  sp.insert_or_assign("field_table_version", r.spectrometer.field_table_version);
  sp.insert_or_assign("integration_time", r.spectrometer.integration_time);
  sp.insert_or_assign("deflections", dmap(r.spectrometer.deflections));
  sp.insert_or_assign("gains", dmap(r.spectrometer.gains));
  sp.insert_or_assign("source_params", dmap(r.spectrometer.source_params));
  root.insert_or_assign("spectrometer", std::move(sp));

  toml::table data;
  toml::array ds;
  for (const auto& s : r.data.series) {
    auto t = trace_table(s.trace);
    t.insert_or_assign("iso", s.iso);
    t.insert_or_assign("det", s.det);
    t.insert_or_assign("kind", s.kind);
    ds.push_back(std::move(t));
  }
  data.insert_or_assign("series", std::move(ds));
  data.insert_or_assign("time_zero", r.data.time_zero);
  toml::table counts;
  for (const auto& [k, v] : r.data.counts) counts.insert_or_assign(k, v);
  data.insert_or_assign("counts", std::move(counts));
  root.insert_or_assign("data", std::move(data));

  toml::table res, ints, bases;
  for (const auto& [k, v] : r.results.intercepts) {
    toml::table t;
    t.insert_or_assign("value", v.intercept.value);
    t.insert_or_assign("error", v.intercept.error);
    t.insert_or_assign("n_used", static_cast<std::int64_t>(v.intercept.n_used));
    std::vector<std::int64_t> idx(v.intercept.filtered_idx.begin(), v.intercept.filtered_idx.end());
    t.insert_or_assign("filtered_idx", num_array(idx));
    t.insert_or_assign("residual_sd", v.intercept.residual_sd);
    t.insert_or_assign("fit", fit_table(v.fit));
    ints.insert_or_assign(k, std::move(t));
  }
  for (const auto& [k, v] : r.results.baselines) {
    toml::table t;
    t.insert_or_assign("value", v.value);
    t.insert_or_assign("error", v.error);
    t.insert_or_assign("fit", fit_table(v.fit));
    bases.insert_or_assign(k, std::move(t));
  }
  res.insert_or_assign("intercepts", std::move(ints));
  res.insert_or_assign("baselines", std::move(bases));
  res.insert_or_assign("blanks_ref", r.results.blanks_ref);
  res.insert_or_assign("icfactors", dmap(r.results.icfactors));
  res.insert_or_assign("whiff", r.results.whiff);
  root.insert_or_assign("results", std::move(res));

  toml::table cond;
  toml::array installed, tripped, errors;
  for (const auto& c : r.conditionals.installed) {
    toml::table t;
    t.insert_or_assign("id", c.id);
    t.insert_or_assign("name", c.name);
    t.insert_or_assign("kind", c.kind);
    t.insert_or_assign("level", c.level);
    t.insert_or_assign("location", c.location);
    t.insert_or_assign("check", c.check);
    t.insert_or_assign("start", c.start);
    t.insert_or_assign("frequency", c.frequency);
    t.insert_or_assign("ntrips", c.ntrips);
    t.insert_or_assign("window", c.window);
    t.insert_or_assign("mapper", c.mapper);
    t.insert_or_assign("analysis_types", str_array(c.analysis_types));
    t.insert_or_assign("abbreviated_count_ratio", c.abbreviated_count_ratio);
    t.insert_or_assign("action", c.action);
    t.insert_or_assign("resume", c.resume);
    t.insert_or_assign("truncate", c.truncate);
    t.insert_or_assign("terminate", c.terminate);
    installed.push_back(std::move(t));
  }
  for (const auto& c : r.conditionals.tripped) {
    toml::table t;
    t.insert_or_assign("id", c.id);
    t.insert_or_assign("name", c.name);
    t.insert_or_assign("kind", c.kind);
    t.insert_or_assign("check", c.check);
    t.insert_or_assign("action", c.action);
    t.insert_or_assign("reading", c.reading);
    t.insert_or_assign("count", c.count);
    t.insert_or_assign("t", c.t);
    t.insert_or_assign("value", c.value);
    t.insert_or_assign("context", dmap(c.context));
    tripped.push_back(std::move(t));
  }
  for (const auto& e : r.conditionals.errors) {
    toml::table t;
    t.insert_or_assign("name", e.name);
    t.insert_or_assign("message", e.message);
    t.insert_or_assign("count", e.count);
    errors.push_back(std::move(t));
  }
  cond.insert_or_assign("installed", std::move(installed));
  cond.insert_or_assign("tripped", std::move(tripped));
  cond.insert_or_assign("errors", std::move(errors));
  root.insert_or_assign("conditionals", std::move(cond));

  toml::array ev;
  for (const auto& e : r.events) {
    toml::table t;
    t.insert_or_assign("t", e.t);
    t.insert_or_assign("kind", e.kind);
    t.insert_or_assign("detail", e.detail);
    ev.push_back(std::move(t));
  }
  root.insert_or_assign("events", std::move(ev));

  toml::table pv;
  pv.insert_or_assign("schema_version", r.provenance.schema_version);
  pv.insert_or_assign("sha", r.provenance.sha);
  pv.insert_or_assign("persister_refs", str_array(r.provenance.persister_refs));
  root.insert_or_assign("provenance", std::move(pv));
  return root;
}

// ---- reading --------------------------------------------------------------

struct Rd {
  const toml::table* t;
  std::string where;
  std::string* err;

  void fail(const std::string& key, const char* want) const {
    if (err->empty()) *err = where + "." + key + ": expected " + want;
  }
  const toml::node* find(const std::string& key) const { return t ? t->get(key) : nullptr; }

  Rd sub(const std::string& key) const {
    const auto* n = find(key);
    if (!n) return {nullptr, where + "." + key, err};
    if (!n->is_table()) {
      fail(key, "table");
      return {nullptr, where + "." + key, err};
    }
    return {n->as_table(), where + "." + key, err};
  }
  void str(const std::string& key, std::string& out) const {
    const auto* n = find(key);
    if (!n) return;
    if (auto v = n->value<std::string>()) out = *v;
    else fail(key, "string");
  }
  void num(const std::string& key, double& out) const {
    const auto* n = find(key);
    if (!n) return;
    if (auto v = n->value<double>()) out = *v;
    else fail(key, "number");
  }
  void opt_num(const std::string& key, std::optional<double>& out) const {
    if (!find(key)) return;
    double v = 0;
    num(key, v);
    out = v;
  }
  template <class I>
  void integer(const std::string& key, I& out) const {
    const auto* n = find(key);
    if (!n) return;
    if (auto v = n->value<std::int64_t>()) out = static_cast<I>(*v);
    else fail(key, "integer");
  }
  void boolean(const std::string& key, bool& out) const {
    const auto* n = find(key);
    if (!n) return;
    if (auto v = n->value<bool>()) out = *v;
    else fail(key, "boolean");
  }
  template <class T>
  void nums(const std::string& key, std::vector<T>& out) const {
    const auto* n = find(key);
    if (!n) return;
    const auto* a = n->as_array();
    if (!a) return fail(key, "array");
    out.clear();
    for (const auto& e : *a) {
      if constexpr (std::is_integral_v<T>) {
        auto v = e.template value<std::int64_t>();
        if (!v) return fail(key, "integer array");
        out.push_back(static_cast<T>(*v));
      } else {
        auto v = e.template value<double>();
        if (!v) return fail(key, "number array");
        out.push_back(static_cast<T>(*v));
      }
    }
  }
  void strs(const std::string& key, std::vector<std::string>& out) const {
    const auto* n = find(key);
    if (!n) return;
    const auto* a = n->as_array();
    if (!a) return fail(key, "array");
    out.clear();
    for (const auto& e : *a) {
      auto v = e.value<std::string>();
      if (!v) return fail(key, "string array");
      out.push_back(*v);
    }
  }
  // Each entry of table `key` -> f(name, Rd-of-entry-table).
  template <class F>
  void each_table(const std::string& key, F&& f) const {
    const auto s = sub(key);
    if (!s.t) return;
    for (const auto& [k, v] : *s.t) {
      if (!v.is_table()) return s.fail(std::string(k.str()), "table");
      f(std::string(k.str()), Rd{v.as_table(), s.where + "." + std::string(k.str()), err});
    }
  }
  // Each table of array `key` -> f(Rd-of-entry).
  template <class F>
  void each_entry(const std::string& key, F&& f) const {
    const auto* n = find(key);
    if (!n) return;
    const auto* a = n->as_array();
    if (!a) return fail(key, "array of tables");
    int i = 0;
    for (const auto& e : *a) {
      const auto* et = e.as_table();
      if (!et) return fail(key, "array of tables");
      f(Rd{et, where + "." + key + "[" + std::to_string(i++) + "]", err});
    }
  }
  void dmap(const std::string& key, std::map<std::string, double>& out) const {
    const auto s = sub(key);
    if (!s.t) return;
    out.clear();
    for (const auto& [k, v] : *s.t) {
      auto d = v.value<double>();
      if (!d) return s.fail(std::string(k.str()), "number");
      out[std::string(k.str())] = *d;
    }
  }
};

Trace read_trace(const Rd& r) {
  Trace t;
  r.nums("t", t.t);
  r.nums("v", t.v);
  r.nums("sigma", t.sigma);
  if (t.t.size() != t.v.size() || (!t.sigma.empty() && t.sigma.size() != t.t.size()))
    if (r.err->empty()) *r.err = r.where + ": trace arrays differ in length";
  return t;
}

reduction::FitSpec read_fit(const Rd& r) {
  reduction::FitSpec f;
  std::string kind = "linear", error = "sem";
  r.str("kind", kind);
  r.str("error", error);
  if (auto k = reduction::parse_fit_kind(kind)) f.kind = *k;
  else if (r.err->empty()) *r.err = r.where + ".kind: unknown fit kind '" + kind + "'";
  f.error = error == "sd" ? reduction::ErrorType::Sd : reduction::ErrorType::Sem;
  r.integer("degree", f.degree);
  r.boolean("outliers_enabled", f.outliers.enabled);
  r.integer("outliers_iterations", f.outliers.iterations);
  r.num("outliers_std_devs", f.outliers.std_devs);
  return f;
}

Result<AnalysisRecord> from_table(const toml::table& root) {
  std::string err;
  const Rd top{&root, "record", &err};
  AnalysisRecord r;

  const auto pv = top.sub("provenance");
  pv.integer("schema_version", r.provenance.schema_version);
  if (err.empty() && r.provenance.schema_version != kRecordSchemaVersion)
    return fail(ErrorKind::Config, "unsupported record schema_version " + std::to_string(r.provenance.schema_version) +
                                       " (expected " + std::to_string(kRecordSchemaVersion) + ")");
  pv.str("sha", r.provenance.sha);
  pv.strs("persister_refs", r.provenance.persister_refs);

  const auto id = top.sub("identity");
  id.str("uuid", r.identity.uuid);
  id.str("identifier", r.identity.identifier);
  id.integer("aliquot", r.identity.aliquot);
  id.str("step", r.identity.step);
  id.str("analysis_type", r.identity.analysis_type);
  id.str("timestamp", r.identity.timestamp);
  id.integer("run_index", r.identity.run_index);
  id.str("queue_uuid", r.identity.queue_uuid);

  const auto sm = top.sub("sample");
  sm.str("sample", r.sample.sample);
  sm.str("project", r.sample.project);
  sm.str("material", r.sample.material);
  sm.str("irradiation", r.sample.irradiation);
  sm.str("level", r.sample.level);
  sm.str("position", r.sample.position);
  sm.str("pi", r.sample.pi);
  sm.str("note", r.sample.note);

  const auto in = top.sub("instrument");
  in.str("mass_spectrometer", r.instrument.mass_spectrometer);
  in.str("extract_device", r.instrument.extract_device);
  in.str("laboratory", r.instrument.laboratory);
  in.str("analyst", r.instrument.analyst);
  in.str("software_version", r.instrument.software_version);
  in.str("software_git_sha", r.instrument.software_git_sha);

  const auto ex = top.sub("extraction");
  const auto spec = ex.sub("spec");
  spec.num("value", r.extraction.spec.value);
  spec.num("duration", r.extraction.spec.duration);
  spec.num("cleanup", r.extraction.spec.cleanup);
  spec.str("units", r.extraction.spec.units);
  spec.str("pattern", r.extraction.spec.pattern);
  spec.nums("positions", r.extraction.spec.positions);
  const auto act = ex.sub("actuals");
  auto& a = r.extraction.actuals;
  act.num("value", a.value);
  act.num("duration", a.duration);
  act.num("cleanup", a.cleanup);
  act.num("beam_diameter", a.beam_diameter);
  act.nums("positions", a.positions);
  act.str("pattern", a.pattern);
  act.dmap("pid_params", a.pid_params);
  act.each_table("series", [&](const std::string& k, const Rd& e) { a.series[k] = read_trace(e); });
  act.strs("snapshot_refs", a.snapshot_refs);
  if (const auto* gp = act.find("grain_polygons")) {
    const auto* polys = gp->as_array();
    if (!polys) act.fail("grain_polygons", "array");
    else
      for (const auto& poly : *polys) {
        std::vector<std::pair<double, double>> pts;
        const auto* pa = poly.as_array();
        if (!pa) { act.fail("grain_polygons", "array of arrays"); break; }
        for (const auto& pt : *pa) {
          const auto* xy = pt.as_array();
          if (!xy || xy->size() != 2 || !xy->get(0)->value<double>() || !xy->get(1)->value<double>()) {
            act.fail("grain_polygons", "[x, y] pairs");
            break;
          }
          pts.emplace_back(*xy->get(0)->value<double>(), *xy->get(1)->value<double>());
        }
        a.grain_polygons.push_back(std::move(pts));
      }
  }
  act.integer("pipette_counts", a.pipette_counts);
  act.opt_num("manometer_pressure", a.manometer_pressure);

  const auto ms = top.sub("measurement");
  const auto plan = ms.sub("plan");
  plan.str("template", r.measurement.plan.template_name);
  plan.str("version", r.measurement.plan.version);
  plan.str("effective_plan_toml", r.measurement.plan.effective_plan_toml);
  const auto ov = plan.sub("overrides");
  if (ov.t)
    for (const auto& [k, v] : *ov.t) {
      auto s = v.value<std::string>();
      if (!s) ov.fail(std::string(k.str()), "string");
      else r.measurement.plan.overrides[std::string(k.str())] = *s;
    }
  if (ms.find("hook")) {
    HookRef h;
    const auto hk = ms.sub("hook");
    hk.str("name", h.name);
    hk.str("sha", h.sha);
    r.measurement.hook = h;
  }
  ms.each_table("scripts", [&](const std::string& k, const Rd& e) {
    ScriptRef s;
    e.str("name", s.name);
    e.str("sha", s.sha);
    e.str("text_ref", s.text_ref);
    r.measurement.scripts[k] = s;
  });

  const auto sp = top.sub("spectrometer");
  sp.str("state_hash", r.spectrometer.state_hash);
  sp.str("field_table_version", r.spectrometer.field_table_version);
  sp.num("integration_time", r.spectrometer.integration_time);
  sp.dmap("deflections", r.spectrometer.deflections);
  sp.dmap("gains", r.spectrometer.gains);
  sp.dmap("source_params", r.spectrometer.source_params);

  const auto data = top.sub("data");
  data.num("time_zero", r.data.time_zero);
  if (const auto* sa = data.find("series")) {
    if (const auto* arr = sa->as_array()) {
      int i = 0;
      for (const auto& e : *arr) {
        const auto* et = e.as_table();
        if (!et) { data.fail("series", "array of tables"); break; }
        const Rd er{et, data.where + ".series[" + std::to_string(i++) + "]", &err};
        DataSeries s;
        er.str("iso", s.iso);
        er.str("det", s.det);
        er.str("kind", s.kind);
        s.trace = read_trace(er);
        r.data.series.push_back(std::move(s));
      }
    } else data.fail("series", "array");
  }
  const auto counts = data.sub("counts");
  if (counts.t)
    for (const auto& [k, v] : *counts.t) {
      auto n = v.value<std::int64_t>();
      if (!n) counts.fail(std::string(k.str()), "integer");
      else r.data.counts[std::string(k.str())] = static_cast<int>(*n);
    }

  const auto res = top.sub("results");
  if (const auto s = res.sub("intercepts"); s.t)
    for (const auto& [k, v] : *s.t) {
      if (!v.is_table()) { s.fail(std::string(k.str()), "table"); break; }
      const Rd e{v.as_table(), s.where + "." + std::string(k.str()), &err};
      InterceptResult ir;
      e.num("value", ir.intercept.value);
      e.num("error", ir.intercept.error);
      e.integer("n_used", ir.intercept.n_used);
      e.nums("filtered_idx", ir.intercept.filtered_idx);
      e.num("residual_sd", ir.intercept.residual_sd);
      ir.fit = read_fit(e.sub("fit"));
      r.results.intercepts[std::string(k.str())] = std::move(ir);
    }
  res.each_table("baselines", [&](const std::string& k, const Rd& e) {
    BaselineResult b;
    e.num("value", b.value);
    e.num("error", b.error);
    b.fit = read_fit(e.sub("fit"));
    r.results.baselines[k] = std::move(b);
  });
  res.str("blanks_ref", r.results.blanks_ref);
  res.dmap("icfactors", r.results.icfactors);
  res.str("whiff", r.results.whiff);

  const auto cond = top.sub("conditionals");
  cond.each_entry("installed", [&](const Rd& e) {
    auto& c = r.conditionals.installed.emplace_back();
    e.str("id", c.id);
    e.str("name", c.name);
    e.str("kind", c.kind);
    e.str("level", c.level);
    e.str("location", c.location);
    e.str("check", c.check);
    e.integer("start", c.start);
    e.integer("frequency", c.frequency);
    e.integer("ntrips", c.ntrips);
    e.integer("window", c.window);
    e.str("mapper", c.mapper);
    e.strs("analysis_types", c.analysis_types);
    e.num("abbreviated_count_ratio", c.abbreviated_count_ratio);
    e.str("action", c.action);
    e.boolean("resume", c.resume);
    e.boolean("truncate", c.truncate);
    e.boolean("terminate", c.terminate);
  });
  cond.each_entry("tripped", [&](const Rd& e) {
    auto& c = r.conditionals.tripped.emplace_back();
    e.str("id", c.id);
    e.str("name", c.name);
    e.str("kind", c.kind);
    e.str("check", c.check);
    e.str("action", c.action);
    e.integer("reading", c.reading);
    e.integer("count", c.count);
    e.num("t", c.t);
    e.num("value", c.value);
    e.dmap("context", c.context);
  });
  cond.each_entry("errors", [&](const Rd& e) {
    auto& c = r.conditionals.errors.emplace_back();
    e.str("name", c.name);
    e.str("message", c.message);
    e.integer("count", c.count);
  });

  if (const auto* ea = root.get("events")) {
    if (const auto* arr = ea->as_array()) {
      int i = 0;
      for (const auto& e : *arr) {
        const auto* et = e.as_table();
        if (!et) { top.fail("events", "array of tables"); break; }
        const Rd er{et, "record.events[" + std::to_string(i++) + "]", &err};
        Event ev;
        er.num("t", ev.t);
        er.str("kind", ev.kind);
        er.str("detail", ev.detail);
        r.events.push_back(std::move(ev));
      }
    } else top.fail("events", "array");
  }

  if (!err.empty()) return fail(ErrorKind::Config, err);
  return r;
}

// ---- minimal JSON reader -> toml tree --------------------------------------

class Json {
 public:
  explicit Json(std::string_view s) : s_(s) {}

  bool parse(toml::table& out, std::string& err) {
    ws();
    if (!object(out)) { err = err_.empty() ? "json: expected object" : err_; return false; }
    ws();
    if (i_ != s_.size()) { err = "json: trailing characters at offset " + std::to_string(i_); return false; }
    return true;
  }

 private:
  using Val = std::variant<toml::table, toml::array, std::string, std::int64_t, double, bool>;

  void ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
  bool eat(char c) { ws(); if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; } return false; }
  bool bad(const char* m) { if (err_.empty()) err_ = std::string("json: ") + m + " at offset " + std::to_string(i_); return false; }

  bool string(std::string& out) {
    ws();
    if (i_ >= s_.size() || s_[i_] != '"') return bad("expected string");
    ++i_;
    out.clear();
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return true;
      if (c != '\\') { out.push_back(c); continue; }
      if (i_ >= s_.size()) break;
      switch (char e = s_[i_++]) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case '"': case '\\': case '/': out.push_back(e); break;
        case 'u': {
          if (i_ + 4 > s_.size()) return bad("bad \\u escape");
          unsigned cp = 0;
          auto [p, ec] = std::from_chars(s_.data() + i_, s_.data() + i_ + 4, cp, 16);
          if (ec != std::errc() || p != s_.data() + i_ + 4) return bad("bad \\u escape");
          i_ += 4;
          if (cp < 0x80) out.push_back(static_cast<char>(cp));
          else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 0x3F))); }
          else { out.push_back(char(0xE0 | (cp >> 12))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
          break;
        }
        default: return bad("bad escape");
      }
    }
    return bad("unterminated string");
  }

  bool object(toml::table& out) {
    if (!eat('{')) return bad("expected '{'");
    if (eat('}')) return true;
    do {
      std::string k;
      if (!string(k) || !eat(':')) return err_.empty() ? bad("expected ':'") : false;
      Val v;
      if (!value(v)) return false;
      insert(out, k, std::move(v));
    } while (eat(','));
    return eat('}') || bad("expected '}'");
  }

  bool array(toml::array& out) {
    if (!eat('[')) return bad("expected '['");
    if (eat(']')) return true;
    do {
      Val v;
      if (!value(v)) return false;
      std::visit([&](auto&& x) { out.push_back(std::move(x)); }, std::move(v));
    } while (eat(','));
    return eat(']') || bad("expected ']'");
  }

  bool value(Val& v) {
    ws();
    if (i_ >= s_.size()) return bad("unexpected end");
    const char c = s_[i_];
    if (c == '{') { toml::table t; if (!object(t)) return false; v = std::move(t); return true; }
    if (c == '[') { toml::array a; if (!array(a)) return false; v = std::move(a); return true; }
    if (c == '"') { std::string s; if (!string(s)) return false; v = std::move(s); return true; }
    if (s_.substr(i_, 4) == "true") { i_ += 4; v = true; return true; }
    if (s_.substr(i_, 5) == "false") { i_ += 5; v = false; return true; }
    const std::size_t start = i_;
    bool is_float = false;
    while (i_ < s_.size() && std::string_view("+-0123456789.eE").find(s_[i_]) != std::string_view::npos) {
      if (s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E') is_float = true;
      ++i_;
    }
    if (start == i_) return bad("unexpected token");
    const std::string tok(s_.substr(start, i_ - start));
    if (is_float) {
      char* end = nullptr;
      const double d = std::strtod(tok.c_str(), &end);
      if (*end) return bad("bad number");
      v = d;
    } else {
      std::int64_t n = 0;
      auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), n);
      if (ec != std::errc() || p != tok.data() + tok.size()) return bad("bad number");
      v = n;
    }
    return true;
  }

  static void insert(toml::table& t, const std::string& k, Val v) {
    std::visit([&](auto&& x) { t.insert_or_assign(k, std::move(x)); }, std::move(v));
  }

  std::string_view s_;
  std::size_t i_ = 0;
  std::string err_;
};

}  // namespace

std::string to_toml(const AnalysisRecord& rec) {
  std::ostringstream os;
  os << to_table(rec);
  return os.str();
}

std::string to_json(const AnalysisRecord& rec) {
  std::ostringstream os;
  os << toml::json_formatter{to_table(rec)} << '\n';
  return os.str();
}

Result<AnalysisRecord> from_toml(std::string_view text) {
  auto parsed = toml::parse(text);
  if (!parsed) return fail(ErrorKind::Config, "toml: " + std::string(parsed.error().description()));
  return from_table(parsed.table());
}

Result<AnalysisRecord> from_json(std::string_view text) {
  toml::table root;
  std::string err;
  if (!Json(text).parse(root, err)) return fail(ErrorKind::Config, err);
  return from_table(root);
}

std::string compute_sha(const AnalysisRecord& rec) {
  AnalysisRecord copy = rec;
  copy.provenance.sha.clear();
  return sha256_hex(to_toml(copy));
}

bool verify_sha(const AnalysisRecord& rec) { return !rec.provenance.sha.empty() && rec.provenance.sha == compute_sha(rec); }

}  // namespace pychron::experiment::record
