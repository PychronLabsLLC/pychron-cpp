#include "pychron/experiment/model/queue_toml.hpp"

#include <initializer_list>
#include <set>

#include <toml++/toml.hpp>

#include "pychron/experiment/model/positions.hpp"

namespace pychron::experiment {
namespace {

// Flat / aliased run keys -> "section.key" (empty section = top level).
struct Alias {
  std::string_view from, section, to;
};
constexpr Alias kRunAliases[] = {
    {"e_value", "extraction", "value"},          {"extract_value", "extraction", "value"},
    {"value", "extraction", "value"},            {"extract_units", "extraction", "units"},
    {"units", "extraction", "units"},            {"extract_device", "extraction", "device"},
    {"device", "extraction", "device"},          {"duration", "extraction", "duration"},
    {"cleanup", "extraction", "cleanup"},        {"pre_cleanup", "extraction", "pre_cleanup"},
    {"post_cleanup", "extraction", "post_cleanup"}, {"position", "extraction", "position"},
    {"pattern", "extraction", "pattern"},        {"beam_diameter", "extraction", "beam_diameter"},
    {"ramp_rate", "extraction", "ramp_rate"},    {"ramp", "extraction", "ramp"},
    {"cryo_temp", "extraction", "cryo_temp"},    {"script", "extraction", "script"},
    {"s_opt", "extraction", "options"},          {"script_options", "extraction", "options"},
    {"options", "extraction", "options"},        {"t_o", "", "truncate"},
};

const Alias* find_alias(std::string_view key) {
  for (const auto& a : kRunAliases)
    if (a.from == key) return &a;
  return nullptr;
}

class Errors {
 public:
  explicit Errors(std::string_view file) : file_(file) {}
  void add(const std::string& where, const std::string& msg) {
    if (!text_.empty()) text_ += "; ";
    text_ += std::string(file_) + ": " + where + ": " + msg;
  }
  bool any() const { return !text_.empty(); }
  const std::string& text() const { return text_; }

 private:
  std::string_view file_;
  std::string text_;
};

void check_keys(const toml::table& t, std::initializer_list<std::string_view> allowed, const std::string& where, Errors& err) {
  std::set<std::string_view> ok(allowed);
  for (const auto& [k, v] : t)
    if (!ok.count(k.str())) err.add(where, "unknown key '" + std::string(k.str()) + "'");
}

class Reader {
 public:
  Reader(const toml::table& t, std::string where, Errors& err) : t_(t), where_(std::move(where)), err_(err) {}

  void str(std::string_view key, std::string& out) const {
    if (auto* n = t_.get(key)) {
      if (auto v = n->value<std::string>()) out = *v;
      else bad(key, "a string");
    }
  }
  void opt_str(std::string_view key, std::optional<std::string>& out) const {
    if (auto* n = t_.get(key)) {
      if (auto v = n->value<std::string>()) out = *v;
      else bad(key, "a string");
    }
  }
  void num(std::string_view key, double& out) const {
    if (auto* n = t_.get(key)) {
      if (auto v = n->value<double>()) out = *v;
      else bad(key, "a number");
    }
  }
  void opt_num(std::string_view key, std::optional<double>& out) const {
    if (auto* n = t_.get(key)) {
      if (auto v = n->value<double>()) out = *v;
      else bad(key, "a number");
    }
  }
  void dur(std::string_view key, Duration& out) const {
    double d = out.count();
    num(key, d);
    out = Duration(d);
  }
  void opt_int(std::string_view key, std::optional<int>& out) const {
    if (auto* n = t_.get(key)) {
      if (auto v = n->value<std::int64_t>()) out = static_cast<int>(*v);
      else bad(key, "an integer");
    }
  }
  void boolean(std::string_view key, bool& out) const {
    if (auto* n = t_.get(key)) {
      if (auto v = n->value<bool>()) out = *v;
      else bad(key, "a boolean");
    }
  }
  void bad(std::string_view key, const char* want) const { err_.add(where_, "'" + std::string(key) + "' must be " + want); }

