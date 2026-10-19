#include "pychron/experiment/run/run.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <random>
#include <set>
#include <string_view>
#include <thread>

#include "pychron/devices/link.hpp"
#include "pychron/experiment/measurement/keep_trying.hpp"
#include "pychron/experiment/measurement/results.hpp"
#include "pychron/experiment/record/builder.hpp"

namespace pychron::experiment::run {

namespace {

const Clock& default_clock() {
  static SteadyClock clock;
  return clock;
}

std::string random_uuid() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<std::uint64_t> d;
  std::uint64_t hi = d(rng), lo = d(rng);
  hi = (hi & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;  // version 4
  lo = (lo & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;  // variant 1
  char buf[37];
  std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(hi >> 32),
                static_cast<unsigned>((hi >> 16) & 0xffff), static_cast<unsigned>(hi & 0xffff),
                static_cast<unsigned>(lo >> 48), static_cast<unsigned long long>(lo & 0xffffffffffffULL));
  return buf;
}

std::string utc(WallTime now) {
  const auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

double seconds(Duration d) { return d.count(); }

std::string param_text(const ParamValue& v) {
  return std::visit(
      [](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, bool>) return x ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::string>) return x;
        else {
          char b[40];
          std::snprintf(b, sizeof b, "%.15g", static_cast<double>(x));
          return b;
        }
      },
      v);
}

}  // namespace

// ---- RunControl ----------------------------------------------------------------

void RunControl::cancel() { token_.cancel(); }
void RunControl::abort() { token_.abort(); }

void RunControl::truncate(bool quick) {
  std::lock_guard lock(mutex_);
  if (engine_ != nullptr) engine_->truncate(quick);
  else pending_truncate_ = pending_truncate_.value_or(false) || quick;
}

void RunControl::set_counts(int counts) {
  std::lock_guard lock(mutex_);
  if (engine_ != nullptr) engine_->set_target(counts);
}

void RunControl::attach(measurement::MeasurementEngine* engine) {
  std::lock_guard lock(mutex_);
  engine_ = engine;
  if (engine_ != nullptr && pending_truncate_) {
    engine_->truncate(*pending_truncate_);
    pending_truncate_.reset();
  }
}

// ---- Run -------------------------------------------------------------------------

Run::Run(RunSpec spec, const QueueSpec& queue, RunServices services, RunHooks hooks, int run_index, std::size_t row)
    : spec_(std::move(spec)),
      queue_(queue),
      s_(std::move(services)),
      hooks_(std::move(hooks)),
      run_index_(run_index),
      row_(row),
      run_id_(s_.make_uuid ? s_.make_uuid() : random_uuid()),
      sm_(run_id_, s_.clock ? *s_.clock : default_clock(), s_.bus) {
  if (s_.clock == nullptr) s_.clock = &default_clock();
  result_.uuid = run_id_;
}

