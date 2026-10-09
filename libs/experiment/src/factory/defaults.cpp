#include "pychron/experiment/factory/defaults.hpp"

#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>

#include <toml++/toml.hpp>

#include "pychron/experiment/model/rules.hpp"

namespace pychron::experiment {
namespace {

constexpr std::string_view kAnyDevice = "*";

class Parser {
 public:
  explicit Parser(std::string_view file) : file_(file) {}

  void add(const std::string& where, const std::string& msg) {
    if (!errors_.empty()) errors_ += "; ";
    errors_ += std::string(file_) + ": " + where + ": " + msg;
  }
  bool ok() const { return errors_.empty(); }
  const std::string& errors() const { return errors_; }

  void check_keys(const toml::table& t, std::initializer_list<std::string_view> allowed, const std::string& where) {
    std::set<std::string_view> ok(allowed);
    for (const auto& [k, v] : t)
      if (!ok.contains(k.str())) add(where, "unknown key '" + std::string(k.str()) + "'");
  }

  void str(const toml::table& t, std::string_view key, std::string& out, const std::string& where) {
    if (const auto* n = t.get(key)) {
      if (auto v = n->value<std::string>()) out = *v;
      else add(where, "'" + std::string(key) + "' must be a string");
    }
  }
  void opt_str(const toml::table& t, std::string_view key, std::optional<std::string>& out, const std::string& where) {
    if (t.contains(key)) str(t, key, out.emplace(), where);
  }
  void num(const toml::table& t, std::string_view key, std::optional<double>& out, const std::string& where) {
    if (const auto* n = t.get(key)) {
      if (auto v = n->value<double>()) out = *v;
      else add(where, "'" + std::string(key) + "' must be a number");
    }
  }
  void dur(const toml::table& t, std::string_view key, std::optional<Duration>& out, const std::string& where) {
    std::optional<double> d;
    num(t, key, d, where);
    if (d) out = Duration(*d);
  }

  RunDefaults entry(const toml::table& t, const std::string& where) {
    RunDefaults d;
    check_keys(t, {"template", "script", "post_equilibration", "post_measurement", "extraction", "overrides", "sample"},
               where);
    str(t, "template", d.template_name, where);
    str(t, "script", d.script, where);
    opt_str(t, "post_equilibration", d.post_equilibration, where);
    opt_str(t, "post_measurement", d.post_measurement, where);

    if (const auto* e = t["extraction"].as_table()) {
      const std::string ew = where + ".extraction";
      check_keys(*e, {"units", "value", "duration", "cleanup", "pre_cleanup", "post_cleanup"}, ew);
      std::string units;
      str(*e, "units", units, ew);
      if (!units.empty()) {
        if (auto u = parse_unit(units)) d.units = *u;
        else add(ew, "unknown units '" + units + "'");
      }
      num(*e, "value", d.value, ew);
      dur(*e, "duration", d.duration, ew);
      dur(*e, "cleanup", d.cleanup, ew);
      dur(*e, "pre_cleanup", d.pre_cleanup, ew);
      dur(*e, "post_cleanup", d.post_cleanup, ew);
    } else if (t.contains("extraction")) {
      add(where, "'extraction' must be a table");
    }

    if (const auto* sm = t["sample"].as_table()) {
      const std::string sw = where + ".sample";
      check_keys(*sm, {"sample", "material", "project"}, sw);
      SampleInfo info;
      str(*sm, "sample", info.sample, sw);
      str(*sm, "material", info.material, sw);
      str(*sm, "project", info.project, sw);
      d.sample = std::move(info);
    } else if (t.contains("sample")) {
      add(where, "'sample' must be a table");
    }

    if (const auto* o = t["overrides"].as_table()) {
      for (const auto& [k, n] : *o) {
        const std::string key(k.str());
        switch (n.type()) {
          case toml::node_type::boolean: d.overrides[key] = n.as_boolean()->get(); break;
          case toml::node_type::integer: d.overrides[key] = static_cast<std::int64_t>(n.as_integer()->get()); break;
          case toml::node_type::floating_point: d.overrides[key] = n.as_floating_point()->get(); break;
          case toml::node_type::string: d.overrides[key] = n.as_string()->get(); break;
          default: add(where + ".overrides", "override '" + key + "' must be a scalar");
        }
      }
    } else if (t.contains("overrides")) {
      add(where, "'overrides' must be a table");
    }
    return d;
  }

