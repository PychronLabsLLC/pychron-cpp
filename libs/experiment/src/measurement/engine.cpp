#include "pychron/experiment/measurement/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "pychron/core/events.hpp"

namespace pychron::experiment::measurement {

namespace {

pychron::Duration seconds_to(double s) {
  return std::chrono::duration_cast<pychron::Duration>(std::chrono::duration<double>(std::max(s, 0.0)));
}

std::vector<collect::Channel> channels_of(const plan::MeasurementPlan& plan, const plan::Hop& hop) {
  std::vector<collect::Channel> out;
  for (const auto& a : plan::active_detectors(plan, hop)) out.push_back({a.isotope, a.detector});
  return out;
}

}  // namespace

std::string_view to_string(Block block) noexcept {
  switch (block) {
    case Block::PeakCenterBefore: return "peak_center.before";
    case Block::BaselineBefore: return "baseline.before";
    case Block::PositionFirstHop: return "position";
    case Block::Equilibrate: return "equilibrate";
    case Block::Main: return "main";
    case Block::BaselineAfter: return "baseline.after";
    case Block::PeakCenterAfter: return "peak_center.after";
  }
  return "main";
}

std::string_view to_string(MeasurementOutcome outcome) noexcept {
  switch (outcome) {
    case MeasurementOutcome::Completed: return "completed";
    case MeasurementOutcome::Truncated: return "truncated";
    case MeasurementOutcome::Terminated: return "terminated";
    case MeasurementOutcome::Cancelled: return "cancelled";
    case MeasurementOutcome::Aborted: return "aborted";
    case MeasurementOutcome::Failed: return "failed";
  }
  return "failed";
}

Result<ConditionalSet> plan_conditionals(const plan::MeasurementPlan& plan) {
  ConditionalSet set;
  for (std::size_t i = 0; i < plan.conditionals.truncations.size(); ++i) {
    const auto& t = plan.conditionals.truncations[i];
    auto expr = parse_expression(t.check);
    if (!expr) return fail(ErrorKind::Config, "conditionals.truncations[" + std::to_string(i) + "]: " + expr.error().what);
    Conditional c;
    c.name = "plan.truncation[" + std::to_string(i) + "]";
    c.kind = ConditionalKind::Truncation;
    c.check = t.check;
    c.expr = std::shared_ptr<const Expr>(std::move(*expr));
    c.start = t.start;
    c.action.type = ActionSpec::Type::Truncate;
    set.items.push_back(std::move(c));
  }
  return set;
}

// ---- hook API ----------------------------------------------------------------

class MeasurementEngine::Api final : public scripting::IMeasurementApi {
 public:
  explicit Api(MeasurementEngine& e) : e_(e) {}

  Result<void> position(std::string_view isotope, std::string_view detector) override {
    return e_.move_to(plan::HopTarget{std::string(isotope), std::nullopt, std::string(detector)}, 0, {});
  }

  Result<void> acquire(int counts, double integration_time_s) override {
    if (e_.collecting_) return fail(ErrorKind::Config, "acquire() is not allowed inside a collection");
    if (counts <= 0 || integration_time_s <= 0) return fail(ErrorKind::Config, "acquire() needs counts > 0 and integration > 0");
    auto channels = e_.last_channels_.empty() ? channels_of(e_.in_.plan, e_.in_.plan.main.hops.front())
                                              : e_.last_channels_;
    auto r = e_.collect(Block::Main, {collect::SeriesKind::Signal, counts, std::move(channels), "hook"},
                        integration_time_s, {});
    if (!r) return fail(r.error());
    return {};
  }

  Result<void> open(std::string_view valve) override {
    if (e_.ctx_.valves == nullptr) return fail(ErrorKind::Config, "no valve service");
    return e_.ctx_.valves->open(std::string(valve));
  }

  Result<void> close(std::string_view valve) override {
    if (e_.ctx_.valves == nullptr) return fail(ErrorKind::Config, "no valve service");
    return e_.ctx_.valves->close(std::string(valve));
  }

