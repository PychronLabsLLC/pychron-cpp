#include "exp.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>

#include "exp_store.hpp"
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
#include "pychron/experiment/persist/persister.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/assembler.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"

namespace elctl {

namespace fs = std::filesystem;
using namespace pychron;
using namespace pychron::experiment;

std::atomic<int>& interrupt_count() {
  static std::atomic<int> n{0};
  return n;
}

void answer_interrupts(int& handled, int count, const std::function<void(const std::string&)>& say,
                       const QueueRequests& queue) {
  if (handled >= count) return;
  for (; handled < count; ++handled) {
    say(handled == 0   ? "interrupt: stopping after the current run (again to cancel it)"
        : handled == 1 ? "interrupt: cancelling (again to abort)"
                       : "interrupt: aborting");
  }
  if (count == 1) queue.stop();
  else if (count == 2) queue.cancel();
  else queue.abort();
}

namespace {

constexpr const char* kExpUsage =
    "usage: elctl [-c <extraction_line.toml>] [--sim] exp <validate|run> <experiment.toml>\n"
    "         [--lab <dir>] [--data <dir>] [--spectrometer <file>] [--canvas <file>]\n"
    "         [--from <row> | --resume] [--dry-run] [--sim-speed <x>|max] [--db <url>] [--sim-save]\n"
    "       --db: write the runs to this database (an install's own is the default); without one, to files under --data\n"
    "       --sim-save: let a simulated session write to the database. Without it simulated runs go to a database\n"
    "                   only when it is the install's own file on this computer; otherwise to files under --data\n"
    "       --sim-speed (with --sim): simulated time, <x> times faster than real time; max does not wait at all\n"
    "       elctl exp notify [--lab <dir>]   send a test notification (<lab>/notifications.toml)\n";

// A text of several lines (a database's refusal, with its statement) as one
// line of at most `limit` characters.
std::string one_line(const std::string& text, std::size_t limit = 300) {
  std::string out;
  bool space = false;
  for (const char c : text) {
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      space = !out.empty();
      continue;
    }
    if (space) out += ' ';
    space = false;
    out += c;
  }
  if (out.size() > limit) {
    out.resize(limit);
    out += "...";
  }
  return out;
}

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
  std::string db;  // --db, else the install's; empty: records are files
  bool db_from_option = false;  // given with --db, not the install's own
  bool sim_save = false;        // --sim-save: a simulated session may write to the database
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
    if (a_.verb == "run" && a_.db.empty() && !g_.database_error.empty()) {
      io_.err << "error: the installation's database: " << g_.database_error << '\n';
      return kFailed;
    }
    if (a_.verb == "run" && a_.db.empty() && g_.database_not_built) {
      io_.err << "warning: built without the DVC store: runs are written to " << (a_.data / "records").string()
              << ", not to the installation's database\n";
    }
    if (a_.verb == "run") {
      // Whose runs are a simulator's: one rule for every program.
      const std::optional<bool> sim_spectrometer =
          lab_.spectrometer ? std::optional<bool>(spectrometer::is_simulated(*lab_.spectrometer)) : std::nullopt;
      simulated_ = lab_.line ? spectrometer::simulated_session(g_.sim, sim_spectrometer, *lab_.line) : g_.sim;
      if (a_.sim_save && a_.db.empty()) {
        io_.err << "error: --sim-save: there is no database to save to (--db, or an install that has one)\n";
        return kFailed;
      }
      // A simulator's runs do not go among a lab's measurements unless that
      // was asked for: they are record files, as before there was a
      // database, and this is said once. Not an error.
      if (simulated_ && !a_.db.empty() && !saves_simulated_runs(a_.db, a_.db_from_option, a_.sim_save)) {
        io_.err << "note: simulated runs are written to " << (a_.data / "records").string() << ", not to "
                << show_database(a_.db) << "; pass --sim-save to save them there\n";
        a_.db.clear();
      }
    }
    // Opened here, on the main thread, before anything is printed or started.
    // Declared (as a member) before the session, which is destroyed first.
    if (a_.verb == "run" && !a_.db.empty()) {
      auto opened = open_exp_store(a_.db, a_.data);
      if (!opened) {
        io_.err << "error: " << opened.error().what << '\n';
        return kFailed;
      }
      store_ = std::move(*opened);
    }
    const bool ok = report();
    if (a_.verb == "validate") return ok ? kOk : kFailed;
    // The row the queue starts at: --from, or with --resume the one after the last that ran.
    if (a_.from) from_ = *a_.from;
    if (a_.resume) {
      auto row = lab::LabSession::resume_row(a_.data);
      if (!row) {
        io_.err << "error: --resume: " << row.error().what << '\n';
        return kFailed;
      }
      from_ = *row;
    }
    if (a_.dry_run) return ok && store_accepts() ? kOk : kFailed;
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