RunResult Run::execute(RunControl& control) {
  auto& token = control.token();
  auto fail_with = [&](RunEvent e, const Error& error) {
    result_.error = error;
    advance(e, error.what);
  };
  // Past the point of no return a failed run keeps what it measured (spec
  // 4.5): saved before it is Failed, so before the executor says it finished.
  // Then the line is made safe (spec 4.7), before the run is over.
  auto fail_run = [&](const Error& error) {
    if (result_.past_no_return) {
      if (auto sv = save(error); !sv) {
        result_.save_error = true;
        note("the failed run's record was not saved: " + sv.error().what);
      }
      (void)make_safe();
    }
    fail_with(RunEvent::Fail, error);
  };
  auto stopped = [&]() -> std::optional<RunEvent> {
    if (token.mode() == scripting::CancelMode::Abort) return RunEvent::Abort;
    if (token.mode() == scripting::CancelMode::Cancel) return RunEvent::Cancel;
    return std::nullopt;
  };

  advance(RunEvent::Start);
  if (auto r = prepare(); !r) {
    fail_with(RunEvent::Fail, r.error());
  } else if (auto early = stopped()) {
    advance(*early, "before extraction");
  } else {
    advance(RunEvent::Prepared);
    auto ex = extract(control);
    if (auto s = stopped(); s == RunEvent::Abort) {
      advance(RunEvent::Abort, "during extraction");
    } else if (s == RunEvent::Cancel) {
      (void)post_measure(control);  // cancel still runs post-measurement
      advance(RunEvent::Cancel, "during extraction");
    } else if (!ex) {
      fail_run(ex.error());
    } else {
      advance(RunEvent::Extracted);
      auto me = measure(control);
      const auto outcome = result_.measurement.outcome;
      if (stopped() == RunEvent::Abort || outcome == measurement::MeasurementOutcome::Aborted) {
        advance(RunEvent::Abort, "during measurement");
      } else if (stopped() == RunEvent::Cancel || outcome == measurement::MeasurementOutcome::Cancelled) {
        (void)post_measure(control);
        advance(RunEvent::Cancel, "during measurement");
      } else if (!me) {
        fail_run(me.error());
      } else if (outcome == measurement::MeasurementOutcome::Failed) {
        fail_run(result_.measurement.error.value_or(Error{ErrorKind::Io, "measurement failed", {}}));
      } else {
        if (outcome == measurement::MeasurementOutcome::Truncated) {
          result_.truncated = true;
          if (sm_.state() == RunState::Measuring) advance(RunEvent::Truncate);
        }
        advance(RunEvent::Measured, std::string(measurement::to_string(outcome)));
        const auto pm = post_measure(control);
        if (!pm) note("post-measurement failed: " + pm.error().what);  // still saves, then made safe
        if (stopped() == RunEvent::Abort) {
          advance(RunEvent::Abort, "during post-measurement");
        } else {
          advance(RunEvent::PostMeasured);
          auto sv = save();
          if (!pm) (void)make_safe();  // spec 4.4, PostMeasure row
          if (!sv) {
            result_.save_error = true;
            fail_with(RunEvent::Fail, sv.error());
          } else {
            advance(RunEvent::Saved);
          }
        }
      }
    }
  }
  result_.state = sm_.state();
  result_.history = sm_.history();
  return result_;
}

Result<scripting::Script> Run::resolve(const std::string& name, scripting::ScriptKind kind) const {
  if (s_.resolver == nullptr) return fail(ErrorKind::Config, "no script resolver for '" + name + "'");
  return s_.resolver->resolve(name, kind);
}

Result<void> Run::prepare() {
  timestamp_ = s_.timestamp ? s_.timestamp() : utc(s_.clock->wall_now());
  // This run's extraction device: the one set directly, else the lab's by
  // name. Its stage is told the queue's tray before any script runs, so a
  // hole name means a hole on that tray and no other.
  device_ = s_.line.device;
  if (device_ == nullptr && s_.devices) {
    const std::string& name = spec_.extraction.device.empty() ? queue_.extract_device : spec_.extraction.device;
    if (!name.empty()) device_ = s_.devices(name);
  }
  if (device_ != nullptr) {
    if (auto* stage = device_->stage()) {
      // With no tray the stage's is cleared: the device outlives the queue,
      // and the last queue's tray must not give this one's hole names a
      // meaning. A device that cannot clear its tray is left as it is.
      auto r = stage->set_tray(queue_.tray);
      if (!r && !queue_.tray.empty()) return fail(r.error());
    }
  }
  if (s_.aliquots != nullptr) {
    auto a = s_.aliquots->allocate(spec_.id);
    if (!a) return fail(a.error());
    result_.aliquot = a->aliquot;
    result_.step = a->step;
  } else {
    result_.aliquot = spec_.id.aliquot.value_or(1);
    result_.step = spec_.id.step;
  }

  if (!spec_.measurement.plan.empty()) {
    if (s_.plans == nullptr) return fail(ErrorKind::Config, "no plan library for '" + spec_.measurement.plan + "'");
    auto loaded = s_.plans->load(spec_.measurement.plan, spec_.measurement.overrides,
                                 plan::LoadOptions{spec_.measurement.advanced});
    if (!loaded) return fail(loaded.error());
    plan_ = std::move(*loaded);
    if (spec_.measurement.hook) plan_->plan.hook = spec_.measurement.hook;
    if (s_.conditionals != nullptr) {
      auto set = s_.conditionals->for_run(queue_, spec_, plan_->plan);
      if (!set) return fail(set.error());
      conditionals_ = std::move(*set);
    } else {
      auto set = plan_truncations(plan_->plan);
      if (!set) return fail(set.error());
      conditionals_ = std::move(*set);
    }
  }

  using K = scripting::ScriptKind;
  auto load = [&](const std::optional<std::string>& name, K kind, std::optional<scripting::Script>& out,
                  const char* ref) -> Result<void> {
    if (!name || name->empty()) return {};
    auto sc = resolve(*name, kind);
    if (!sc) return fail(sc.error());
    out = std::move(*sc);
    script_refs_[ref] = record::ScriptRef{out->name, "", ""};
    return {};
  };
  if (auto r = load(spec_.extraction.script.empty() ? std::nullopt : std::optional(spec_.extraction.script),
                    K::Extraction, extraction_, "extraction");
      !r)
    return r;
  if (auto r = load(spec_.post_equilibration, K::PostEquilibration, post_eq_, "post_eq"); !r) return r;
  if (auto r = load(spec_.post_measurement, K::PostMeasurement, post_meas_, "post_meas"); !r) return r;
  if ((extraction_ || post_eq_ || post_meas_) && (s_.scripts == nullptr || !s_.scripts->available()))
    return fail(ErrorKind::Config, "the run names scripts but no script host is available");

  if (s_.persister != nullptr) {
    RunIdentity id = spec_.id;
    id.aliquot = result_.aliquot;
    id.step = result_.step;
    if (auto r = s_.persister->begin_run(id, queue_); !r) return r;
  }
  return {};
}

