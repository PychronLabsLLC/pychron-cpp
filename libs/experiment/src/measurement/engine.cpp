#include "pychron/experiment/measurement/engine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <utility>

#include "pychron/core/events.hpp"
#include "pychron/devices/link.hpp"
#include "pychron/experiment/conditionals/library.hpp"
#include "pychron/reduction/fits.hpp"

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

// A number as a person writes it: 500, 0.05, 1.2e-05.
std::string plain(double v) {
  std::array<char, 32> buf{};
  std::snprintf(buf.data(), buf.size(), "%g", v);
  return buf.data();
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

Result<ConditionalSet> plan_conditionals(const plan::MeasurementPlan& plan) { return plan_truncations(plan); }

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
  gap_from_.reset();
  silence_error_.clear();
  main_gap_ = false;
  main_cut_ = false;
  // A truncate requested before run() (e.g. during extraction) is kept.
  started_ = ctx_.clock.now();
  collector_.start(started_);
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
    if (options_.on_block_started) options_.on_block_started(b);
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
  if (auto r = step(Block::Main, [&] {
        if (auto m = main(); !m) return m;
        return check_main_counts();
      });
      !r)
    return r;
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
  // A small peak (after a blank) still fits, and its centre would go into the
  // field table. Only readings of the main block say how much signal there
  // is: with none (before the gas is in) the block runs.
  if (p.peak_center.min_signal > 0) {
    const auto series = collector_.series({req.isotope, req.detector, collect::SeriesKind::Signal});
    if (series && !series->v.empty()) {
      const auto n = std::min(series->v.size(), plan::kPeakCenterSignalReadings);
      const double mean = std::accumulate(series->v.end() - static_cast<std::ptrdiff_t>(n), series->v.end(), 0.0) /
                          static_cast<double>(n);
      if (mean < p.peak_center.min_signal) {
        PeakCenterReport skipped;
        skipped.request = std::move(req);
        skipped.skipped = true;
        skipped.message = "signal " + plain(mean) + " below min_signal " + plain(p.peak_center.min_signal);
        skipped.finished = ctx_.clock.now();
        publish_log(LogLevel::Info, std::string(to_string(block)) + " skipped: " + skipped.message);
        result_.peak_centers.push_back(std::move(skipped));
        return {};
      }
    }
  }
  auto rep = ctx_.peak_center->peak_center(req, *token_);
  position_.reset();  // the job leaves the magnet wherever it ended
  if (!rep) {
    if (token_->requested()) return {};
    return fail(rep.error());
  }
  if (!rep->ok)
    publish_log(LogLevel::Warn, std::string(to_string(block)) + " failed: " + rep->message + "; continuing");
  rep->finished = ctx_.clock.now();
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
    if (auto r = start_acquisition(sniff_integration); !r) {
      if (!is_transient(r.error().kind)) return r;
      publish_log(LogLevel::Warn, "sniff not started: " + r.error().what);  // the equilibration goes on
      return {};
    }
    acquiring_ = true;
    last_reading_ = ctx_.clock.now();
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
        if (auto r = close_inlet(); !r) {
          out = r;
          break;
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
      if (!r && !is_transient(r.error().kind)) {
        out = fail(r.error());
        break;
      }
      std::optional<spectrometer::Reading> reading;
      if (r && *r) {
        reading = std::move(**r);
      } else {
        // A silence: waited out up to the inlet's next move, which is made on time.
        if (!r) silence_error_ = r.error().what;
        const TimePoint cap = started_ + seconds_to(close_at ? *close_at : open_at);
        auto w = wait_out_gap(Block::Equilibrate, sniff_integration, timeout, cap);
        if (!w) {
          out = fail(w.error());
          break;
        }
        if (w->end == GapEnd::Cap) continue;
        if (w->end == GapEnd::NotResumed) {
          stop_sniff();
          continue;
        }
        reading = std::move(w->reading);
      }
      last_reading_ = ctx_.clock.now();
      const auto status = collector_.add(*reading);
      ++sniffed;
      if (ctx_.bus != nullptr) ctx_.bus->publish(CountsProgress{in_.run_id, Block::Equilibrate, sniffed, p.sniff.counts});
      if (status != collect::CollectStatus::Running) stop_sniff();
    } else if (!wait((close_at ? *close_at : open_at) - now)) {
      break;
    }
  }
  stop_sniff();
  if (gap_from_) close_gap(Block::Equilibrate, ctx_.clock.now(), false);  // silent until the inlet closed
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
      if (auto c = close_inlet(); !c) return c;
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
      if (*r == collect::CollectStatus::Truncated || main_cut_) return {};
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
    auto u = critical("unprotect " + d, [&] { return ctx_.spectrometer.protect(d, false); });
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
  if (auto r = start_acquisition(integration); !r) {
    if (!is_transient(r.error().kind)) return fail(r.error());
    // Not started, and not again (spec 4.4): the collection ends without data.
    silence_error_ = r.error().what;
    gap_from_ = ctx_.clock.now();
    close_gap(block, ctx_.clock.now(), false);
    if (block == Block::Main) main_cut_ = true;
    return collect::CollectStatus::Complete;
  }
  acquiring_ = true;
  // A collection's planned end: after a gap it ends then, not at its count,
  // so that its end and time zero do not move (spec 4.4).
  const TimePoint planned_end = ctx_.clock.now() + integration * spec.counts;
  last_reading_ = ctx_.clock.now();
  collector_.begin(std::move(spec), std::move(hook));
  collecting_ = true;

  std::optional<Error> error;
  bool had_gap = false;
  auto status = collect::CollectStatus::Running;
  while (status == collect::CollectStatus::Running && !stopping()) {
    {
      std::lock_guard lock(truncate_mutex_);
      if (truncate_request_ && block != Block::Equilibrate) collector_.truncate();
    }
    auto r = ctx_.spectrometer.next_reading(timeout);
    if (!r && !is_transient(r.error().kind)) {
      error = r.error();
      break;
    }
    std::optional<spectrometer::Reading> reading;
    if (r && *r) {
      reading = std::move(**r);
    } else {
      if (!r) silence_error_ = r.error().what;
      auto w = wait_out_gap(block, integration, timeout, planned_end);
      if (!w) {
        error = w.error();
        break;
      }
      if (block == Block::Main) main_gap_ = true;
      if (w->end != GapEnd::Resumed) {
        if (gap_from_) close_gap(block, ctx_.clock.now(), false);
        if (w->end == GapEnd::NotResumed && block == Block::Main) main_cut_ = true;
        break;
      }
      reading = std::move(w->reading);
      had_gap = true;
    }
    last_reading_ = ctx_.clock.now();
    status = collector_.add(*reading);
    if (ctx_.bus != nullptr)
      ctx_.bus->publish(CountsProgress{in_.run_id, block, collector_.count(), collector_.target()});
    if (had_gap && status == collect::CollectStatus::Running && ctx_.clock.now() >= planned_end) break;
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
  if (status == collect::CollectStatus::Running) status = collect::CollectStatus::Complete;  // ended by time
  return status;
}