  // "<check> -> <action>", e.g. "Ar40 > 100 -> truncate"; no action means truncate.
  Result<void> add_conditional(std::string_view spec) override {
    const auto arrow = spec.find("->");
    std::string check(spec.substr(0, arrow));
    std::string action = arrow == std::string_view::npos ? "truncate" : std::string(spec.substr(arrow + 2));
    auto expr = compile_check(check, std::nullopt, "");
    if (!expr) return fail(expr.error());
    auto a = parse_action(action);
    if (!a) return fail(a.error());
    if (is_queue_action(a->type)) return fail(ErrorKind::Config, "queue actions are not allowed in add_conditional");
    Conditional c;
    c.name = "hook:" + std::string(spec);
    c.kind = ConditionalKind::Action;
    c.check = std::move(check);
    c.expr = *expr;
    c.action = *a;
    c.level = ConditionalLevel::Hook;
    e_.in_.conditionals.items.push_back(std::move(c));
    e_.rebuild_conditionals();
    return {};
  }

  Result<void> truncate(bool quick) override {
    e_.request_truncate(quick ? 0.25 : 1.0);
    return {};
  }

  void log(std::string_view message) override {
    e_.result_.notes.emplace_back(message);
    e_.publish_log(LogLevel::Info, std::string(message));
  }