scripting::ScriptContext Run::script_context() const {
  const auto& ex = spec_.extraction;
  scripting::ValueMap g;
  g["analysis_type"] = std::string(to_string(spec_.id.type));
  g["run_identifier"] = spec_.id.identifier + "-" + std::to_string(result_.aliquot) + result_.step;
  g["extract_value"] = ex.value;
  g["extract_units"] = std::string(to_string(ex.units));
  g["duration"] = seconds(ex.duration);
  g["cleanup"] = seconds(ex.cleanup);
  g["pre_cleanup"] = seconds(ex.pre_cleanup);
  g["post_cleanup"] = seconds(ex.post_cleanup);
  g["extract_device"] = ex.device.empty() ? queue_.extract_device : ex.device;
  g["tray"] = queue_.tray;
  g["pattern"] = ex.pattern.value_or(std::string{});  // empty: none, so a script can test it
  if (ex.position && !ex.position->holes.empty()) g["position"] = std::int64_t{ex.position->holes.front()};
  if (ex.beam_diameter) g["beam_diameter"] = *ex.beam_diameter;
  if (ex.ramp_rate) g["ramp_rate"] = *ex.ramp_rate;
  g["ramp_duration"] = seconds(ex.ramp);
  if (ex.cryo_temp) g["cryo_temperature"] = *ex.cryo_temp;
  return scripting::make_context(std::move(g));
}

Result<void> Run::run_script(const scripting::Script& script, scripting::ScriptKind kind, RunControl& control,
                             std::function<void()> on_pump_time_start) {
  scripting::ScriptEnvironment env;
  env.line = s_.line;
  env.line.device = device_;
  env.resources = s_.resources;
  env.resolver = s_.resolver;
  env.clock = s_.clock;
  env.context = script_context();
  env.on_pump_time_start = std::move(on_pump_time_start);
  env.log = [this](std::string_view line) { note(std::string(line)); };
  auto r = s_.scripts->run(script, env, control.token());
  // The record names a run's own scripts; the safe state comes after it is saved.
  if (r && kind != scripting::ScriptKind::SafeState) {
    const char* ref = kind == scripting::ScriptKind::Extraction         ? "extraction"
                      : kind == scripting::ScriptKind::PostEquilibration ? "post_eq"
                                                                         : "post_meas";
    script_refs_[ref].sha = r->sha;
  }
  if (!r) return fail(r.error());
  return {};
}