 private:
  std::string_view file_;
  std::string errors_;
};

}  // namespace

Result<DefaultsTable> DefaultsTable::from_toml(std::string_view text, std::string_view name) {
  auto parsed = toml::parse(text, name);
  if (!parsed) return fail(ErrorKind::Config, std::string(name) + ": syntax error: " + std::string(parsed.error().description()));
  Parser p(name);
  DefaultsTable table;
  for (const auto& [type_key, type_node] : parsed.table()) {
    const std::string tw(type_key.str());
    auto type = parse_analysis_type(tw);
    if (!type) {
      p.add(tw, "unknown analysis type");
      continue;
    }
    const auto* devices = type_node.as_table();
    if (!devices) {
      p.add(tw, "must be a table of [" + tw + ".<extract_device>] entries");
      continue;
    }
    for (const auto& [dev_key, dev_node] : *devices) {
      const std::string where = tw + "." + std::string(dev_key.str());
      const auto* entry = dev_node.as_table();
      if (!entry) {
        p.add(where, "must be a table");
        continue;
      }
      table.set(*type, std::string(dev_key.str()), p.entry(*entry, where));
    }
  }
  if (!p.ok()) return fail(ErrorKind::Config, p.errors());
  return table;
}

Result<DefaultsTable> DefaultsTable::load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return from_toml(ss.str(), path);
}

const RunDefaults* DefaultsTable::find(AnalysisType type, std::string_view device) const {
  if (auto it = entries_.find(std::pair{type, std::string(device)}); it != entries_.end()) return &it->second;
  if (auto it = entries_.find(std::pair{type, std::string(kAnyDevice)}); it != entries_.end()) return &it->second;
  return nullptr;
}

void DefaultsTable::set(AnalysisType type, std::string device, RunDefaults defaults) {
  entries_.insert_or_assign(std::pair{type, std::move(device)}, std::move(defaults));
}

void apply_defaults(RunSpec& run, const RunDefaults& d) {
  run.measurement.plan = d.template_name;
  run.measurement.overrides = d.overrides;
  run.extraction.script = d.script;
  run.post_equilibration = d.post_equilibration;
  run.post_measurement = d.post_measurement;
  auto& e = run.extraction;
  if (d.units) e.units = *d.units;
  if (d.value) e.value = *d.value;
  if (d.duration) e.duration = *d.duration;
  if (d.cleanup) e.cleanup = *d.cleanup;
  if (d.pre_cleanup) e.pre_cleanup = *d.pre_cleanup;
  if (d.post_cleanup) e.post_cleanup = *d.post_cleanup;
  if (d.sample) run.sample = *d.sample;
}

void strip_for_type(RunSpec& run) {
  const FieldRules f = rules_for(run.id.type);
  auto& e = run.extraction;
  if (!f.extraction) {
    e.device.clear();
    e.script.clear();
    e.options.clear();
    e.duration = e.cleanup = e.pre_cleanup = e.post_cleanup = Duration(0);
  }
  if (!f.heating) {
    e.value = 0;
    e.pattern.reset();
    e.beam_diameter.reset();
    e.ramp_rate.reset();
    e.ramp = Duration(0);
    e.cryo_temp.reset();
  }
  if (!f.position) e.position.reset();
  if (!f.measurement) run.measurement = MeasurementRef{};
}

Result<RunSpec> make_run(std::string_view identifier, const IdentifierRules& ids, std::string_view extract_device,
                         const DefaultsTable& defaults) {
  if (auto ok = ids.validate_identifier(identifier); !ok) return fail(ok.error());
  RunSpec run;
  run.id.identifier = std::string(identifier);
  run.id.type = ids.classify(identifier);
  run.extraction.device = std::string(extract_device);
  if (const RunDefaults* d = defaults.find(run.id.type, extract_device)) apply_defaults(run, *d);
  strip_for_type(run);
  return run;
}

Result<RunSpec> make_special_run(AnalysisType type, const IdentifierRules& ids, std::string_view extract_device,
                                 const DefaultsTable& defaults) {
  const std::string prefix = ids.prefix_for(type);
  if (prefix.empty())
    return fail(ErrorKind::Config, "no special identifier for analysis type '" + std::string(to_string(type)) + "'");
  return make_run(prefix, ids, extract_device, defaults);
}

}  // namespace pychron::experiment
