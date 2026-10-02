#include "exp.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <thread>

#include "pychron/core/config/loader.hpp"
#include "pychron/experiment/conditionals/library.hpp"
#include "pychron/experiment/conditionals/validate.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/measurement/adapters.hpp"
#include "pychron/experiment/model/identifiers.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/experiment/model/queue_validation.hpp"
#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/plan/plan_library.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/assembler.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/switch_valve_service.hpp"

namespace elctl {

namespace fs = std::filesystem;
using namespace pychron;
using namespace pychron::experiment;

std::atomic<int>& interrupt_count() {
  static std::atomic<int> n{0};
  return n;
}

namespace {

constexpr const char* kExpUsage =
    "usage: elctl [-c <extraction_line.toml>] [--sim] exp <validate|run> <experiment.toml>\n"
    "         [--lab <dir>] [--data <dir>] [--spectrometer <file>] [--canvas <file>]\n"
    "         [--from <row> | --resume] [--dry-run] [--sim-speed <x>]\n";

std::string clock_text(experiment::Duration d) {
  const auto total = static_cast<long long>(std::llround(d.count()));
  std::ostringstream s;
  s << total / 3600 << ':' << std::setw(2) << std::setfill('0') << (total / 60) % 60 << ':' << std::setw(2)
    << std::setfill('0') << total % 60;
  return s.str();
}

std::string read_text(const fs::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct ExpArgs {
  std::string verb;
  fs::path queue_file, lab, data, spectrometer, canvas;
  std::optional<std::size_t> from;
  bool resume = false, dry_run = false;
  double sim_speed = 0;
};

// Script names exist in any script directory (validation does not know the kind).
class LabScripts final : public experiment::IScriptResolver {
 public:
  explicit LabScripts(fs::path root) : resolver_(std::move(root)) {}
  bool has_script(std::string_view name) const override {
    using K = scripting::ScriptKind;
    for (auto kind : {K::Extraction, K::PostEquilibration, K::PostMeasurement, K::MeasurementHook})
      if (resolver_.resolve(name, kind)) return true;
    return false;
  }
  const scripting::DirectoryScriptResolver& resolver() const { return resolver_; }

 private:
  scripting::DirectoryScriptResolver resolver_;
};

class LabConditionals final : public experiment::IConditionalResolver {
 public:
  explicit LabConditionals(const IConditionalSource& source) : source_(source) {}
  bool has_conditional(std::string_view name, std::string_view) const override { return has_conditional_set(name); }
  bool has_conditional_set(std::string_view name) const override {
    auto t = source_.text(name);
    return t && t->has_value();
  }

 private:
  const IConditionalSource& source_;
};

// Everything that comes from the lab directory, loaded without hardware.
struct Lab {
  fs::path dir, data;
  IdentifierRules ids = IdentifierRules::defaults();
  std::optional<config::SystemConfig> line;
  std::optional<spectrometer::cfg::SpectrometerData> spectrometer;
  std::unique_ptr<measurement::SystemConfigAliases> aliases;
  std::unique_ptr<measurement::SpectrometerCatalog> catalog;
  std::unique_ptr<plan::PlanLibrary> plans;
  std::unique_ptr<DirectoryConditionalSource> condition_source;
  std::unique_ptr<ConditionalLibrary> conditionals;
  std::unique_ptr<LabScripts> scripts;
  std::unique_ptr<LabConditionals> condition_names;
  std::vector<std::string> problems;
};

Lab load_lab(const ExpArgs& a, const ExpGlobals& g) {
  Lab lab;
  lab.dir = a.lab;
  lab.data = a.data;
  if (fs::exists(lab.dir / "identifiers.toml")) {
    auto ids = IdentifierRules::load((lab.dir / "identifiers.toml").string());
    if (ids) lab.ids = *ids;
    else lab.problems.push_back(ids.error().what);
  }
  std::error_code ec;
  if (fs::exists(g.config, ec)) {
    auto report = config::load_report(g.config);
    if (report.ok()) {
      lab.line = std::move(*report.config);
    } else {
      for (const auto& d : report.diagnostics) lab.problems.push_back(config::to_string(d));
    }
  }
  if (!a.spectrometer.empty()) {
    auto data = spectrometer::cfg::load_spectrometer(a.spectrometer);
    if (data) lab.spectrometer = std::move(*data);
    else lab.problems.push_back(data.error().what);
  }
  if (lab.line) lab.aliases = std::make_unique<measurement::SystemConfigAliases>(*lab.line);
  if (lab.spectrometer) lab.catalog = std::make_unique<measurement::SpectrometerCatalog>(lab.spectrometer->config);
  lab.plans = std::make_unique<plan::PlanLibrary>(plan::PlanResolvers{lab.aliases.get(), lab.catalog.get()});
  if (fs::is_directory(lab.dir / "plans", ec)) {
    for (const auto& e : fs::directory_iterator(lab.dir / "plans", ec)) {
      if (e.path().extension() != ".toml") continue;
      auto t = plan::parse_plan_template(read_text(e.path()), e.path().filename().string());
      if (t) lab.plans->add(std::move(*t));
      else lab.problems.push_back(t.error().what);
    }
  }
  lab.condition_source = std::make_unique<DirectoryConditionalSource>(lab.dir / "conditionals");
  lab.conditionals = std::make_unique<ConditionalLibrary>(*lab.condition_source);
  lab.condition_names = std::make_unique<LabConditionals>(*lab.condition_source);
  lab.scripts = std::make_unique<LabScripts>(lab.dir / "scripts");
  return lab;
}

MetricCatalog catalog_for(const Lab& lab) {
  MetricCatalog c;
  if (lab.line)
    for (const auto& g : lab.line->gauges) c.gauges.insert(g.name);
  if (lab.spectrometer) {
    for (const auto& d : lab.spectrometer->config.detectors) c.detectors.insert(d.name);
    for (const auto& [name, table] : lab.spectrometer->tables)
      for (const auto& p : table.points) c.isotopes.insert(p.isotope);
  }
  return c;
}

class Exp {
 public:
  Exp(ExpArgs args, ExpGlobals globals, Io io) : a_(std::move(args)), g_(std::move(globals)), io_(io) {}

  int run() {
    lab_ = load_lab(a_, g_);
    auto q = load_queue_file(a_.queue_file.string(), lab_.ids);
    if (!q) {
      io_.err << "error: " << q.error().what << '\n';
      return kFailed;
    }
    queue_ = std::move(*q);
    const bool ok = report();
    if (a_.verb == "validate" || a_.dry_run) return ok ? kOk : kFailed;
    if (!ok) {
      io_.err << "error: the queue has errors; nothing was run\n";
      return kFailed;
    }
    return execute();
  }

 private:
  // Prints the queue, its estimates and every problem. True when runnable.
  bool report() {
    for (const auto& p : lab_.problems) io_.err << "error: " << p << '\n';
    QueueResolvers resolvers{lab_.plans.get(), lab_.scripts.get(), lab_.condition_names.get()};
    auto rep = check_queue(queue_, lab_.ids, resolvers);
    io_.out << "queue " << (queue_.name.empty() ? a_.queue_file.filename().string() : queue_.name) << ": "
            << queue_.runs.size() << " run(s), ETA " << clock_text(rep.eta) << '\n';
    for (std::size_t i = 0; i < queue_.runs.size(); ++i) {
      const auto& r = queue_.runs[i];
      io_.out << "  " << std::setw(3) << i << "  " << std::left << std::setw(12) << r.id.identifier << std::setw(15)
              << to_string(r.id.type) << std::setw(20) << (r.measurement.plan.empty() ? "-" : r.measurement.plan)
              << std::right << (r.skip ? "skip" : clock_text(i < rep.run_estimates.size() ? rep.run_estimates[i]
                                                                                          : experiment::Duration{}))
              << '\n';
    }
    int errors = static_cast<int>(lab_.problems.size());
    for (const auto& d : rep.diagnostics) {
      const bool error = d.severity == Severity::Error;
      errors += error ? 1 : 0;
      (error ? io_.err : io_.out) << (error ? "error: " : "warning: ")
                                  << (d.run >= 0 ? "runs[" + std::to_string(d.run) + "]." : std::string("queue."))
                                  << d.field << ": " << d.message << '\n';
    }
    // Conditionals that will apply, checked against the lab's names.
    const auto catalog = catalog_for(lab_);
    std::set<std::string> reported;
    for (std::size_t i = 0; i < queue_.runs.size(); ++i) {
      const auto& r = queue_.runs[i];
      if (r.skip || r.measurement.plan.empty() || !lab_.plans->find(r.measurement.plan)) continue;
      auto loaded = lab_.plans->load(r.measurement.plan, r.measurement.overrides);
      if (!loaded) continue;  // check_queue reported it
      auto set = lab_.conditionals->for_run(queue_, r, loaded->plan);
      if (!set) {
        io_.err << "error: runs[" << i << "].conditionals: " << set.error().what << '\n';
        ++errors;
        continue;
      }
      for (const auto& d : validate_conditionals(*set, catalog)) {
        if (!reported.insert(d.conditional + d.message).second) continue;
        (d.error ? io_.err : io_.out) << (d.error ? "error: " : "warning: ") << "conditional " << d.conditional
                                      << ": " << d.message << '\n';
        errors += d.error ? 1 : 0;
      }
    }
    if (errors == 0) io_.out << "ok: " << a_.queue_file.string() << '\n';
    return errors == 0;
  }

  void say(const std::string& line) {
    std::lock_guard lock(out_mutex_);
    io_.out << line << '\n';
    io_.out.flush();
  }

  int execute() {
    if (!lab_.line) {
      io_.err << "error: no extraction line config at " << g_.config.string() << '\n';
      return kFailed;
    }
    // Clock: real time, or simulated time running sim_speed times faster.
    std::unique_ptr<ManualClock> manual;
    std::thread pump;
    std::atomic<bool> pumping{true};
    if (a_.sim_speed > 0) {
      manual = std::make_unique<ManualClock>(TimePoint{} + std::chrono::hours(1));
      pump = std::thread([&] {
        const auto step = std::chrono::duration_cast<pychron::Duration>(std::chrono::duration<double>(0.001 * a_.sim_speed));
        while (pumping) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          manual->advance(step);
        }
      });
    }
    struct PumpGuard {
      std::atomic<bool>& on;
      std::thread& t;
      ~PumpGuard() {
        on = false;
        if (t.joinable()) t.join();
      }
    } pump_guard{pumping, pump};

    systems::ExtractionLine::Options line_options;
    line_options.clock = manual.get();
    line_options.force_sim = g_.sim;
    std::optional<fs::path> canvas;
    if (!a_.canvas.empty()) canvas = a_.canvas;
    else if (fs::exists(g_.config.parent_path() / "canvas.toml")) canvas = g_.config.parent_path() / "canvas.toml";
    auto line = systems::ExtractionLine::load(g_.config, canvas, line_options);
    if (!line) {
      io_.err << "error: " << line.error().what << '\n';
      return kFailed;
    }
    if (auto r = (*line)->start(); !r) {
      io_.err << "error: " << r.error().what << '\n';
      return kFailed;
    }
    const Clock& clock = (*line)->clock();

    // Spectrometer. With --sim its beam follows the config's field table.
    std::unique_ptr<spectrometer::Spectrometer> spec;
    if (lab_.spectrometer) {
      auto data = *lab_.spectrometer;
      if (g_.sim) {
        sim::BeamSettings beam;
        if (data.config.source.nominal_hv) beam.nominal_hv = *data.config.source.nominal_hv;
        if (auto it = data.tables.find(data.config.magnet.field_table); it != data.tables.end()) {
          auto table = spectrometer::to_field_table(it->second);
          beam.table_value = [table](double mass, const std::string& det) {
            auto v = table.value_for(mass, det);
            return v ? *v : mass / 8.0;
          };
        }
        sim::BeamModelRegistry::global().set("default", std::make_shared<sim::BeamModel>(clock, beam));
      }
      auto assembled = spectrometer::SpectrometerAssembler::assemble(
          std::move(data), spectrometer::SpectrometerContext{clock, (*line)->scheduler(), (*line)->bus()});
      if (!assembled) {
        io_.err << "error: " << assembled.error().what << '\n';
        return kFailed;
      }
      spec = std::move(*assembled);
    }

    // Services.
    auto host = scripting::make_script_host();
    systems::SwitchValveService script_valves((*line)->switches(), "script");
    std::optional<measurement::SpectrometerPort> port;
    if (spec) port.emplace(*spec);
    measurement::ExtractionLineValves valves(**line, "measurement");
    measurement::InstrumentMetrics instrument(spec.get(), line->get());
    persist::FilePersister files(lab_.data / "records");
    persist::Spool spool(lab_.data / "spool");
    persist::SavePipeline save(spool, files);
    persist::AliquotAllocator aliquots(files);

    executor::ExecutorContext ctx;
    auto& s = ctx.services;
    s.clock = &clock;
    s.bus = &(*line)->bus();
    s.scripts = host.get();
    s.resolver = &lab_.scripts->resolver();
    s.line.valves = &script_valves;
    s.spectrometer = port ? &*port : nullptr;
    s.valves = &valves;
    s.instrument_metrics = &instrument;
    if (spec) {
      s.spectrometer_info = [&spec] {
        const auto st = spec->snapshot();
        return run::SpectrometerInfo{st.hash_hex(), st.field_table,
                                     std::chrono::duration<double>(st.integration).count()};
      };
    }
    s.plans = lab_.plans.get();
    s.conditionals = lab_.conditionals.get();
    s.aliquots = &aliquots;
    s.persister = &files;
    s.save = &save;
    s.instrument.mass_spectrometer = queue_.mass_spectrometer;
    s.instrument.analyst = queue_.username;
    ctx.pre_run_metrics = &instrument;
    ctx.blank = default_blank_factory(lab_.ids);

    executor::ExecutorOptions options;
    options.state_file = lab_.data / "executor_state.json";
    std::size_t from = a_.from.value_or(0);
    if (a_.resume) {
      auto row = executor::Executor::resume_row(options.state_file);
      if (!row) {
        io_.err << "error: --resume: " << row.error().what << '\n';
        return kFailed;
      }
      from = *row;
    }

    // Progress.
    auto& bus = (*line)->bus();
    std::map<std::string, std::string> names;  // run id -> identifier
    std::mutex names_mutex;
    std::vector<SignalBus::Subscription> subs;
    subs.push_back(bus.subscribe<executor::RunStarted>([&](const executor::RunStarted& e) {
      {
        std::lock_guard lock(names_mutex);
        names[e.run_id] = e.identifier;
      }
      say("run " + std::to_string(e.row) + " " + e.identifier + ": started");
    }));
    subs.push_back(bus.subscribe<run::RunStateChanged>([&](const run::RunStateChanged& e) {
      std::string id;
      {
        std::lock_guard lock(names_mutex);
        id = names[e.run_id];
      }
      say("  " + id + ": " + std::string(run::to_string(e.to)) + (e.reason.empty() ? "" : " (" + e.reason + ")"));
    }));
    subs.push_back(bus.subscribe<executor::RunFinished>([&](const executor::RunFinished& e) {
      const auto& r = e.summary;
      std::string line_text = "run " + std::to_string(r.row) + " " + r.identifier + "-" + std::to_string(r.aliquot) +
                              r.step + ": " + std::string(run::to_string(r.state));
      if (r.truncated) line_text += " (truncated)";
      if (r.error) line_text += ": " + *r.error;
      say(line_text);
      for (const auto& c : r.queue_changes) say("  queue: " + c);
    }));
    subs.push_back(bus.subscribe<executor::ExecutorWaiting>([&](const executor::ExecutorWaiting& e) {
      if (e.duration > experiment::Duration::zero()) say("waiting " + clock_text(e.duration) + ": " + e.reason);
    }));
    subs.push_back(bus.subscribe<measurement::ConditionalTripped>([&](const measurement::ConditionalTripped& e) {
      say("  conditional " + e.trip.name + " tripped: " + e.trip.check);
    }));

    executor::Executor ex(ctx, options);
    executor::QueueResult result;
    std::atomic<bool> finished{false};
    interrupt_count() = 0;
    std::thread worker([&] {
      result = ex.execute(queue_object(), from);
      finished = true;
    });
    int handled = 0;
    while (!finished) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      const int n = interrupt_count();
      for (; handled < n; ++handled) {
        if (handled == 0) {
          say("interrupt: stopping after the current run (again to cancel it)");
          ex.stop();
        } else if (handled == 1) {
          say("interrupt: cancelling (again to abort)");
          ex.cancel();
        } else {
          say("interrupt: aborting");
          ex.abort();
        }
      }
    }
    worker.join();
    subs.clear();
    if (g_.sim) sim::BeamModelRegistry::global().clear();

    say("queue " + std::string(executor::to_string(result.end)) + (result.reason.empty() ? "" : ": " + result.reason));
    int ok_runs = 0;
    for (const auto& r : result.runs) ok_runs += r.state == run::RunState::Success ? 1 : 0;
    say(std::to_string(ok_runs) + "/" + std::to_string(result.runs.size()) + " run(s) succeeded; records in " +
        (lab_.data / "records").string());
    if (save.pending() > 0) say("warning: " + std::to_string(save.pending()) + " record(s) still in the spool");
    (*line)->stop();
    const bool good = result.end == executor::QueueEnd::Completed || result.end == executor::QueueEnd::Stopped;
    return good ? kOk : kFailed;
  }