void Run::advance(RunEvent e, std::string reason) {
  // Never expected: said, so that a run whose states are out of step shows it.
  if (auto r = sm_.advance(e, std::move(reason)); !r) note(r.error().what);
}

void Run::note(std::string message) {
  RunNote said{run_id_, row_, std::move(message), s_.clock->now()};
  {
    std::lock_guard lock(messages_mutex_);
    result_.messages.push_back(said.text);
    message_times_.push_back(said.ts);
  }
  if (s_.bus != nullptr) s_.bus->publish(said);
}

// The line cryostat's temperatures as the extraction ended, for the record
// (owner decision 2026-10-05). A failed read leaves them out and says why.
void Run::record_cryo() {
  if (s_.line.cryo == nullptr) return;
  auto temps = s_.line.cryo->read_cryo_inputs();
  if (!temps) {
    if (!extraction::is_not_supported(temps.error())) note("cryo temperatures not recorded: " + temps.error().what);
    return;
  }
  actuals_.cryo_measured = std::move(*temps);
}

void Run::end_extraction() {
  if (device_ == nullptr) return;
  // A pattern never outlives the run that started it: the stage is stopped
  // before the laser goes off, and the next run finds the device idle.
  if (auto* patterns = device_->pattern_runner()) {
    if (auto r = patterns->stop_pattern(); !r) note("stop_pattern: " + r.error().what);
  }
  if (auto r = device_->end_extract(); !r) note("end_extract: " + r.error().what);
  if (auto r = device_->disable(); !r) note("disable: " + r.error().what);
}

SafeStateResult Run::make_safe() {
  SafeStateResult out;
  const auto step = [&](const std::string& name, const std::function<Result<void>()>& op) {
    if (auto r = keep_trying(name, op); !r) {
      out.failed_steps.push_back(name);
      note("safe state: " + name + " failed: " + r.error().what);
    }
  };
  if (device_ != nullptr) {
    step("end extraction", [&] { return device_->end_extract(); });
    step("disable " + device_->device_name(), [&] { return device_->disable(); });
  }
  if (plan_) {
    const auto& p = plan_->plan;
    if (s_.valves != nullptr && !p.equilibration.inlet.empty())
      step("close inlet " + p.equilibration.inlet, [&] { return s_.valves->close(p.equilibration.inlet); });
    if (s_.spectrometer != nullptr) {
      std::set<std::string> protected_detectors;
      for (const auto& gas : p.gases)
        for (const auto& seg : gas.segments)
          for (const auto& pos : seg.positions)
            for (const auto& e : pos.collect)
              if (e.protect) protected_detectors.insert(e.detector);
      for (const auto& d : protected_detectors)
        step("unprotect " + d, [&] { return s_.spectrometer->protect(d, false); });
    }
  }
  // Then the lab's own sequence, when it has one: the line only, no
  // acquisition (its kind has no such verbs). Not repeated: a script is not
  // an absolute target.
  if (s_.scripts != nullptr && s_.resolver != nullptr) {
    auto script = s_.resolver->resolve(scripting::to_string(scripting::ScriptKind::SafeState),
                                       scripting::ScriptKind::SafeState);
    const bool none = !script && script.error().code == scripting::kScriptNotFound;
    if (none) {
      // The built-in steps are all there is.
    } else if (!s_.scripts->available()) {
      note("safe state: the lab's safe_state script was not run: this build has no scripting");
    } else {
      RunControl control;
      Result<void> r = script ? run_script(*script, scripting::ScriptKind::SafeState, control)
                              : Result<void>(fail(script.error()));
      if (!r) {
        out.failed_steps.emplace_back("safe_state script");
        alarm(AlarmSeverity::Critical, "safe state: the safe_state script failed: " + r.error().what);
      }
    }
  }
  if (!out.failed_steps.empty()) {
    result_.unsafe = true;
    result_.unsafe_steps.insert(result_.unsafe_steps.end(), out.failed_steps.begin(), out.failed_steps.end());
  }
  return out;
}