  // What the database would refuse of the queue as it will run (from row
  // from_), as error lines. True when it would take it (or there is no database).
  bool store_accepts() {
    if (!store_) return true;
    const auto problems = store_->persister().check_queue(persist::rows_from(queue_, from_));
    if (!problems) {
      complain_lines("the queue cannot be saved: " + problems.error().what);
      return false;
    }
    if (problems->empty()) return true;
    complain("error: the queue cannot be saved:");
    for (const auto& line : *problems) complain("error: " + line);
    return false;
  }

  // An error whose text has several lines, each as its own error line.
  void complain_lines(const std::string& text) {
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) complain("error: " + line);
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

  // An error line. Under the lock say() takes: once the run has threads, a
  // stall report can come from the clock's.
  void complain(const std::string& line) {
    std::lock_guard lock(out_mutex_);
    io_.err << line << '\n';
    io_.err.flush();
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
      clock_options.on_stall = [this](std::string report) { complain("elctl: " + report); };
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
      complain("error: " + line.error().what);
      return kFailed;
    }
    const Clock& clock = (*line)->clock();
    // A spectrometer is simulated when --sim says so or when its own file
    // is (every transport and driver a simulator's), as the line is when its
    // transports are. Its beam then follows the config's field table and,
    // where the line is simulated too, measures what the line's source volume
    // holds. A simulated spectrometer on a real line has no simulated gas to
    // read and keeps the beam's fixed argon; a real spectrometer has no beam.
    const bool sim_spectrometer = lab_.spectrometer && (g_.sim || spectrometer::is_simulated(*lab_.spectrometer));
    // The simulated beam (below) refers to the clock: it goes on every way
    // out, after the spectrometer and before the line and the clock. Only
    // once this run has put a beam in the registry (`registered`, set where
    // it does): a run that fails before then leaves the registry as it found
    // it. The registry is cleared whole, since the drivers of this
    // spectrometer may have made beams of other names on the same clock.
    struct BeamGuard {
      bool registered = false;
      ~BeamGuard() {
        if (registered) sim::BeamModelRegistry::global().clear();
      }
    } beam_guard;
    // This thread takes part in the clock's time until the run is over:
    // simulated time moves only while it, too, is waiting in the clock.
    const Clock::Participant participant(clock, "elctl");
    if (auto r = (*line)->start(); !r) {
      complain("error: " + r.error().what);
      return kFailed;
    }

    // Spectrometer.
    std::unique_ptr<spectrometer::Spectrometer> spec;
    if (lab_.spectrometer) {
      auto data = *lab_.spectrometer;
      std::shared_ptr<sim::BeamModel> sim_beam;
      if (sim_spectrometer) {
        sim::BeamSettings beam;
        if (data.config.source.nominal_hv) beam.nominal_hv = *data.config.source.nominal_hv;
        if (auto it = data.tables.find(data.config.magnet.field_table); it != data.tables.end()) {
          auto table = spectrometer::to_field_table(it->second);
          beam.table_value = [table](double mass, const std::string& det) {
            auto v = table.value_for(mass, det);
            return v ? *v : mass / 8.0;
          };
        }
        sim_beam = std::make_shared<sim::BeamModel>(clock, beam);
        beam_guard.registered = true;  // before the registry and the drivers have one
        sim::BeamModelRegistry::global().set("default", sim_beam);
      }
      auto assembled = spectrometer::SpectrometerAssembler::assemble(
          std::move(data), spectrometer::SpectrometerContext{clock, (*line)->scheduler(), (*line)->bus()});
      if (!assembled) {
        complain("error: " + assembled.error().what);
        return kFailed;
      }
      spec = std::move(*assembled);
      // Now that the beam has its detectors: the line's gas, and sim.toml's
      // baselines.
      if (sim_beam && (*line)->sim() != nullptr) {
        auto fed = spectrometer::feed_beam_from_line(*sim_beam, *(*line)->sim());
        if (!fed) {
          complain("error: " + fed.error().what);
          return kFailed;
        }
        if (*fed == spectrometer::BeamFeed::FixedGas) {
          say(std::string("note: the simulated spectrometer is not joined to the line: ") +
              ((*line)->canvas() != nullptr ? "the canvas has no spectrometer stage"
                                            : "the line was loaded without its canvas"));
        }
      }
    }

    executor::ExecutorOptions options;
    const std::size_t from = from_;
    lab::LabSession session(lab_, lab::SessionHardware{**line, spec.get(), nullptr, {}, nullptr, simulated_},
                            lab::SessionOptions{a_.data, options, {}, store_ ? &store_->persister() : nullptr});
    // What the lab asks that this session cannot do (a camera it may not
    // center holes with): a queue that uses that device will not start.
    for (const auto& p : session.problems()) say("warning: " + p);

