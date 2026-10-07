#include "exp.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>

#include "pychron/core/config/loader.hpp"
#include "pychron/core/process.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/measurement/adapters.hpp"
#include "pychron/experiment/model/identifiers.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/experiment/model/queue_validation.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/assembler.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"

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
    "         [--from <row> | --resume] [--dry-run] [--sim-speed <x>|max]\n"
    "       --sim-speed (with --sim): simulated time, <x> times faster than real time; max does not wait at all\n"
    "       elctl exp notify [--lab <dir>]   send a test notification (<lab>/notifications.toml)\n";

std::string clock_text(experiment::Duration d) {
  const auto total = static_cast<long long>(std::llround(d.count()));
  std::ostringstream s;
  s << total / 3600 << ':' << std::setw(2) << std::setfill('0') << (total / 60) % 60 << ':' << std::setw(2)
    << std::setfill('0') << total % 60;
  return s.str();
}

struct ExpArgs {
  std::string verb;
  fs::path queue_file, lab, data, spectrometer, canvas;
  std::optional<std::size_t> from;
  bool resume = false, dry_run = false;
  double sim_speed = 0;  // 0: real time; infinity: --sim-speed max
};

class Exp {
 public:
  Exp(ExpArgs args, ExpGlobals globals, Io io) : a_(std::move(args)), g_(std::move(globals)), io_(io) {}

  int run() {
    lab_ = lab::load_lab({a_.lab, g_.config, a_.spectrometer});
    if (a_.verb == "notify") return notify();
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
    const auto check = lab::check_lab_queue(lab_, queue_);
    const auto& rep = check.report;
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
    for (const auto& d : check.all()) {
      const bool error = d.severity == Severity::Error;
      (error ? io_.err : io_.out) << (error ? "error: " : "warning: ") << lab::describe(d) << '\n';
    }
    if (check.ok()) io_.out << "ok: " << a_.queue_file.string() << '\n';
    return check.ok();
  }