Result<void> Run::keep_trying(const std::string& step, const std::function<Result<void>()>& op) {
  const measurement::KeepTrying how{*s_.clock, s_.bus, "safe state",
                                    [this](pychron::Duration d) {
                                      s_.clock->sleep_for(d);
                                      return true;
                                    },
                                    [this](const std::string& message) { note(message); },
                                    s_.critical_window, kSafeStateAlarmAfter};
  return measurement::keep_trying(how, step, op);
}

void Run::alarm(AlarmSeverity severity, std::string message) {
  note(message);
  if (s_.bus != nullptr) s_.bus->publish(Alarm{"safe state", severity, std::move(message), s_.clock->now()});
}

Result<void> Run::extract(RunControl& control) {
  actuals_.value = spec_.extraction.value;
  actuals_.cleanup = seconds(spec_.extraction.cleanup);
  if (!extraction_) return {};
  if (hooks_.acquire_extraction && !hooks_.acquire_extraction()) return {};  // cancelled while waiting
  // The point of no return (spec 4.4): from the script's first line the
  // sample is spent.
  result_.past_no_return = true;
  const auto start = s_.clock->now();
  auto r = run_script(*extraction_, scripting::ScriptKind::Extraction, control);
  end_extraction();  // always: the device never stays on after this phase
  record_cryo();
  if (hooks_.release_extraction) hooks_.release_extraction();
  actuals_.duration = std::chrono::duration<double>(s_.clock->now() - start).count();
  if (r && s_.persister != nullptr) {
    record::AnalysisRecord partial;
    partial.identity.uuid = run_id_;
    partial.identity.identifier = spec_.id.identifier;
    partial.identity.aliquot = result_.aliquot;
    partial.identity.step = result_.step;
    partial.identity.analysis_type = std::string(to_string(spec_.id.type));
    partial.identity.timestamp = timestamp_;
    partial.extraction.actuals = actuals_;
    if (auto se = s_.persister->save_extraction(partial); !se)
      note("save_extraction: " + se.error().what);
  }
  return r;
}

Result<void> Run::measure(RunControl& control) {
  if (!plan_) return {};  // e.g. a degas: extraction only
  if (s_.spectrometer == nullptr) return fail(ErrorKind::Config, "no spectrometer for the measurement");
  if (hooks_.acquire_spectrometer && !hooks_.acquire_spectrometer()) return {};  // cancelled while waiting

  measurement::EngineContext ctx{*s_.spectrometer,      *s_.clock, s_.valves, s_.peak_center, nullptr,
                                 s_.bus,                s_.instrument_metrics};
  ctx.timing = s_.detector_timing;
  ctx.reference = s_.reference_detector;
  measurement::MeasurementInputs in;
  in.plan = plan_->plan;
  in.conditionals = conditionals_;
  in.run_id = run_id_;
  in.analysis_type = std::string(to_string(spec_.id.type));
  in.icfactors = s_.icfactors;
  in.arar = s_.arar;

  // The post-equilibration script runs beside the measurement, on a thread
  // that says when it is done (under post_eq_mutex, through the clock).
  std::thread post_eq;
  std::mutex post_eq_mutex;
  std::condition_variable post_eq_cv;
  bool post_eq_done = false;
  measurement::EngineOptions options = s_.engine;
  options.on_inlet_closed = [&, this] {
    if (post_eq_ && !control.requested() && !post_eq.joinable()) {
      // Time does not move on between the thread's start and its Participant.
      auto hold = std::make_shared<Clock::Hold>(*s_.clock);
      post_eq = std::thread([&, this, hold]() mutable {
        Clock::Participant participant(*s_.clock, "run.post_eq");
        hold.reset();
        if (auto r = run_script(*post_eq_, scripting::ScriptKind::PostEquilibration, control); !r)
          note("post-equilibration failed: " + r.error().what);
        // Said while this thread is still a participant.
        std::lock_guard lock(post_eq_mutex);
        post_eq_done = true;
        s_.clock->notify_all(post_eq_cv);
      });
    }
    if (hooks_.on_overlap_ready) hooks_.on_overlap_ready();
  };
  // Told by the engine itself, not through the bus: the bus is for those who
  // watch the run, and the run is measuring whether or not anyone does.
  options.on_block_started = [this](measurement::Block b) {
    if (b == measurement::Block::Main && sm_.state() == RunState::Equilibrating) advance(RunEvent::Equilibrated);
  };
  measurement::MeasurementEngine engine(ctx, std::move(in), std::move(options));
  control.attach(&engine);
  result_.measurement = engine.run(control.token());
  // Each silence in the readings is said (the record has it as a gap event).
  if (s_.bus != nullptr)
    for (const auto& gap : result_.measurement.gaps)
      s_.bus->publish(RunNote{run_id_, row_, measurement::describe(gap), gap.at});
  if (result_.measurement.truncated_by_link) note("truncated: the spectrometer went silent and did not resume");
  control.attach(nullptr);
  if (post_eq.joinable()) {
    {
      // The script may still have to wait in the clock: joined once it has said it is done.
      std::unique_lock lock(post_eq_mutex);
      while (!post_eq_done) s_.clock->wait(post_eq_cv, lock);
    }
    post_eq.join();
  }
  // A measurement with no main block still moves on.
  if (sm_.state() == RunState::Equilibrating && !result_.measurement.data.series.empty() &&
      result_.measurement.outcome != measurement::MeasurementOutcome::Failed)
    advance(RunEvent::Equilibrated);
  if (hooks_.release_spectrometer) hooks_.release_spectrometer();
  return {};
}

