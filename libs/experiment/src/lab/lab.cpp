#include "pychron/experiment/lab/lab.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include "pychron/core/config/loader.hpp"

namespace pychron::experiment::lab {

namespace fs = std::filesystem;

namespace {

std::string read_text(const fs::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

LabScripts::LabScripts(fs::path root) : root_(root), resolver_(std::move(root)) {}

std::vector<std::string> LabScripts::names(scripting::ScriptKind kind) const {
  std::vector<std::string> out;
  const fs::path dir = root_ / std::string(scripting::to_string(kind));
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return out;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".py") continue;
    fs::path rel = fs::relative(it->path(), dir, ec);
    rel.replace_extension();
    std::string name;
    for (const auto& part : rel) name += (name.empty() ? "" : ":") + part.string();
    out.push_back(std::move(name));
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool LabScripts::has_script(std::string_view name) const {
  using K = scripting::ScriptKind;
  for (auto kind : {K::Extraction, K::PostEquilibration, K::PostMeasurement, K::MeasurementHook})
    if (resolver_.resolve(name, kind)) return true;
  return false;
}

bool LabConditionals::has_conditional(std::string_view name, std::string_view) const {
  return has_conditional_set(name);
}

bool LabConditionals::has_conditional_set(std::string_view name) const {
  auto t = source_.text(name);
  return t && t->has_value();
}

MetricCatalog Lab::metric_catalog() const {
  MetricCatalog c;
  if (line)
    for (const auto& g : line->gauges) c.gauges.insert(g.name);
  if (spectrometer) {
    for (const auto& d : spectrometer->config.detectors) c.detectors.insert(d.name);
    for (const auto& [name, table] : spectrometer->tables)
      for (const auto& p : table.points) c.isotopes.insert(p.isotope);
  }
  return c;
}

Lab load_lab(const LabPaths& paths) {
  Lab lab;
  lab.paths = paths;
  const fs::path& dir = paths.dir;
  std::error_code ec;
  if (fs::exists(dir / "identifiers.toml", ec)) {
    auto ids = IdentifierRules::load((dir / "identifiers.toml").string());
    if (ids) lab.ids = *ids;
    else lab.problems.push_back(ids.error().what);
  }
  if (!paths.line_config.empty() && fs::exists(paths.line_config, ec)) {
    auto report = config::load_report(paths.line_config);
    if (report.ok()) {
      lab.line = std::move(*report.config);
    } else {
      for (const auto& d : report.diagnostics) lab.problems.push_back(config::to_string(d));
    }
  }
  if (!paths.spectrometer.empty()) {
    auto data = spectrometer::cfg::load_spectrometer(paths.spectrometer);
    if (data) lab.spectrometer = std::move(*data);
    else lab.problems.push_back(data.error().what);
  }
  if (lab.line) lab.aliases = std::make_unique<measurement::SystemConfigAliases>(*lab.line);
  if (lab.spectrometer) lab.catalog = std::make_unique<measurement::SpectrometerCatalog>(lab.spectrometer->config);
  lab.plans = std::make_unique<plan::PlanLibrary>(plan::PlanResolvers{lab.aliases.get(), lab.catalog.get()});
  if (fs::is_directory(dir / "plans", ec)) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir / "plans", ec))
      if (e.path().extension() == ".toml") files.push_back(e.path());
    std::sort(files.begin(), files.end());  // directory order is unspecified
    for (const auto& f : files) {
      auto t = plan::parse_plan_template(read_text(f), f.filename().string());
      if (t) lab.plans->add(std::move(*t));
      else lab.problems.push_back(t.error().what);
    }
  }
  lab.condition_source = std::make_unique<DirectoryConditionalSource>(dir / "conditionals");
  lab.conditionals = std::make_unique<ConditionalLibrary>(*lab.condition_source);
  lab.condition_files = std::make_unique<ConditionalFiles>(dir / "conditionals");
  lab.condition_names = std::make_unique<LabConditionals>(*lab.condition_source);
  lab.scripts = std::make_unique<LabScripts>(dir / "scripts");
  if (fs::exists(dir / "peak_center.toml", ec)) {
    auto pc = jobs::parse_peak_center_configs(read_text(dir / "peak_center.toml"), (dir / "peak_center.toml").string());
    if (pc) lab.peak_centers = std::move(*pc);
    else lab.problems.push_back(pc.error().what);
  }
  if (fs::exists(dir / "defaults.toml", ec)) {
    auto d = DefaultsTable::load((dir / "defaults.toml").string());
    if (d) lab.defaults = std::move(*d);
    else lab.problems.push_back(d.error().what);
  }
  if (fs::exists(dir / "notifications.toml", ec)) {
    auto n = NotificationConfig::load(dir / "notifications.toml");
    if (n) lab.notifications = std::move(*n);
    else lab.problems.push_back(n.error().what);
  }
  if (fs::is_directory(dir / "blocks", ec)) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir / "blocks", ec))
      if (e.path().extension() == ".toml") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      auto b = load_block(f.string(), lab.ids);
      if (b) lab.blocks[b->name.empty() ? f.stem().string() : b->name] = std::move(*b);
      else lab.problems.push_back(b.error().what);
    }
  }
  return lab;
}