  const toml::table& t_;
  std::string where_;
  Errors& err_;
};

// Copies a run table, moving flat/aliased extraction keys under [extraction].
toml::table normalize_run(const toml::table& in, const std::string& where, Errors& err) {
  toml::table out;
  toml::table extraction;
  std::set<std::string> seen;
  auto put = [&](toml::table& dst, std::string_view key, const toml::node& n, std::string_view from) {
    if (dst.contains(key)) {
      err.add(where, "'" + std::string(from) + "' duplicates '" + std::string(key) + "'");
      return;
    }
    n.visit([&](const auto& v) { dst.insert_or_assign(key, v); });
  };
  for (const auto& [k, n] : in) {
    if (k.str() == "extraction") {
      if (const auto* sub = n.as_table())
        for (const auto& [sk, sn] : *sub) put(extraction, sk.str(), sn, "extraction." + std::string(sk.str()));
      else err.add(where, "'extraction' must be a table");
    } else if (const Alias* a = find_alias(k.str())) {
      put(a->section.empty() ? out : extraction, a->to, n, k.str());
    } else {
      put(out, k.str(), n, k.str());
    }
  }
  if (!extraction.empty()) out.insert_or_assign("extraction", std::move(extraction));
  return out;
}

void read_position(const toml::table& t, std::optional<Position>& out, const std::string& where, Errors& err) {
  const toml::node* n = t.get("position");
  if (!n) return;
  if (auto s = n->value<std::string>()) {
    auto p = parse_position(*s);
    if (p) out = *p;
    else err.add(where, "position: " + p.error().what);
  } else if (auto i = n->value<std::int64_t>()) {
    if (*i < 0) err.add(where, "position: negative hole");
    else out = Position{{static_cast<int>(*i)}};
  } else if (const auto* arr = n->as_array()) {
    Position p;
    for (const auto& e : *arr) {
      auto v = e.value<std::int64_t>();
      if (!v || *v < 0) {
        err.add(where, "position: array must hold non-negative integers");
        return;
      }
      p.holes.push_back(static_cast<int>(*v));
    }
    out = std::move(p);
  } else {
    err.add(where, "position must be a string, integer or array of integers");
  }
}

void read_overrides(const toml::table& t, ParamOverrides& out, const std::string& where, Errors& err) {
  for (const auto& [k, n] : t) {
    const std::string key(k.str());
    switch (n.type()) {
      case toml::node_type::boolean: out[key] = n.as_boolean()->get(); break;
      case toml::node_type::integer: out[key] = static_cast<std::int64_t>(n.as_integer()->get()); break;
      case toml::node_type::floating_point: out[key] = n.as_floating_point()->get(); break;
      case toml::node_type::string: out[key] = n.as_string()->get(); break;
      default: err.add(where, "override '" + key + "' must be a scalar");
    }
  }
}

void read_extraction(const toml::table& t, ExtractionSpec& e, const std::string& where, Errors& err) {
  check_keys(t, {"device", "position", "value", "units", "duration", "cleanup", "pre_cleanup", "post_cleanup", "pattern",
                 "beam_diameter", "ramp_rate", "ramp", "cryo_temp", "script", "options"}, where, err);
  Reader r(t, where, err);
  r.str("device", e.device);
  read_position(t, e.position, where, err);
  r.num("value", e.value);
  std::string units;
  r.str("units", units);
  if (!units.empty()) {
    if (auto u = parse_unit(units)) e.units = *u;
    else err.add(where, "unknown units '" + units + "'");
  }
  r.dur("duration", e.duration);
  r.dur("cleanup", e.cleanup);
  r.dur("pre_cleanup", e.pre_cleanup);
  r.dur("post_cleanup", e.post_cleanup);
  r.opt_str("pattern", e.pattern);
  r.opt_num("beam_diameter", e.beam_diameter);
  r.opt_num("ramp_rate", e.ramp_rate);
  r.dur("ramp", e.ramp);
  r.opt_num("cryo_temp", e.cryo_temp);
  r.str("script", e.script);
  r.str("options", e.options);
}

void read_run(const toml::table& raw, const QueueSpec& q, const IdentifierRules& ids, RunSpec& run, const std::string& where,
              Errors& err) {
  toml::table t = normalize_run(raw, where, err);
  check_keys(t, {"identifier", "aliquot", "step", "extraction", "measurement", "post_equilibration", "post_measurement",
                 "overlap", "overlap_min", "delay_after", "conditionals", "truncate", "comment", "weight", "skip",
                 "end_after", "sample"}, where, err);
  Reader r(t, where, err);
  r.str("identifier", run.id.identifier);
  r.opt_int("aliquot", run.id.aliquot);
  r.str("step", run.id.step);
  run.id.type = ids.classify(run.id.identifier);
  run.extraction.device = q.extract_device;
  if (const auto* e = t["extraction"].as_table()) read_extraction(*e, run.extraction, where + ".extraction", err);

  if (const auto* m = t["measurement"].as_table()) {
    const std::string mw = where + ".measurement";
    check_keys(*m, {"plan", "hook", "advanced", "overrides"}, mw, err);
    Reader mr(*m, mw, err);
    mr.str("plan", run.measurement.plan);
    mr.opt_str("hook", run.measurement.hook);
    mr.boolean("advanced", run.measurement.advanced);
    if (const auto* o = (*m)["overrides"].as_table()) read_overrides(*o, run.measurement.overrides, mw + ".overrides", err);
  } else if (t.contains("measurement")) {
    err.add(where, "'measurement' must be a table");
  }
  r.opt_str("post_equilibration", run.post_equilibration);
  r.opt_str("post_measurement", run.post_measurement);
  r.dur("overlap", run.overlap.duration);
  r.dur("overlap_min", run.overlap.min_delay);
  r.dur("delay_after", run.delay_after);
  r.str("comment", run.comment);
  r.opt_num("weight", run.weight);
  r.boolean("skip", run.skip);
  r.boolean("end_after", run.end_after);

  if (const auto* c = t["conditionals"].as_array()) {
    for (const auto& n : *c) {
      if (auto s = n.value<std::string>()) {
        run.conditionals.push_back({*s, "action"});
      } else if (const auto* ct = n.as_table()) {
        const std::string cw = where + ".conditionals";
        check_keys(*ct, {"name", "kind"}, cw, err);
        ConditionalRef ref;
        Reader cr(*ct, cw, err);
        cr.str("name", ref.name);
        cr.str("kind", ref.kind);
        if (ref.name.empty()) err.add(cw, "conditional needs a name");
        else run.conditionals.push_back(std::move(ref));
      } else {
        err.add(where, "conditionals must be strings or {name, kind} tables");
      }
    }
  } else if (t.contains("conditionals")) {
    err.add(where, "'conditionals' must be an array");
  }
  if (const toml::node* n = t.get("truncate")) {
    if (auto s = n->value<std::string>()) run.conditionals.push_back({*s, "truncate"});
    else if (const auto* a = n->as_array()) {
      for (const auto& e : *a) {
        if (auto es = e.value<std::string>()) run.conditionals.push_back({*es, "truncate"});
        else err.add(where, "truncate entries must be strings");
      }
    } else err.add(where, "'truncate' must be a string or array of strings");
  }
  if (const auto* s = t["sample"].as_table()) {
    const std::string sw = where + ".sample";
    check_keys(*s, {"sample", "material", "project", "irradiation", "level", "irradiation_position"}, sw, err);
    Reader sr(*s, sw, err);
    sr.str("sample", run.sample.sample);
    sr.str("material", run.sample.material);
    sr.str("project", run.sample.project);
    sr.str("irradiation", run.sample.irradiation);
    sr.str("level", run.sample.level);
    sr.opt_int("irradiation_position", run.sample.irradiation_position);
  }
}

}  // namespace

Result<QueueSpec> parse_queue(std::string_view text, const IdentifierRules& ids, std::string_view name) {
  auto parsed = toml::parse(text, name);
  if (!parsed) return fail(ErrorKind::Config, std::string(name) + ": syntax error: " + std::string(parsed.error().description()));
  const toml::table& root = parsed.table();
  Errors err(name);
  QueueSpec q;

  check_keys(root, {"queue", "runs"}, "root", err);
  const auto* qt = root["queue"].as_table();
  if (!qt) {
    err.add("root", "missing [queue] table");
  } else {
    check_keys(*qt, {"schema_version", "name", "mass_spectrometer", "extract_device", "tray", "load", "username", "email",
                     "queue_conditionals", "repository", "delays"}, "queue", err);
    Reader r(*qt, "queue", err);
    std::optional<int> version;
    r.opt_int("schema_version", version);
    if (version && *version != kQueueSchemaVersion)
      err.add("queue", "unsupported schema_version " + std::to_string(*version));
    r.str("name", q.name);
    r.str("mass_spectrometer", q.mass_spectrometer);
    r.str("extract_device", q.extract_device);
    r.str("tray", q.tray);
    r.str("load", q.load);
    r.str("username", q.username);
    r.str("email", q.email);
    r.str("queue_conditionals", q.queue_conditionals);
    r.str("repository", q.repository);
    if (const auto* d = (*qt)["delays"].as_table()) {
      check_keys(*d, {"before_analyses", "between_analyses", "after_blank", "extract_delay"}, "queue.delays", err);
      Reader dr(*d, "queue.delays", err);
      dr.dur("before_analyses", q.delays.before_analyses);
      dr.dur("between_analyses", q.delays.between_analyses);
      dr.dur("after_blank", q.delays.after_blank);
      dr.dur("extract_delay", q.delays.extract_delay);
    }
  }
  if (const auto* runs = root["runs"].as_array()) {
    int i = 0;
    for (const auto& n : *runs) {
      const std::string where = "runs[" + std::to_string(i++) + "]";
      const auto* t = n.as_table();
      if (!t) {
        err.add(where, "must be a table");
        continue;
      }
      RunSpec run;
      read_run(*t, q, ids, run, where, err);
      q.runs.push_back(std::move(run));
    }
  } else if (root.contains("runs")) {
    err.add("root", "'runs' must be an array of tables");
  }
  if (err.any()) return fail(ErrorKind::Config, err.text());
  return q;
}

}  // namespace pychron::experiment