Result<void> Run::post_measure(RunControl& control) {
  auto pump = [this] {
    if (hooks_.on_pump_time_started) hooks_.on_pump_time_started();
  };
  if (!post_meas_ || control.token().mode() == scripting::CancelMode::Abort) {
    pump();
    return {};
  }
  bool signalled = false;
  auto r = run_script(*post_meas_, scripting::ScriptKind::PostMeasurement, control, [&] {
    signalled = true;
    pump();
  });
  if (!signalled) pump();
  return r;
}

Result<void> Run::save(const std::optional<Error>& failure) {
  if (!plan_) return {};  // nothing measured, nothing to record
  record::RecordBuilder b;
  if (failure) b.set_status(record::RecordStatus::Failed, failure->what);
  else if (result_.truncated) b.set_status(record::RecordStatus::Truncated);
  record::Identity id;
  id.uuid = run_id_;
  id.identifier = spec_.id.identifier;
  id.aliquot = result_.aliquot;
  id.step = result_.step;
  id.analysis_type = std::string(to_string(spec_.id.type));
  id.timestamp = timestamp_;
  id.run_index = run_index_;
  id.queue_uuid = queue_.name;
  b.set_identity(id);
  const auto& sm = spec_.sample;
  b.set_sample(record::SampleMeta{sm.sample, sm.project, sm.material, sm.irradiation, sm.level,
                                  sm.irradiation_position ? std::to_string(*sm.irradiation_position) : "",
                                  "", spec_.comment});
  auto inst = s_.instrument;
  if (inst.mass_spectrometer.empty()) inst.mass_spectrometer = queue_.mass_spectrometer;
  if (inst.extract_device.empty())
    inst.extract_device = spec_.extraction.device.empty() ? queue_.extract_device : spec_.extraction.device;
  b.set_instrument(inst);

  record::Extraction ex;
  ex.spec.value = spec_.extraction.value;
  ex.spec.duration = seconds(spec_.extraction.duration);
  ex.spec.cleanup = seconds(spec_.extraction.cleanup);
  ex.spec.units = std::string(to_string(spec_.extraction.units));
  if (spec_.extraction.position) ex.spec.positions = spec_.extraction.position->holes;
  ex.spec.cryo_temperature = spec_.extraction.cryo_temp;
  ex.actuals = actuals_;
  ex.actuals.positions = ex.spec.positions;
  b.set_extraction(ex);

  record::Measurement m;
  m.plan.template_name = spec_.measurement.plan;
  m.plan.effective_plan_toml = plan_->effective_toml;
  for (const auto& [k, v] : spec_.measurement.overrides) m.plan.overrides[k] = param_text(v);
  if (plan_->plan.hook) m.hook = record::HookRef{*plan_->plan.hook, ""};
  m.scripts = script_refs_;
  if (!m.scripts.contains("extraction")) m.scripts["extraction"] = record::ScriptRef{"(none)", "", ""};
  b.set_measurement(m);

  record::SpectrometerRec spec;
  if (s_.spectrometer_info) {
    const auto info = s_.spectrometer_info();
    spec.state_hash = info.state_hash;
    spec.field_table_version = info.field_table;
    spec.integration_time = info.integration_s;
  }
  if (spec.integration_time <= 0 && !plan_->plan.gases.empty()) {
    if (const auto* main = plan::main_segment(plan_->plan.gases.front())) spec.integration_time = main->integration_s;
  }
  b.set_spectrometer(spec);

  const auto& meas = result_.measurement;
  const auto data = measurement::to_record_data(meas.data, plan_->plan);
  for (const auto& series : data.series) b.add_series(series);
  for (const auto& gas : data.gases) b.add_gas(gas);
  b.set_time_zero(data.time_zero);
  for (const auto& [k, n] : data.counts) b.set_count(k, n);
  auto fits = measurement::fit_results(meas.data, plan_->plan, s_.reference_detector);
  for (const auto& [iso, ir] : fits.results.intercepts) b.set_intercept(iso, ir);
  for (const auto& [det, br] : fits.results.baselines) b.set_baseline(det, br);
  for (const auto& pos : fits.results.baseline_positions) b.add_baseline_position(pos);
  for (const auto& [key, c] : fits.results.baseline_choices) b.set_baseline_choice(key, c);
  for (const auto& [det, f] : s_.icfactors) b.set_icfactor(det, f);
  for (const auto& e : fits.errors) note("fit: " + e);
  if (meas.whiff) b.set_whiff(std::string(to_string(*meas.whiff)));
  b.set_conditionals(measurement::to_record_conditionals(meas.installed, meas.data.trips, meas.conditional_errors));
  const auto history = sm_.history();
  const auto t0 = history.empty() ? TimePoint{} : history.front().ts;
  for (const auto& h : history)
    b.add_event(record::Event{std::chrono::duration<double>(h.ts - t0).count(), "state",
                              std::string(to_string(h.from)) + " -> " + std::string(to_string(h.to)) +
                                  (h.reason.empty() ? "" : ": " + h.reason)});
  for (const auto& gap : meas.gaps)
    b.add_event(record::Event{std::chrono::duration<double>(gap.at - t0).count(), "gap", measurement::describe(gap)});
  for (const auto& pc : meas.peak_centers) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s on %s (%s): ", pc.request.isotope.c_str(), pc.request.detector.c_str(),
                  pc.request.config.c_str());
    std::string detail = buf;
    if (pc.ok && pc.center) {
      std::snprintf(buf, sizeof buf, "center %.6f%s", *pc.center, pc.table_updated ? ", table updated" : "");
      detail += buf;
      if (pc.table_value) {
        std::snprintf(buf, sizeof buf, ", table value %.6f", *pc.table_value);
        detail += buf;
      }
    } else if (pc.skipped) {
      detail += "skipped: " + pc.message;
    } else {
      detail += "failed: " + pc.message;
    }
    b.add_event(record::Event{std::chrono::duration<double>(pc.finished - t0).count(), "peak_center", detail});
  }

  {
    // What the run said up to now; a save failure is said after the record.
    std::lock_guard lock(messages_mutex_);
    for (std::size_t i = 0; i < result_.messages.size(); ++i)
      b.add_event(record::Event{std::chrono::duration<double>(message_times_[i] - t0).count(), "note",
                                result_.messages[i]});
  }

  auto finalized = b.finalize();
  result_.record = finalized ? *finalized : b.draft();
  if (s_.save != nullptr) {
    // Spooled even when incomplete, so nothing measured is lost.
    auto saved = s_.save->save(*result_.record);
    if (!saved) return fail(saved.error());
    if (saved->pending) {
      // In the spool, and not where finished runs go: the run is what it
      // was, flagged, and says why. The queue goes on; the record is sent
      // again when a queue next starts.
      result_.save_error = true;
      note(std::string(kNotSavedNote) + saved->pending->what);
    }
  }
  if (!finalized) return fail(finalized.error());
  return {};
}

}  // namespace pychron::experiment::run