std::string describe(const MeasurementGap& gap) {
  std::array<char, 160> text{};
  std::snprintf(text.data(), text.size(), "%s: no readings from %.1f s to %.1f s; %s",
                std::string(to_string(gap.block)).c_str(), gap.from, gap.to,
                gap.resumed ? "they resumed" : "they did not resume");
  return text.data();
}

Result<MeasurementEngine::GapWait> MeasurementEngine::wait_out_gap(Block block, pychron::Duration integration,
                                                                 pychron::Duration timeout, TimePoint cap) {
  const auto& clock = ctx_.clock;
  if (!gap_from_) {
    gap_from_ = last_reading_;
    publish_log(LogLevel::Warn, std::string(to_string(block)) + ": no readings; waiting for them" +
                                    (silence_error_.empty() ? "" : " (" + silence_error_ + ")"));
  }
  const TimePoint max_end = *gap_from_ + options_.max_gap;
  const TimePoint deadline = std::min(max_end, cap);
  // Nothing else is done while waiting: no valve, no detector, no safe state.
  while (!stopping()) {
    const TimePoint now = clock.now();
    if (now >= deadline) break;
    const auto left = deadline - now;
    auto r = ctx_.spectrometer.next_reading(std::min(timeout, std::chrono::duration_cast<pychron::Duration>(left)));
    if (r && *r) {
      close_gap(block, clock.now(), true);
      return GapWait{GapEnd::Resumed, std::move(**r)};
    }
    if (!r) {
      if (!is_transient(r.error().kind)) return fail(r.error());
      silence_error_ = r.error().what;
    }
    // A port that answered at once (its link is down) is asked again after a while.
    if (clock.now() == now && !wait(std::chrono::duration<double>(std::min<pychron::Duration>(integration, left)).count()))
      break;
  }
  if (stopping() || clock.now() < max_end) return GapWait{GapEnd::Cap, std::nullopt};
  // max_gap has passed: the acquisition is started again once, if the link is back.
  publish_log(LogLevel::Warn, std::string(to_string(block)) + ": no readings for " +
                                  std::to_string(std::chrono::duration_cast<std::chrono::seconds>(options_.max_gap).count()) +
                                  " s; starting the acquisition again");
  ctx_.spectrometer.stop_acquisition();
  if (auto s = start_acquisition(integration); s) {
    auto r = ctx_.spectrometer.next_reading(timeout);
    if (r && *r) {
      close_gap(block, clock.now(), true);
      return GapWait{GapEnd::Resumed, std::move(**r)};
    }
    if (!r) silence_error_ = r.error().what;
  } else {
    silence_error_ = s.error().what;
  }
  close_gap(block, clock.now(), false);
  return GapWait{GapEnd::NotResumed, std::nullopt};
}