bool LabCheck::ok() const {
  if (!report.ok()) return false;
  for (const auto& d : extra)
    if (d.severity == Severity::Error) return false;
  return true;
}

std::vector<Diagnostic> LabCheck::all() const {
  auto out = report.diagnostics;
  out.insert(out.end(), extra.begin(), extra.end());
  return out;
}

LabCheck check_lab_queue(const Lab& lab, const QueueSpec& queue) {
  LabCheck out;
  for (const auto& p : lab.problems) out.extra.push_back({Severity::Error, -1, "lab", p});
  out.report = check_queue(queue, lab.ids, lab.resolvers());

  const auto catalog = lab.metric_catalog();
  std::set<std::string> reported;
  for (std::size_t i = 0; i < queue.runs.size(); ++i) {
    const auto& r = queue.runs[i];
    const int row = static_cast<int>(i);
    if (r.skip || r.measurement.plan.empty() || !lab.plans->find(r.measurement.plan)) continue;
    auto loaded = lab.plans->load(r.measurement.plan, r.measurement.overrides, plan::LoadOptions{r.measurement.advanced});
    if (!loaded) {
      // A known plan that does not load with this run's overrides (not
      // exposed, wrong type, ...) would only fail when the run starts.
      out.extra.push_back({Severity::Error, row, "measurement", loaded.error().what});
      continue;
    }
    const auto& pc = loaded->plan.peak_center;
    if ((pc.before || pc.after) && pc.config != "default" && !lab.peak_centers.contains(pc.config)) {
      if (reported.insert("peak_center:" + pc.config).second) {
        out.extra.push_back({Severity::Error, row, "peak_center",
                             "plan " + r.measurement.plan + " uses peak center config '" + pc.config +
                                 "', which is not in " + (lab.paths.dir / "peak_center.toml").string()});
      }
    }
    auto set = lab.conditionals->for_run(queue, r, loaded->plan);
    if (!set) {
      out.extra.push_back({Severity::Error, row, "conditionals", set.error().what});
      continue;
    }
    for (const auto& d : validate_conditionals(*set, catalog)) {
      if (!reported.insert(d.conditional + d.message).second) continue;
      out.extra.push_back({d.error ? Severity::Error : Severity::Warning, row, "conditionals",
                           "conditional " + d.conditional + ": " + d.message});
    }
  }
  return out;
}

std::string describe(const Diagnostic& d) {
  std::string where;
  if (d.run >= 0) where = "runs[" + std::to_string(d.run) + "]." + d.field;
  else if (d.field == "lab" || d.field.starts_with("queue.")) where = d.field;
  else where = "queue." + d.field;
  return where + ": " + d.message;
}

}  // namespace pychron::experiment::lab