  ExperimentQueue& queue_object() {
    if (!queue_model_) queue_model_.emplace(queue_);
    return *queue_model_;
  }

  ExpArgs a_;
  ExpGlobals g_;
  Io io_;
  Lab lab_;
  QueueSpec queue_;
  std::optional<ExperimentQueue> queue_model_;
  std::mutex out_mutex_;
};

}  // namespace

int exp_command(const std::vector<std::string>& args, const ExpGlobals& globals, Io io) {
  if (args.empty() || (args[0] != "run" && args[0] != "validate")) {
    io.err << kExpUsage;
    return kUsage;
  }
  ExpArgs a;
  a.verb = args[0];
  auto usage = [&](const std::string& m) {
    io.err << "elctl exp: " << m << '\n' << kExpUsage;
    return static_cast<int>(kUsage);
  };
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string& x = args[i];
    auto value = [&]() -> std::optional<std::string> {
      if (i + 1 >= args.size()) return std::nullopt;
      return args[++i];
    };
    if (x == "--lab" || x == "--data" || x == "--spectrometer" || x == "--canvas" || x == "--from" ||
        x == "--sim-speed") {
      auto v = value();
      if (!v) return usage(x + " needs a value");
      if (x == "--lab") a.lab = *v;
      else if (x == "--data") a.data = *v;
      else if (x == "--spectrometer") a.spectrometer = *v;
      else if (x == "--canvas") a.canvas = *v;
      else if (x == "--from") {
        try {
          a.from = static_cast<std::size_t>(std::stoul(*v));
        } catch (...) {
          return usage("--from needs a row number");
        }
      } else {
        try {
          a.sim_speed = std::stod(*v);
        } catch (...) {
          return usage("--sim-speed needs a number");
        }
        if (a.sim_speed <= 0) return usage("--sim-speed must be positive");
      }
    } else if (x == "--resume") {
      a.resume = true;
    } else if (x == "--dry-run") {
      a.dry_run = true;
    } else if (!x.starts_with("-") && a.queue_file.empty()) {
      a.queue_file = x;
    } else {
      return usage("unexpected '" + x + "'");
    }
  }
  if (a.queue_file.empty()) return usage("needs an experiment.toml");
  if (a.resume && a.from) return usage("--from and --resume are exclusive");
  if (a.sim_speed > 0 && !globals.sim) return usage("--sim-speed needs --sim");
  if (a.lab.empty()) a.lab = a.queue_file.has_parent_path() ? a.queue_file.parent_path() : fs::path(".");
  if (a.data.empty()) a.data = a.lab / "data";
  if (a.spectrometer.empty() && fs::exists(a.lab / "spectrometer.toml")) a.spectrometer = a.lab / "spectrometer.toml";
  return Exp(std::move(a), globals, io).run();
}

}  // namespace elctl