    // Progress.
    auto& bus = (*line)->bus();
    // Set when the queue has ended, to wake the wait below at once.
    std::mutex ended_mutex;
    std::condition_variable ended_cv;
    bool ended = false;
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
      if (r.save_error && r.state == run::RunState::Success) line_text += " (not saved)";  // the run said why
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

    subs.push_back(bus.subscribe<lab::QueueEnded>([&](const lab::QueueEnded&) {
      {
        std::lock_guard lock(ended_mutex);
        ended = true;
      }
      ended_cv.notify_all();
    }));

    interrupt_count() = 0;
    if (auto r = session.start(queue_, from); !r) {
      complain_lines(r.error().what);
      return kFailed;
    }
    // The end of the queue and the operator's interrupt are the outside
    // world's: waited for in real time, off the clock, so that an interrupt is
    // heard whatever simulated time is doing (paced slowly, or stalled).
    {
      const Clock::Detached detached(clock);
      int handled = 0;
      while (session.running()) {
        {
          std::unique_lock lock(ended_mutex);
          ended_cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return ended; });
        }
        const int n = interrupt_count();
        answer_interrupts(handled, n, [&](const std::string& text) { say(text); },
                          {[&] { session.stop(); }, [&] { session.cancel(); }, [&] { session.abort(); }});
      }
    }
    const executor::QueueResult result = *session.wait();
    // Nothing is left to poll for, and the queue-end message may take a while.
    (*line)->stop();
    {
      // Its commands are the outside world's, and take real time.
      const Clock::Detached detached(clock);
      session.notifier().wait_idle();
    }
    subs.clear();

    say("queue " + std::string(executor::to_string(result.end)) + (result.reason.empty() ? "" : ": " + result.reason));
    int ok_runs = 0, saved_runs = 0;
    for (const auto& r : result.runs) {
      ok_runs += r.state == run::RunState::Success ? 1 : 0;
      saved_runs += r.state == run::RunState::Success && !r.save_error ? 1 : 0;
    }
    // Where the runs went; and, when some got no further than the spool, how many did.
    const std::string where = store_ ? "in " + show_database(a_.db) : "records in " + (a_.data / "records").string();
    say(std::to_string(ok_runs) + "/" + std::to_string(result.runs.size()) + " run(s) succeeded; " +
        (saved_runs == ok_runs ? "" : std::to_string(saved_runs) + " of them ") + where);
    if (session.pending_saves() > 0) {
      // The reason is the last refusal's; it is the persister's text, which never holds the password.
      const auto why = session.last_save_error();
      say("warning: " + std::to_string(session.pending_saves()) + " record(s) " +
          (store_ ? "waiting in " + (a_.data / "spool").string() : std::string("still in the spool")) +
          (why ? ": " + one_line(*why) : std::string()));
    }
    const bool good = result.end == executor::QueueEnd::Completed || result.end == executor::QueueEnd::Stopped;
    return good ? kOk : kFailed;
  }

  ExpArgs a_;
  ExpGlobals g_;
  Io io_;
  lab::Lab lab_;
  QueueSpec queue_;
  std::size_t from_ = 0;  // the row the queue starts at
  bool simulated_ = false;  // the session's hardware is a simulator's: its records say so
  std::mutex out_mutex_;
  // After everything the session's threads may call it from is gone: members
  // outlive execute()'s locals, the session among them.
  std::unique_ptr<ExpStore> store_;
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
    if (x == "--lab" || x == "--db" || x == "--data" || x == "--spectrometer" || x == "--canvas" || x == "--from" ||
        x == "--sim-speed") {
      auto v = value();
      if (!v) return usage(x + " needs a value");
      if (x == "--lab") a.lab = *v;
      else if (x == "--db") {
        a.db = *v;
        a.db_from_option = true;
      }
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
    } else if (x == "--sim-save") {
      a.sim_save = true;
    } else if (x == "--resume") {
      a.resume = true;
    } else if (x == "--dry-run") {
      a.dry_run = true;
    } else if (!x.starts_with('-') && a.queue_file.empty()) {
      a.queue_file = x;
    } else {
      return usage("unexpected '" + x + "'");
    }
  }
  if (a.lab.empty() && !globals.lab.empty()) a.lab = globals.lab;
  if (a.data.empty() && !globals.data.empty()) a.data = globals.data;
  if (a.spectrometer.empty() && !globals.spectrometer.empty()) a.spectrometer = globals.spectrometer;
  if (a.canvas.empty() && !globals.canvas.empty()) a.canvas = globals.canvas;
  if (a.db.empty()) a.db = globals.database;
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