void MeasurementEngine::close_gap(Block block, TimePoint to, bool resumed) {
  if (!gap_from_) return;
  MeasurementGap gap{in_.run_id, block, collector_.seconds(*gap_from_), collector_.seconds(to), resumed, to};
  gap_from_.reset();
  publish_log(resumed ? LogLevel::Info : LogLevel::Warn, describe(gap));
  result_.gaps.push_back(gap);
  if (ctx_.bus != nullptr) ctx_.bus->publish(gap);
}

Result<void> MeasurementEngine::start_acquisition(pychron::Duration integration) {
  auto r = ctx_.spectrometer.start_acquisition(integration);
  if (!r && r.error().code == kOutcomeUnknown) {
    // It may or may not have started (spec 4.4): stopped and started again, once.
    publish_log(LogLevel::Warn, "acquisition start of unknown outcome: starting it again (" + r.error().what + ")");
    ctx_.spectrometer.stop_acquisition();
    r = ctx_.spectrometer.start_acquisition(integration);
  }
  return r;
}

Result<void> MeasurementEngine::critical(const std::string& what, const std::function<Result<void>()>& op) {
  const KeepTrying how{ctx_.clock,
                       ctx_.bus,
                       "measurement",
                       [this](pychron::Duration d) { return wait(std::chrono::duration<double>(d).count()); },
                       [this](const std::string& message) { publish_log(LogLevel::Warn, message); },
                       options_.critical_window,
                       options_.critical_alarm_after};
  return keep_trying(how, what, op);
}

Result<void> MeasurementEngine::close_inlet() {
  const auto& inlet = in_.plan.equilibration.inlet;
  if (inlet.empty()) return {};
  if (auto r = critical("close inlet " + inlet, [&] { return ctx_.valves->close(inlet); }); !r) return r;
  inlet_open_ = false;
  return {};
}

Result<void> MeasurementEngine::check_main_counts() {
  if (!main_gap_ && !main_cut_) return {};
  int planned = 0;
  for (const auto& hop : in_.plan.main.hops)
    if (!hop.baseline) planned += hop.counts;
  planned *= in_.plan.main.cycles;
  std::size_t fit_minimum = 0;
  for (const auto& [key, kind] : in_.plan.fits.signal) {
    reduction::FitSpec f;
    f.kind = kind;
    fit_minimum = std::max(fit_minimum, reduction::parameter_count(f));
  }
  const int minimum = std::max(static_cast<int>(std::ceil(options_.min_counts_fraction * planned)),
                               static_cast<int>(fit_minimum));
  if (main_count_ < minimum)
    return fail(ErrorKind::Io, "main: " + std::to_string(main_count_) + " of " + std::to_string(planned) +
                                   " counts when the spectrometer went silent, at least " + std::to_string(minimum) +
                                   " needed" + (silence_error_.empty() ? "" : ": " + silence_error_));
  if (main_cut_) {
    main_truncated_ = true;
    result_.truncated_by_link = true;
    publish_log(LogLevel::Warn, "main truncated by the spectrometer link: " + std::to_string(main_count_) + " of " +
                                    std::to_string(planned) + " counts");
  }
  return {};
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