  // Sends a test message on every configured channel and reports each.
  int notify() {
    for (const auto& p : lab_.problems)
      if (p.find("notifications.toml") != std::string::npos) {
        io_.err << "error: " << p << '\n';
        return kFailed;
      }
    if (lab_.notifications.empty()) {
      io_.err << "error: no notifications are configured (" << (a_.lab / "notifications.toml").string() << ")\n";
      return kFailed;
    }
    const auto sent = lab::deliver(lab_.notifications, lab::test_notification(a_.lab.string()), pychron::run_process);
    bool ok = true;
    for (const auto& d : sent) {
      ok = ok && d.ok;
      (d.ok ? io_.out : io_.err) << (d.ok ? "sent: " : "failed: ") << d.channel << (d.ok ? "" : ": " + d.error) << '\n';
    }
    return ok ? kOk : kFailed;
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
    // Clock: real time, or simulated time running sim_speed times faster
    // (VirtualClock), which starts at the real time of day. Declared before
    // everything that is given it, so it is the last to go.
    std::unique_ptr<VirtualClock> sim_clock;
    if (a_.sim_speed > 0) {
      VirtualClock::Options clock_options;
      clock_options.speed = a_.sim_speed;
      clock_options.epoch = std::chrono::system_clock::now();
      // Straight to stderr: a stalled run is one that prints nothing more.
      clock_options.on_stall = [](std::string report) { std::fprintf(stderr, "elctl: %s\n", report.c_str()); };
      sim_clock = std::make_unique<VirtualClock>(std::move(clock_options));
    }

    systems::ExtractionLine::Options line_options;
    line_options.clock = sim_clock.get();
    line_options.force_sim = g_.sim;
    std::optional<fs::path> canvas;
    if (!a_.canvas.empty()) canvas = a_.canvas;
    else if (fs::exists(g_.config.parent_path() / "canvas.toml")) canvas = g_.config.parent_path() / "canvas.toml";
    auto line = systems::ExtractionLine::load(g_.config, canvas, line_options);
    if (!line) {
      io_.err << "error: " << line.error().what << '\n';
      return kFailed;
    }
    const Clock& clock = (*line)->clock();
    // This thread takes part in the clock's time until the run is over:
    // simulated time moves only while it, too, is waiting in the clock.
    const Clock::Participant participant(clock, "elctl");
    if (auto r = (*line)->start(); !r) {
      io_.err << "error: " << r.error().what << '\n';
      return kFailed;
    }

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

    executor::ExecutorOptions options;
    std::size_t from = a_.from.value_or(0);
    if (a_.resume) {
      auto row = lab::LabSession::resume_row(a_.data);
      if (!row) {
        io_.err << "error: --resume: " << row.error().what << '\n';
        return kFailed;
      }
      from = *row;
    }
    lab::LabSession session(lab_, lab::SessionHardware{**line, spec.get(), nullptr, {}},
                            lab::SessionOptions{a_.data, options, {}});
    // What the lab asks that this session cannot do (a camera it may not
    // center holes with): a queue that uses that device will not start.
    for (const auto& p : session.problems()) say("warning: " + p);

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
    subs.push_back(bus.subscribe<run::RunNote>([&](const run::RunNote& e) {
      std::string id;
      {
        std::lock_guard lock(names_mutex);
        id = names[e.run_id];
      }
      say("  " + id + ": " + e.text);
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
    subs.push_back(bus.subscribe<lab::NotificationSent>([&](const lab::NotificationSent& e) {
      say(e.ok ? "notified " + e.channel + ": " + e.subject : "notification " + e.channel + " failed: " + e.error);
    }));

    interrupt_count() = 0;
    if (auto r = session.start(queue_, from); !r) {
      io_.err << "error: " << r.error().what << '\n';
      return kFailed;
    }
    int handled = 0;
    while (session.running()) {
      clock.sleep_for(std::chrono::milliseconds(50));
      const int n = interrupt_count();
      for (; handled < n; ++handled) {
        if (handled == 0) {
          say("interrupt: stopping after the current run (again to cancel it)");
          session.stop();
        } else if (handled == 1) {
          say("interrupt: cancelling (again to abort)");
          session.cancel();
        } else {
          say("interrupt: aborting");
          session.abort();
        }
      }
    }
    const executor::QueueResult result = *session.wait();
    {
      // The queue-end message: its commands are the outside world's, and take
      // real time.
      const Clock::Detached detached(clock);
      session.notifier().wait_idle();
    }
    subs.clear();
    if (g_.sim) sim::BeamModelRegistry::global().clear();

    say("queue " + std::string(executor::to_string(result.end)) + (result.reason.empty() ? "" : ": " + result.reason));
    int ok_runs = 0;
    for (const auto& r : result.runs) ok_runs += r.state == run::RunState::Success ? 1 : 0;
    say(std::to_string(ok_runs) + "/" + std::to_string(result.runs.size()) + " run(s) succeeded; records in " +
        (a_.data / "records").string());
    if (session.pending_saves() > 0) say("warning: " + std::to_string(session.pending_saves()) + " record(s) still in the spool");
    (*line)->stop();
    const bool good = result.end == executor::QueueEnd::Completed || result.end == executor::QueueEnd::Stopped;
    return good ? kOk : kFailed;
  }

  ExpArgs a_;
  ExpGlobals g_;
  Io io_;
  lab::Lab lab_;
  QueueSpec queue_;
  std::mutex out_mutex_;
};

}  // namespace

int exp_command(const std::vector<std::string>& args, const ExpGlobals& globals, Io io) {
  if (args.empty() || (args[0] != "run" && args[0] != "validate" && args[0] != "notify")) {
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
        if (*v == "max") {
          a.sim_speed = std::numeric_limits<double>::infinity();
          continue;
        }
        // std::stod reads "inf" and "nan", and stops at what it cannot read.
        std::size_t read = 0;
        try {
          a.sim_speed = std::stod(*v, &read);
        } catch (...) {
          return usage("--sim-speed needs a number");
        }
        if (read != v->size() || !std::isfinite(a.sim_speed)) return usage("--sim-speed needs a number");
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
  if (a.lab.empty() && !globals.lab.empty()) a.lab = globals.lab;
  if (a.data.empty() && !globals.data.empty()) a.data = globals.data;
  if (a.spectrometer.empty() && !globals.spectrometer.empty()) a.spectrometer = globals.spectrometer;
  if (a.canvas.empty() && !globals.canvas.empty()) a.canvas = globals.canvas;
  if (a.verb == "notify") {
    if (!a.queue_file.empty()) return usage("notify takes no experiment.toml");
    if (a.lab.empty()) a.lab = ".";
    return Exp(std::move(a), globals, io).run();
  }
  if (a.queue_file.empty()) return usage("needs an experiment.toml");
  // With an install, a queue named relative to its folder is found there.
  if (!globals.lab.empty() && a.queue_file.is_relative() && !fs::exists(a.queue_file) &&
      fs::exists(globals.lab / a.queue_file))
    a.queue_file = globals.lab / a.queue_file;
  if (a.resume && a.from) return usage("--from and --resume are exclusive");
  if (a.sim_speed > 0 && !globals.sim) return usage("--sim-speed needs --sim");
  if (a.lab.empty()) a.lab = a.queue_file.has_parent_path() ? a.queue_file.parent_path() : fs::path(".");
  if (a.data.empty()) a.data = a.lab / "data";
  if (a.spectrometer.empty() && fs::exists(a.lab / "spectrometer.toml")) a.spectrometer = a.lab / "spectrometer.toml";
  return Exp(std::move(a), globals, io).run();
}

}  // namespace elctl