 private:
  MeasurementEngine& e_;
};

// ---- engine ------------------------------------------------------------------

MeasurementEngine::MeasurementEngine(EngineContext context, MeasurementInputs inputs, EngineOptions options)
    : ctx_(context), in_(std::move(inputs)), options_(std::move(options)), collector_(ctx_.clock, ctx_.bus) {
  rebuild_conditionals();
}

MeasurementEngine::~MeasurementEngine() = default;

Result<void> MeasurementEngine::validate() const {
  const auto& p = in_.plan;
  std::vector<std::string> problems;
  if (p.main.hops.empty()) problems.emplace_back("plan has no hops");
  for (std::size_t i = 0; i < p.main.hops.size(); ++i) {
    if (!plan::hop_target(p, p.main.hops[i]))
      problems.push_back("main.hops[" + std::to_string(i) + "]: no magnet position (reference detector not in hop)");
  }
  if ((!p.equilibration.inlet.empty() || !p.equilibration.outlet.empty()) && ctx_.valves == nullptr)
    problems.emplace_back("plan equilibrates through valves but no valve service is configured");
  if ((p.peak_center.before || p.peak_center.after) && ctx_.peak_center == nullptr)
    problems.emplace_back("plan enables peak centering but no peak-center service is configured");
  if (p.hook && ctx_.hook == nullptr) problems.push_back("plan names hook '" + *p.hook + "' but no hook runner is configured");
  if (problems.empty()) return {};
  std::string what;
  for (const auto& s : problems) what += (what.empty() ? "" : "\n") + s;
  return fail(ErrorKind::Config, what);
}

void MeasurementEngine::rebuild_conditionals() {
  conditionals_ = std::make_unique<ConditionalEngine>(in_.conditionals, in_.analysis_type);
}

MeasurementResult MeasurementEngine::run(scripting::CancelToken& token) {
  token_ = &token;
  result_ = MeasurementResult{};
  position_.reset();
  protected_.clear();
  last_channels_.clear();
  inlet_open_ = false;
  close_inlet_now_ = false;
  main_count_ = 0;
  stop_ = Stop::None;
  main_truncated_ = false;
  break_main_ = false;
  {
    std::lock_guard lock(truncate_mutex_);
    truncate_request_.reset();
  }
  collector_.start(ctx_.clock.now());
  collector_.set_fallback(ctx_.metrics);
  collector_.set_fits(in_.plan.fits);
  collector_.set_icfactors(in_.icfactors);
  collector_.set_arar(in_.arar);
  rebuild_conditionals();
  for (const auto* c : conditionals_->installed()) result_.installed.push_back(*c);

  Result<void> r = validate();
  if (r) r = run_blocks();

  const bool abnormal = !r || token.requested() || stop_ == Stop::Cancel || stop_ == Stop::Abort;
  cleanup(abnormal);

  result_.data = collector_.data();
  result_.conditional_errors = conditionals_->errors();
  if (token.mode() == scripting::CancelMode::Abort || stop_ == Stop::Abort) {
    result_.outcome = MeasurementOutcome::Aborted;
  } else if (token.mode() == scripting::CancelMode::Cancel || stop_ == Stop::Cancel) {
    result_.outcome = MeasurementOutcome::Cancelled;
  } else if (!r) {
    result_.outcome = MeasurementOutcome::Failed;
    result_.error = r.error();
  } else if (stop_ == Stop::Terminate) {
    result_.outcome = MeasurementOutcome::Terminated;
  } else if (main_truncated_) {
    result_.outcome = MeasurementOutcome::Truncated;
  } else {
    result_.outcome = MeasurementOutcome::Completed;
  }
  token_ = nullptr;
  return std::move(result_);
}

Result<void> MeasurementEngine::run_blocks() {
  const auto& p = in_.plan;
  auto step = [&](Block b, auto&& fn) -> Result<void> {
    if (stopping()) return {};
    if (ctx_.bus != nullptr) ctx_.bus->publish(BlockStarted{in_.run_id, b});
    result_.blocks.push_back(b);
    Result<void> r = fn();
    if (ctx_.bus != nullptr) ctx_.bus->publish(BlockFinished{in_.run_id, b, r.has_value()});
    return r;
  };

  if (p.peak_center.before) {
    if (auto r = step(Block::PeakCenterBefore, [&] { return peak_center(Block::PeakCenterBefore); }); !r) return r;
  }
  if (p.baseline.before) {
    if (auto r = step(Block::BaselineBefore, [&] { return baseline(Block::BaselineBefore); }); !r) return r;
  }
  if (auto r = step(Block::PositionFirstHop, [&] {
        const auto& hop = p.main.hops.front();
        return move_to(plan::hop_target(p, hop), hop.settle_s, hop.protect);
      });
      !r)
    return r;
  if (auto r = step(Block::Equilibrate, [&] { return equilibrate(); }); !r) return r;
  if (!stopping()) {
    if (auto r = call_hook("before_main"); !r) return r;
  }
  if (auto r = step(Block::Main, [&] { return main(); }); !r) return r;
  if (!stopping()) {
    if (auto r = call_hook("after_main"); !r) return r;
  }
  if (p.baseline.after) {
    if (auto r = step(Block::BaselineAfter, [&] { return baseline(Block::BaselineAfter); }); !r) return r;
  }
  if (p.peak_center.after) {
    if (auto r = step(Block::PeakCenterAfter, [&] { return peak_center(Block::PeakCenterAfter); }); !r) return r;
  }
  return {};
}

Result<void> MeasurementEngine::peak_center(Block block) {
  const auto& p = in_.plan;
  PeakCenterRequest req{p.peak_center.isotope,
                        p.peak_center.detector.empty() ? p.detectors.reference : p.peak_center.detector,
                        p.peak_center.config};
  auto rep = ctx_.peak_center->peak_center(req, *token_);
  position_.reset();  // the job leaves the magnet wherever it ended
  if (!rep) {
    if (token_->requested()) return {};
    return fail(rep.error());
  }
  if (!rep->ok)
    publish_log(LogLevel::Warn, std::string(to_string(block)) + " failed: " + rep->message + "; continuing");
  result_.peak_centers.push_back(*rep);
  return {};
}

std::vector<collect::Channel> MeasurementEngine::baseline_channels() const {
  std::vector<collect::Channel> out;
  for (const auto& hop : in_.plan.main.hops) {
    for (const auto& a : plan::active_detectors(in_.plan, hop)) {
      auto same = [&](const collect::Channel& c) { return c.detector == a.detector; };
      if (std::none_of(out.begin(), out.end(), same)) out.push_back({"", a.detector});
    }
  }
  return out;
}

Result<void> MeasurementEngine::baseline(Block block) {
  const auto& b = in_.plan.baseline;
  if (auto r = move_to(plan::baseline_target(in_.plan), b.settle_s, {}); !r) return r;
  const int counts = block == Block::BaselineAfter ? scaled(b.counts) : b.counts;
  auto r = collect(block, {collect::SeriesKind::Baseline, counts, baseline_channels(), std::string(to_string(block))},
                   b.integration_s, {});
  if (!r) return fail(r.error());
  return {};
}

Result<void> MeasurementEngine::equilibrate() {
  const auto& p = in_.plan;
  const auto& eq = p.equilibration;
  const auto now_s = [&] { return collector_.seconds(ctx_.clock.now()); };

  if (!eq.outlet.empty()) {
    if (auto r = ctx_.valves->close(eq.outlet); !r) return r;
  }

  bool sniffing = false;
  int sniffed = 0;
  const auto sniff_integration = seconds_to(p.sniff.integration_s);
  const auto timeout = std::chrono::duration_cast<pychron::Duration>(sniff_integration * options_.timeout_factor) +
                       options_.timeout_slack;
  auto stop_sniff = [&] {
    if (!sniffing) return;
    sniffing = false;
    collecting_ = false;
    collector_.finish();
    ctx_.spectrometer.stop_acquisition();
    acquiring_ = false;
  };
  auto start_sniff = [&]() -> Result<void> {
    if (!p.sniff.enabled || p.sniff.counts <= 0) return {};
    auto channels = channels_of(p, p.main.hops.front());
    last_channels_ = channels;
    if (auto r = ctx_.spectrometer.start_acquisition(sniff_integration); !r) return r;
    acquiring_ = true;
    collector_.begin({collect::SeriesKind::Sniff, p.sniff.counts, std::move(channels), "sniff"},
                     [this](int, double) { return sniff_reading(); });
    collecting_ = true;
    sniffing = true;
    return {};
  };
  // With a whiff, sniffing starts once the whiff has decided.
  if (!p.whiff.enabled) {
    if (auto r = start_sniff(); !r) return r;
  }

  const double open_at = now_s() + eq.inlet_delay_s;
  std::optional<double> close_at;
  Result<void> out;
  while (!stopping()) {
    const double now = now_s();
    if (!close_at && now >= open_at) {
      if (!eq.inlet.empty()) {
        if (auto r = ctx_.valves->open(eq.inlet); !r) {
          out = r;
          break;
        }
        inlet_open_ = true;
      }
      collector_.set_inlet_open(now);
      close_at = now + eq.time_s;
      if (p.main.time_zero.kind == plan::TimeZeroKind::Offset) collector_.set_time_zero(now + p.main.time_zero.offset_s);
      if (p.whiff.enabled) {
        if (auto r = whiff(); !r) {
          out = r;
          break;
        }
        if (stopping()) break;
        if (auto r = start_sniff(); !r) {
          out = r;
          break;
        }
        continue;  // the whiff took time: re-read the clock
      }
    }
    if (close_at && (now >= *close_at || close_inlet_now_)) {
      if (eq.close_inlet) {
        if (!eq.inlet.empty()) {
          if (auto r = ctx_.valves->close(eq.inlet); !r) {
            out = r;
            break;
          }
          inlet_open_ = false;
        }
        collector_.set_inlet_close(now);
        if (ctx_.bus != nullptr) ctx_.bus->publish(OverlapReady{in_.run_id});
        if (options_.on_inlet_closed) options_.on_inlet_closed();
      }
      if (p.main.time_zero.kind == plan::TimeZeroKind::OnInletClose) collector_.set_time_zero(now);
      break;
    }
    if (sniffing) {
      auto r = ctx_.spectrometer.next_reading(timeout);
      if (!r) {
        out = fail(r.error());
        break;
      }
      if (!*r) {
        out = fail(ErrorKind::Timeout, "sniff: no reading within the timeout");
        break;
      }
      const auto status = collector_.add(**r);
      ++sniffed;
      if (ctx_.bus != nullptr) ctx_.bus->publish(CountsProgress{in_.run_id, Block::Equilibrate, sniffed, p.sniff.counts});
      if (status != collect::CollectStatus::Running) stop_sniff();
    } else if (!wait((close_at ? *close_at : open_at) - now)) {
      break;
    }
  }
  stop_sniff();
  return out;
}

Result<void> MeasurementEngine::whiff() {
  const auto& p = in_.plan;
  Whiff w;
  w.sniff = p.whiff.counts;
  for (const auto& c : p.whiff.checks) {
    auto expr = parse_expression(c.check);
    auto action = parse_whiff_action(c.action);
    if (!expr || !action) return fail(ErrorKind::Config, "whiff check '" + c.check + "' is invalid");
    w.checks.push_back({c.check, std::shared_ptr<const Expr>(std::move(*expr)), *action});
  }
  auto r = collect(Block::Equilibrate, {collect::SeriesKind::Whiff, p.whiff.counts, channels_of(p, p.main.hops.front()), "whiff"},
                   p.whiff.integration_s, {});
  if (!r) return fail(r.error());
  if (stopping()) return {};
  const auto action = evaluate_whiff(w, collector_.metrics(), in_.variables).value_or(WhiffCheck::Action::RunRemainder);
  result_.whiff = action;
  publish_log(LogLevel::Info, "whiff: " + std::string(to_string(action)));
  if (auto h = call_hook("on_whiff_result", {{"result", std::string(to_string(action))}}); !h) return h;
  switch (action) {
    case WhiffCheck::Action::RunRemainder: break;
    case WhiffCheck::Action::Pump: {
      const auto& eq = p.equilibration;
      if (!eq.inlet.empty()) {
        if (auto c = ctx_.valves->close(eq.inlet); !c) return c;
        inlet_open_ = false;
      }
      if (!eq.outlet.empty()) {
        if (auto o = ctx_.valves->open(eq.outlet); !o) return o;
      }
      stop_ = Stop::Terminate;
      break;
    }
    case WhiffCheck::Action::Abort: stop_ = Stop::Abort; break;
  }
  return {};
}

Result<void> MeasurementEngine::main() {
  const auto& p = in_.plan;
  for (int cycle = 0; cycle < p.main.cycles; ++cycle) {
    for (const auto& hop : p.main.hops) {
      if (stopping()) return {};
      if (auto r = move_to(plan::hop_target(p, hop), hop.settle_s, hop.protect); !r) return r;
      auto channels = channels_of(p, hop);
      last_channels_ = channels;
      collect::ReadingHook hook;
      if (!hop.baseline) hook = [this](int count, double t) { return main_reading(count, t); };
      auto r = collect(Block::Main,
                       {hop.baseline ? collect::SeriesKind::Baseline : collect::SeriesKind::Signal, hop.counts,
                        std::move(channels), "main"},
                       p.main.integration_s, std::move(hook));
      if (!r) return fail(r.error());
      if (*r == collect::CollectStatus::Truncated) return {};
    }
  }
  return {};
}

Result<void> MeasurementEngine::call_hook(std::string_view entry, const scripting::ValueMap& args) {
  if (!in_.plan.hook || ctx_.hook == nullptr) return {};
  Api api(*this);
  auto r = ctx_.hook->call(entry, api, *token_, args);
  if (!r && token_->requested()) return {};
  return r;
}

Result<void> MeasurementEngine::move_to(const std::optional<plan::HopTarget>& target, double settle_s,
                                        const std::vector<std::string>& protect) {
  if (!target) return {};  // collect wherever the magnet is
  if (position_ && *position_ == *target) return {};
  for (const auto& d : protect) {
    if (auto r = ctx_.spectrometer.protect(d, true); !r) return r;
    protected_.insert(d);
  }
  Result<void> r = ctx_.spectrometer.position(*target);
  if (r) {
    position_ = *target;
    wait(settle_s);
  } else {
    position_.reset();
  }
  for (const auto& d : protect) {
    auto u = ctx_.spectrometer.protect(d, false);
    if (u) protected_.erase(d);
    if (!u && r) r = u;
  }
  return r;
}

Result<collect::CollectStatus> MeasurementEngine::collect(Block block, collect::CollectionSpec spec,
                                                          double integration_s, collect::ReadingHook hook) {
  if (spec.counts <= 0) return collect::CollectStatus::Complete;
  const auto integration = seconds_to(integration_s);
  const auto timeout =
      std::chrono::duration_cast<pychron::Duration>(integration * options_.timeout_factor) + options_.timeout_slack;
  if (auto r = ctx_.spectrometer.start_acquisition(integration); !r) return fail(r.error());
  acquiring_ = true;
  collector_.begin(std::move(spec), std::move(hook));
  collecting_ = true;

  std::optional<Error> error;
  auto status = collect::CollectStatus::Running;
  while (status == collect::CollectStatus::Running && !stopping()) {
    {
      std::lock_guard lock(truncate_mutex_);
      if (truncate_request_ && block != Block::Equilibrate) collector_.truncate();
    }
    auto r = ctx_.spectrometer.next_reading(timeout);
    if (!r) {
      error = r.error();
      break;
    }
    if (!*r) {
      error = Error{ErrorKind::Timeout, std::string(to_string(block)) + ": no reading within the timeout", {}};
      break;
    }
    status = collector_.add(**r);
    if (ctx_.bus != nullptr)
      ctx_.bus->publish(CountsProgress{in_.run_id, block, collector_.count(), collector_.target()});
  }
  collecting_ = false;
  collector_.finish();
  ctx_.spectrometer.stop_acquisition();
  acquiring_ = false;
  if (error) return fail(*error);

  if (status == collect::CollectStatus::Truncated && stop_ == Stop::None) {
    std::optional<double> ratio;
    {
      std::lock_guard lock(truncate_mutex_);
      if (block != Block::Equilibrate) ratio = std::exchange(truncate_request_, std::nullopt);
    }
    if (block == Block::Main && ratio) {
      main_truncated_ = true;
      result_.count_scale = *ratio;
    }
    if (block == Block::Main) break_main_ = false;  // a non-resuming action ended main
  }
  return status;
}

bool MeasurementEngine::main_reading(int /*count*/, double t) {
  ++main_count_;
  if (in_.plan.main.time_zero.kind == plan::TimeZeroKind::OnFirstCount && !collector_.time_zero())
    collector_.set_time_zero(t);
  if (auto trip = conditionals_->evaluate(kMeasurementOrder, collector_.metrics(), in_.variables, main_count_, t)) {
    collector_.add_trips({*trip});
    handle_trip(*trip);
  }
  return stop_ != Stop::None || break_main_;
}

bool MeasurementEngine::sniff_reading() {
  const double t = collector_.seconds(ctx_.clock.now());
  if (auto trip = conditionals_->evaluate(ConditionalKind::Equilibration, collector_.metrics(), in_.variables,
                                          collector_.count(), t)) {
    collector_.add_trips({*trip});
    handle_trip(*trip);
  }
  return false;
}

void MeasurementEngine::handle_trip(const Trip& trip) {
  using T = ActionSpec::Type;
  if (ctx_.bus != nullptr) ctx_.bus->publish(ConditionalTripped{in_.run_id, trip});
  publish_log(LogLevel::Info, "conditional '" + trip.name + "' tripped (" + std::string(to_string(trip.kind)) + "): " +
                                  trip.check);
  switch (trip.kind) {
    case ConditionalKind::Truncation:
      request_truncate(trip.action.quick ? 0.25 : trip.abbreviated_count_ratio);
      return;
    case ConditionalKind::Termination: stop_ = Stop::Terminate; return;
    case ConditionalKind::Cancelation:
      stop_ = Stop::Cancel;
      result_.cancel_queue = true;
      return;
    case ConditionalKind::Equilibration: close_inlet_now_ = true; return;
    case ConditionalKind::Modification:
      result_.modifications.push_back(trip);
      if (trip.truncate) request_truncate(trip.abbreviated_count_ratio);
      if (trip.terminate) stop_ = Stop::Terminate;
      return;
    case ConditionalKind::PreRun:
    case ConditionalKind::PostRun: return;  // between-run checks; not evaluated in-run
    case ConditionalKind::Action: break;
  }
  switch (trip.action.type) {
    case T::Truncate: request_truncate(trip.action.quick ? 0.25 : 1.0); return;
    case T::Terminate: stop_ = Stop::Terminate; return;
    case T::Cancel:
      stop_ = Stop::Cancel;
      result_.cancel_queue = true;
      return;
    case T::SetParam: in_.variables.params[trip.action.name] = trip.action.value; break;
    case T::RunHook:
      if (auto r = call_hook(trip.action.name); !r)
        result_.notes.push_back("hook '" + trip.action.name + "' failed: " + r.error().what);
      break;
    case T::Notify:
      result_.notes.push_back("notify: " + trip.name);
      publish_log(LogLevel::Warn, "notify: conditional '" + trip.name + "' (" + trip.check + ")");
      break;
    default: break;
  }
  if (!trip.resume) break_main_ = true;  // pychron: a non-resuming action ends the block
}

void MeasurementEngine::truncate(bool quick) { request_truncate(quick ? 0.25 : 1.0); }

void MeasurementEngine::request_truncate(double ratio) {
  std::lock_guard lock(truncate_mutex_);
  truncate_request_ = ratio;
  if (collecting_) collector_.truncate();
}

bool MeasurementEngine::wait(double seconds) {
  if (seconds > 0) {
    const auto d = seconds_to(seconds);
    if (options_.sleep) {
      options_.sleep(d);
    } else if (token_->wait_until(ctx_.clock, ctx_.clock.now() + d) == scripting::WaitResult::Cancelled) {
      return false;
    }
  }
  return !token_->requested();
}

bool MeasurementEngine::stopping() const { return token_->requested() || stop_ != Stop::None; }

void MeasurementEngine::publish_log(LogLevel level, std::string message) {
  if (ctx_.bus != nullptr) ctx_.bus->publish(Log{level, "measurement", std::move(message), ctx_.clock.now()});
}

int MeasurementEngine::scaled(int counts) const {
  if (!main_truncated_) return counts;
  return std::max(1, static_cast<int>(std::ceil(counts * result_.count_scale)));
}

void MeasurementEngine::cleanup(bool abnormal) {
  if (acquiring_) {
    ctx_.spectrometer.stop_acquisition();
    acquiring_ = false;
  }
  for (const auto& d : std::set<std::string>(protected_)) {
    if (auto r = ctx_.spectrometer.protect(d, false); r) {
      protected_.erase(d);
    } else {
      publish_log(LogLevel::Error, "could not unprotect " + d + ": " + r.error().what);
    }
  }
  if (abnormal && inlet_open_ && ctx_.valves != nullptr) {
    if (auto r = ctx_.valves->close(in_.plan.equilibration.inlet); r) {
      inlet_open_ = false;
    } else {
      publish_log(LogLevel::Error, "could not close inlet " + in_.plan.equilibration.inlet + ": " + r.error().what);
    }
  }
}

}  // namespace pychron::experiment::measurement
