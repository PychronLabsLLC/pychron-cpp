#include "pychron/experiment/measurement/engine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <type_traits>
#include <utility>
#include <variant>

#include "pychron/core/events.hpp"
#include "pychron/devices/link.hpp"
#include "pychron/experiment/conditionals/library.hpp"
#include "pychron/reduction/fits.hpp"

namespace pychron::experiment::measurement {

namespace {

pychron::Duration seconds_to(double s) {
  return std::chrono::duration_cast<pychron::Duration>(std::chrono::duration<double>(std::max(s, 0.0)));
}

// A number as a person writes it: 500, 0.05, 1.2e-05.
std::string plain(double v) {
  std::array<char, 32> buf{};
  std::snprintf(buf.data(), buf.size(), "%g", v);
  return buf.data();
}

// What a position's active entries collect, under their labels.
std::vector<collect::Channel> channels_of(const plan::Position& pos) {
  std::vector<collect::Channel> out;
  for (const auto& e : plan::collecting(pos)) out.push_back({e.label, e.detector});
  return out;
}

// A baseline segment collects per detector, with no label, each detector once.
std::vector<collect::Channel> baseline_channels(const plan::Position& pos) {
  std::vector<collect::Channel> out;
  for (const auto& e : plan::collecting(pos)) {
    const auto same = [&](const collect::Channel& c) { return c.detector == e.detector; };
    if (std::none_of(out.begin(), out.end(), same)) out.push_back({"", e.detector});
  }
  return out;
}

// "Ar40@H1", "mass 34.2@H1", "dac 1.234", "hv 4500@H1".
std::string describe(const plan::At& at) {
  return std::visit(
      [](const auto& a) -> std::string {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, plan::AtLabel>) return a.label + "@" + a.detector;
        else if constexpr (std::is_same_v<T, plan::AtMass>) return "mass " + plain(a.mass) + "@" + a.detector;
        else if constexpr (std::is_same_v<T, plan::AtDac>) return "dac " + plain(a.dac);
        else return "hv " + plain(a.hv) + "@" + a.detector;
      },
      at);
}

bool is_sniff(const plan::Segment& s) {
  return s.kind == plan::SegmentKind::Sniff && s.when == plan::When::DuringEquilibration;
}
bool is_whiff(const plan::Segment& s) { return s.kind == plan::SegmentKind::Whiff && s.when == plan::When::AfterInlet; }

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
    case Block::Segment: return "segment";
    case Block::Step: return "step";
    case Block::Prepare: return "prepare";
    case Block::PumpOut: return "pump_out";
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
    return e_.position_magnet(plan::AtLabel{std::string(isotope), std::string(detector)}, 0, {}, -1, -1);
  }

  Result<void> acquire(int counts, double integration_time_s) override {
    if (e_.collecting_) return fail(ErrorKind::Config, "acquire() is not allowed inside a collection");
    if (counts <= 0 || integration_time_s <= 0) return fail(ErrorKind::Config, "acquire() needs counts > 0 and integration > 0");
    auto channels = e_.last_channels_;
    if (channels.empty() && e_.gas_ != nullptr)
      channels = channels_of(e_.gas_->segments[e_.main_].positions.front());
    collect::CollectionSpec spec;
    spec.counts = counts;
    spec.channels = std::move(channels);
    spec.label = "hook";
    if (e_.gas_ != nullptr) {
      spec.gas = e_.gas_->name;
      spec.segment = static_cast<int>(e_.main_);
    }
    auto r = e_.collect(Block::Main, std::move(spec), integration_time_s, {});
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
  const plan::Gas* gas = p.gases.empty() ? nullptr : &p.gases.front();
  const plan::Segment* main = gas != nullptr ? plan::main_segment(*gas) : nullptr;
  if (gas == nullptr) problems.emplace_back("plan has no gas");
  else if (main == nullptr) problems.emplace_back("plan has no after_inlet signal segment");
  else if (main->positions.empty()) problems.emplace_back("plan has no hops");
  if (gas != nullptr) {
    for (std::size_t s = 0; s < gas->segments.size(); ++s) {
      const auto& seg = gas->segments[s];
      if (seg.kind != plan::SegmentKind::Signal || p.detectors.reference.empty()) continue;
      for (std::size_t j = 0; j < seg.positions.size(); ++j) {
        if (!plan::position_target(p, seg.positions[j]))
          problems.push_back("segment[" + std::to_string(s) + "].positions[" + std::to_string(j) +
                             "]: no magnet position (reference detector not in position)");
      }
    }
    const auto& eq = gas->equilibration;
    if ((!eq.inlet.empty() || !eq.outlet.empty()) && ctx_.valves == nullptr)
      problems.emplace_back("plan equilibrates through valves but no valve service is configured");
    const bool centers = std::any_of(gas->segments.begin(), gas->segments.end(),
                                     [](const plan::Segment& seg) { return seg.kind == plan::SegmentKind::PeakCenter; });
    if (centers && ctx_.peak_center == nullptr)
      problems.emplace_back("plan enables peak centering but no peak-center service is configured");
  }
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
  gas_ = nullptr;
  main_ = 0;
  time_zero_ = {};
  block_ = Block::Main;
  position_.reset();
  table_.clear();
  unwritten_tables_.clear();
  standing_.clear();
  step_integration_.reset();
  time_zero_by_step_ = false;
  blanked_ = false;
  optics_.clear();
  deflected_.clear();
  settle_due_ = false;
  integration_.reset();
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
  collector_.set_plan(in_.plan);
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

Block MeasurementEngine::block_of(std::size_t segment) const {
  using plan::SegmentKind;
  using plan::When;
  if (segment == main_) return Block::Main;
  const auto& segs = gas_->segments;
  const auto& seg = segs[segment];
  // The first segment of a v1 kind and placement takes the v1 block's name.
  const auto first_of = [&](SegmentKind kind, When when) {
    if (seg.kind != kind || seg.when != when) return false;
    const auto it = std::find_if(segs.begin(), segs.end(),
                                 [&](const plan::Segment& s) { return s.kind == kind && s.when == when; });
    return it - segs.begin() == static_cast<std::ptrdiff_t>(segment);
  };
  if (first_of(SegmentKind::PeakCenter, When::BeforeInlet)) return Block::PeakCenterBefore;
  if (first_of(SegmentKind::PeakCenter, When::AfterRun)) return Block::PeakCenterAfter;
  if (first_of(SegmentKind::Baseline, When::BeforeInlet)) return Block::BaselineBefore;
  if (first_of(SegmentKind::Baseline, When::AfterMain)) return Block::BaselineAfter;
  return plan::is_step(seg.kind) ? Block::Step : Block::Segment;
}

std::string MeasurementEngine::collection_label(std::size_t segment, Block block) const {
  switch (block) {
    case Block::Main:
    case Block::BaselineBefore:
    case Block::BaselineAfter: return std::string(to_string(block));
    default: break;
  }
  const auto& seg = gas_->segments[segment];
  if (seg.kind == plan::SegmentKind::Sniff) return "sniff";
  if (seg.kind == plan::SegmentKind::Whiff) return "whiff";
  const std::string at = "segment[" + std::to_string(segment) + "]";
  return gas_->name.empty() ? at : gas_->name + "." + at;
}

void MeasurementEngine::trace(int segment, int position, std::string what, std::string detail) {
  result_.trace.push_back(TraceEntry{gas_ != nullptr ? gas_->name : std::string(), segment, position, std::move(what),
                                     std::move(detail), collector_.seconds(ctx_.clock.now())});
}

const plan::Equilibration& MeasurementEngine::equilibration() const {
  return gas_ != nullptr ? gas_->equilibration : in_.plan.equilibration;
}

Result<void> MeasurementEngine::run_blocks() {
  // One gas for now (Phase 5 of the v2 plan runs every gas and every `when`).
  gas_ = &in_.plan.gases.front();
  const auto& segs = gas_->segments;
  main_ = static_cast<std::size_t>(plan::main_segment(*gas_) - segs.data());
  const auto& main = segs[main_];
  time_zero_ = main.time_zero ? *main.time_zero : gas_->time_zero.value_or(plan::TimeZero{});

  auto step = [&](Block b, int segment, auto&& fn) -> Result<void> {
    if (stopping()) return {};
    block_ = b;
    if (options_.on_block_started) options_.on_block_started(b);
    if (ctx_.bus != nullptr) ctx_.bus->publish(BlockStarted{in_.run_id, b, gas_->name, segment});
    result_.blocks.push_back(b);
    Result<void> r = fn();
    if (ctx_.bus != nullptr) ctx_.bus->publish(BlockFinished{in_.run_id, b, r.has_value(), gas_->name, segment});
    return r;
  };
  auto run_when = [&](plan::When when) -> Result<void> {
    for (std::size_t s = 0; s < segs.size(); ++s) {
      if (segs[s].when != when) continue;
      if (auto r = step(block_of(s), static_cast<int>(s), [&] { return run_segment(s); }); !r) return r;
    }
    return {};
  };

  if (auto r = run_when(plan::When::BeforeInlet); !r) return r;
  const auto position_first = [&] {
    return enter_position(main, main.positions.front(), static_cast<int>(main_), 0);
  };
  if (auto r = step(Block::PositionFirstHop, -1, position_first); !r) return r;
  if (auto r = step(Block::Equilibrate, -1, [&] { return equilibrate(); }); !r) return r;
  if (!stopping()) {
    if (auto r = call_hook("before_main"); !r) return r;
  }
  const auto run_main = [&] {
    if (auto m = run_segment(main_); !m) return m;
    return check_main_counts();
  };
  if (auto r = step(Block::Main, static_cast<int>(main_), run_main); !r) return r;
  // The other after_inlet signal segments, in file order after main.
  for (std::size_t s = main_ + 1; s < segs.size(); ++s) {
    if (segs[s].kind != plan::SegmentKind::Signal || segs[s].when != plan::When::AfterInlet) continue;
    if (auto r = step(block_of(s), static_cast<int>(s), [&] { return run_segment(s); }); !r) return r;
  }
  if (!stopping()) {
    if (auto r = call_hook("after_main"); !r) return r;
  }
  if (auto r = run_when(plan::When::AfterMain); !r) return r;
  return run_when(plan::When::AfterRun);
}

Result<void> MeasurementEngine::run_segment(std::size_t segment) {
  const auto& seg = gas_->segments[segment];
  if (plan::is_step(seg.kind)) return run_step(segment);
  const bool is_main = segment == main_;
  {
    std::lock_guard lock(cycles_mutex_);
    segment_running_ = true;
    cycle_ = 0;
    cycles_override_.reset();
  }
  // The segment's cycles, or what a live edit made them; notes the cycle in progress.
  const auto more = [&](int cycle) {
    std::lock_guard lock(cycles_mutex_);
    cycle_ = cycle;
    return cycle < cycles_override_.value_or(seg.cycles);
  };
  const auto run_cycles = [&]() -> Result<void> {
    for (int cycle = 0; more(cycle); ++cycle) {
      for (std::size_t j = 0; j < seg.positions.size(); ++j) {
        if (stopping()) return {};
        const auto& pos = seg.positions[j];
        // Main collects as planned; what runs after it scales by a truncation.
        const int counts = is_main ? pos.counts : scaled(pos.counts);
        auto r = run_position(*gas_, segment, j, pos, counts);
        if (!r) return fail(r.error());
        if (is_main && (*r == collect::CollectStatus::Truncated || main_cut_)) return {};
      }
    }
    return {};
  };
  Result<void> r = run_cycles();
  {
    std::lock_guard lock(cycles_mutex_);
    segment_running_ = false;
    if (is_main) main_cycles_ = cycles_override_.value_or(seg.cycles);
  }
  if (!r) return r;  // cleanup restores what is left
  // A deflection does not outlive its segment (legacy peak_hop_collector reset).
  return restore_deflections(nullptr, static_cast<int>(segment), -1);
}

void MeasurementEngine::set_cycles(int cycles) {
  std::lock_guard lock(cycles_mutex_);
  if (!segment_running_) return;
  cycles_override_ = std::max(cycles, cycle_ + 1);
}

Result<void> MeasurementEngine::run_step(std::size_t segment) {
  const auto& seg = gas_->segments[segment];
  if (seg.kind == plan::SegmentKind::PeakCenter && seg.peak_center) {
    const auto& pc = *seg.peak_center;
    const auto& det = pc.detectors.empty() ? in_.plan.detectors.reference : pc.detectors.front();
    trace(static_cast<int>(segment), -1, "step", "peak_center " + pc.label + "@" + det);
    return peak_center(pc);
  }
  const int si = static_cast<int>(segment);
  switch (seg.kind) {
    case plan::SegmentKind::Set:
      if (seg.set) return set_step(*seg.set, si);
      return {};
    case plan::SegmentKind::BlankBeam: {
      const bool on = seg.blank_beam.value_or(true);
      trace(si, -1, "step", std::string("blank_beam ") + (on ? "on" : "off"));
      if (auto r = ctx_.spectrometer.blank_beam(on); !r) return r;
      blanked_ = on;
      return {};
    }
    case plan::SegmentKind::Park: {
      if (!seg.park) return {};
      const auto& park = *seg.park;
      plan::At target = park.mass ? plan::At{plan::AtMass{*park.mass, park.detector}}
                                  : plan::At{plan::AtLabel{park.label, park.detector}};
      trace(si, -1, "step", "park " + describe(target));
      if (position_ && *position_ == target) return {};
      auto r = ctx_.spectrometer.position(target);
      if (r) position_ = target;
      else position_.reset();
      return r;
    }
    case plan::SegmentKind::Delay: {
      const double d = seg.delay_s.value_or(0);
      trace(si, -1, "step", "delay " + plain(d));
      wait(d);
      return {};
    }
    case plan::SegmentKind::TimeZero: {
      const auto tz = seg.time_zero.value_or(plan::TimeZero{plan::TimeZeroKind::Now, 0});
      const double at = collector_.seconds(ctx_.clock.now()) + (tz.kind == plan::TimeZeroKind::Offset ? tz.offset_s : 0.0);
      trace(si, -1, "step", "time_zero " + plain(at));
      collector_.set_time_zero(at);
      time_zero_by_step_ = true;  // the gas's policy does not set it again
      return {};
    }
    default: break;
  }
  return fail(ErrorKind::Config, std::string(plan::to_string(seg.kind)) + " step: not run by this build yet");
}

// Each sub-key one port call, in this order: deflection (by detector),
// gain, source, hv, cdd_voltage, integration_s. A deflection stands until
// the run ends (a position's own deflection of that detector goes back to
// it); integration_s holds for the gas's later collections whose position
// does not set its own.
Result<void> MeasurementEngine::set_step(const plan::SetStep& set, int segment) {
  trace(segment, -1, "step", "set");
  for (const auto& [det, value] : set.deflection) {
    double original = 0;
    if (const auto it = standing_.find(det); it != standing_.end()) {
      original = it->second.original;
    } else if (auto now = ctx_.spectrometer.deflection(det)) {
      original = *now;
    } else {
      return fail(now.error());
    }
    standing_[det] = Deflected{value, original};  // before the call: a failed set is still restored
    auto r = ctx_.spectrometer.set_deflection(det, value);
    if (!r) return fail(r.error());
    trace(segment, -1, "deflect", det + "=" + plain(*r));
    settle_due_ = true;
  }
  for (const auto& [det, gain] : set.gain) {
    trace(segment, -1, "gain", det + "=" + plain(gain));
    if (auto r = ctx_.spectrometer.set_gain(det, gain); !r) return r;
  }
  for (const auto& [name, value] : set.source) {
    trace(segment, -1, "source", name + "=" + plain(value));
    if (auto r = ctx_.spectrometer.set_param(name, value); !r) return r;
    settle_due_ = true;
  }
  if (set.hv) {
    trace(segment, -1, "hv", plain(*set.hv));
    if (auto r = ctx_.spectrometer.set_hv(*set.hv); !r) return r;
    position_.reset();  // HV moves every mass
  }
  if (set.cdd_voltage) {
    trace(segment, -1, "cdd_voltage", plain(*set.cdd_voltage));
    if (auto r = ctx_.spectrometer.set_cdd_voltage(*set.cdd_voltage); !r) return r;
  }
  if (set.integration_s) {
    trace(segment, -1, "integration", plain(*set.integration_s));
    step_integration_ = *set.integration_s;
  }
  return {};
}

Result<void> MeasurementEngine::peak_center(const plan::PeakCenterStep& step) {
  const auto& p = in_.plan;
  std::vector<std::string> detectors = step.detectors;
  if (detectors.empty()) detectors.push_back(p.detectors.reference);
  // With several detectors the job runs once on each, in order (worked plan 7.3).
  for (const auto& det : detectors) {
    if (stopping()) return {};
    PeakCenterRequest req;
    req.isotope = step.label;
    req.detector = det;
    req.config = step.config;
    req.extra_detectors = step.extra_detectors;
    req.write_table = step.write_table;
    if (auto r = peak_center_on(std::move(req), step.min_signal); !r) return r;
  }
  return {};
}

Result<void> MeasurementEngine::peak_center_on(PeakCenterRequest req, double min_signal) {
  const Block block = block_;
  // A small peak (after a blank) still fits, and its centre would go into the
  // field table. Only signal readings say how much signal there is: with
  // none (before the gas is in) the step runs.
  if (min_signal > 0) {
    const auto series = collector_.series_of(req.isotope + "@" + req.detector, collect::kSignal);
    if (series && !series->v.empty()) {
      const auto n = std::min(series->v.size(), plan::kPeakCenterSignalReadings);
      const double mean = std::accumulate(series->v.end() - static_cast<std::ptrdiff_t>(n), series->v.end(), 0.0) /
                          static_cast<double>(n);
      if (mean < min_signal) {
        PeakCenterReport skipped;
        skipped.request = std::move(req);
        skipped.skipped = true;
        skipped.message = "signal " + plain(mean) + " below min_signal " + plain(min_signal);
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
  if (!rep->ok) {
    publish_log(LogLevel::Warn, std::string(to_string(block)) + " failed: " + rep->message + "; continuing");
    // The IC data is still worth having (legacy generate_ic_mftable): what
    // names the table runs on the current one, and the run says so.
    if (!req.write_table.empty() && unwritten_tables_.insert(req.write_table).second)
      result_.notes.push_back("table " + req.write_table + " not written: " + rep->message);
  }
  rep->finished = ctx_.clock.now();
  result_.peak_centers.push_back(*rep);
  return {};
}

Result<void> MeasurementEngine::equilibrate() {
  const auto& segs = gas_->segments;
  const auto& eq = gas_->equilibration;
  const auto now_s = [&] { return collector_.seconds(ctx_.clock.now()); };
  const auto sniff_at = std::find_if(segs.begin(), segs.end(), is_sniff);
  const plan::Segment* sniff = sniff_at == segs.end() ? nullptr : &*sniff_at;
  const auto whiff_at = std::find_if(segs.begin(), segs.end(), is_whiff);

  if (!eq.outlet.empty()) {
    if (auto r = ctx_.valves->close(eq.outlet); !r) return r;
  }

  bool sniffing = false;
  int sniffed = 0;
  const int sniff_counts = sniff != nullptr ? sniff->positions.front().counts : 0;
  const auto sniff_integration = seconds_to(sniff != nullptr ? sniff->integration_s : 1);
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
    if (sniff == nullptr || sniff_counts <= 0) return {};
    auto channels = channels_of(sniff->positions.front());
    last_channels_ = channels;
    if (auto r = start_acquisition(sniff_integration); !r) {
      if (!is_transient(r.error().kind)) return r;
      publish_log(LogLevel::Warn, "sniff not started: " + r.error().what);  // the equilibration goes on
      return {};
    }
    acquiring_ = true;
    last_reading_ = ctx_.clock.now();
    trace(static_cast<int>(sniff_at - segs.begin()), 0, "collect", std::to_string(sniff_counts));
    collect::CollectionSpec spec;
    spec.kind = std::string(collect::kSniff);
    spec.counts = sniff_counts;
    spec.channels = std::move(channels);
    spec.label = "sniff";
    spec.gas = gas_->name;
    spec.segment = static_cast<int>(sniff_at - segs.begin());
    collector_.begin(std::move(spec), [this](int, double) { return sniff_reading(); });
    collecting_ = true;
    sniffing = true;
    return {};
  };
  // With a whiff, sniffing starts once the whiff has decided.
  if (whiff_at == segs.end()) {
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
      if (time_zero_.kind == plan::TimeZeroKind::Offset && !time_zero_by_step_)
        collector_.set_time_zero(now + time_zero_.offset_s);
      if (whiff_at != segs.end()) {
        if (auto r = whiff(static_cast<std::size_t>(whiff_at - segs.begin())); !r) {
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
      if (time_zero_.kind == plan::TimeZeroKind::OnInletClose && !time_zero_by_step_) collector_.set_time_zero(now);
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
      note_beams(collect::kSniff, *reading);
      const auto status = collector_.add(*reading);
      ++sniffed;
      if (ctx_.bus != nullptr) ctx_.bus->publish(CountsProgress{in_.run_id, Block::Equilibrate, sniffed, sniff_counts});
      if (status != collect::CollectStatus::Running) stop_sniff();
    } else if (!wait((close_at ? *close_at : open_at) - now)) {
      break;
    }
  }
  stop_sniff();
  if (gap_from_) close_gap(Block::Equilibrate, ctx_.clock.now(), false);  // silent until the inlet closed
  return out;
}

Result<void> MeasurementEngine::whiff(std::size_t segment) {
  const auto& seg = gas_->segments[segment];
  const auto& pos = seg.positions.front();
  Whiff w;
  w.sniff = pos.counts;
  for (const auto& c : seg.checks) {
    auto expr = parse_expression(c.check);
    auto action = parse_whiff_action(c.action);
    if (!expr || !action) return fail(ErrorKind::Config, "whiff check '" + c.check + "' is invalid");
    w.checks.push_back({c.check, std::shared_ptr<const Expr>(std::move(*expr)), *action});
  }
  auto r = run_position(*gas_, segment, 0, pos, pos.counts);
  if (!r) return fail(r.error());
  if (stopping()) return {};
  const auto action = evaluate_whiff(w, collector_.metrics(), in_.variables).value_or(WhiffCheck::Action::RunRemainder);
  result_.whiff = action;
  publish_log(LogLevel::Info, "whiff: " + std::string(to_string(action)));
  if (auto h = call_hook("on_whiff_result", {{"result", std::string(to_string(action))}}); !h) return h;
  switch (action) {
    case WhiffCheck::Action::RunRemainder: break;
    case WhiffCheck::Action::Pump: {
      const auto& eq = gas_->equilibration;
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

Result<void> MeasurementEngine::call_hook(std::string_view entry, const scripting::ValueMap& args) {
  if (!in_.plan.hook || ctx_.hook == nullptr) return {};
  Api api(*this);
  auto r = ctx_.hook->call(entry, api, *token_, args);
  if (!r && token_->requested()) return {};
  return r;
}

Result<collect::CollectStatus> MeasurementEngine::run_position(const plan::Gas& gas, std::size_t segment,
                                                               std::size_t position, const plan::Position& pos,
                                                               int counts) {
  const auto& seg = gas.segments[segment];
  const int si = static_cast<int>(segment);
  const int pi = static_cast<int>(position);
  // A sniff or whiff reads where the magnet is (as the plan's duration counts it).
  const bool in_place = seg.kind == plan::SegmentKind::Sniff || seg.kind == plan::SegmentKind::Whiff;
  if (!in_place) {
    if (auto r = enter_position(seg, pos, si, pi); !r) return fail(r.error());
  }

  std::string_view kind = collect::kSignal;
  switch (seg.kind) {
    case plan::SegmentKind::Baseline: kind = collect::kBaseline; break;
    case plan::SegmentKind::Sniff: kind = collect::kSniff; break;
    case plan::SegmentKind::Whiff: kind = collect::kWhiff; break;
    default:
      if (pos.kind == plan::PositionKind::Baseline) kind = collect::kBaseline;
      break;
  }
  auto channels = seg.kind == plan::SegmentKind::Baseline ? baseline_channels(pos) : channels_of(pos);
  collect::ReadingHook hook;
  if (seg.kind == plan::SegmentKind::Signal) {
    last_channels_ = channels;
    if (kind == collect::kSignal) hook = [this](int count, double t) { return main_reading(count, t); };
  }
  const double integration_s = pos.integration_s.value_or(step_integration_.value_or(seg.integration_s));
  if (counts > 0) {
    // Acquisition starts again for every collection; a new integration is traced.
    if (integration_ && *integration_ != seconds_to(integration_s)) trace(si, pi, "integration", plain(integration_s));
    trace(si, pi, "collect", std::to_string(counts));
  }
  collect::CollectionSpec spec;
  spec.kind = std::string(kind);
  spec.counts = counts;
  spec.channels = std::move(channels);
  spec.label = collection_label(segment, block_);
  spec.gas = gas.name;
  spec.segment = si;
  spec.position = pi;
  auto status = collect(block_, std::move(spec), integration_s, std::move(hook));
  if (!status || in_place) return status;
  // What this position deflected and the next one (cyclically) does not set goes back.
  const auto& next = seg.positions[(position + 1) % seg.positions.size()];
  if (auto r = restore_deflections(&next, si, pi); !r) return fail(r.error());
  return status;
}

Result<void> MeasurementEngine::enter_position(const plan::Segment& seg, const plan::Position& pos, int segment,
                                               int position) {
  const auto target = plan::position_target(in_.plan, pos, ctx_.reference);
  const std::string& named = pos.table.empty() ? seg.table : pos.table;
  // A table a failed peak center did not write: the current one.
  const std::string table = unwritten_tables_.contains(named) ? std::string() : named;
  std::map<std::string, double> wanted;
  for (const auto& e : pos.collect)
    if (e.deflection) wanted[e.detector] = *e.deflection;
  std::map<std::string, double> current;
  for (const auto& [det, d] : deflected_) current[det] = d.wanted;
  const bool optics_same = pos.optics.empty() || pos.optics == optics_;
  const bool there = !target || (position_ && *position_ == *target);
  const Settle settle = settle_for(pos);
  note_modes(pos);
  if (there && optics_same && table == table_ && wanted == current && !settle_due_ && !settle.mode_change) return {};

  // Another table or other optics put a label elsewhere: the move is made again.
  if (table != table_) {
    position_.reset();
    auto r = ctx_.spectrometer.with_table(table);
    trace(segment, position, "table", table.empty() ? "(current)" : table);
    if (!r) return r;
    table_ = table;
  }
  if (!optics_same) {
    position_.reset();
    auto r = ctx_.spectrometer.apply_optics(pos.optics);
    trace(segment, position, "optics", pos.optics);
    if (!r) return r;
    optics_ = pos.optics;
  }

  std::vector<std::string> protect;
  for (const auto& e : pos.collect)
    if (e.protect) protect.push_back(e.detector);
  for (const auto& d : protect) {
    if (auto r = ctx_.spectrometer.protect(d, true); !r) return r;
    protected_.insert(d);
    trace(segment, position, "protect", d + " on");
  }
  Result<void> r;
  for (const auto& e : pos.collect) {
    if (!e.deflection) continue;
    const auto it = deflected_.find(e.detector);
    if (it != deflected_.end() && it->second.wanted == *e.deflection) continue;
    double original = 0;
    if (it != deflected_.end()) {
      original = it->second.original;
    } else if (auto now = ctx_.spectrometer.deflection(e.detector)) {
      original = *now;
    } else {
      r = fail(now.error());
      break;
    }
    // Recorded before the call, so that a failed set is still restored.
    deflected_[e.detector] = Deflected{*e.deflection, original};
    auto set = ctx_.spectrometer.set_deflection(e.detector, *e.deflection);
    if (!set) {
      r = fail(set.error());
      break;
    }
    trace(segment, position, "deflect", e.detector + "=" + plain(*set));
  }
  if (r && target && !(position_ && *position_ == *target)) {
    r = ctx_.spectrometer.position(*target);
    trace(segment, position, "move", describe(*target));
    if (r) position_ = *target;
    else position_.reset();
  }
  if (r) {
    settle_due_ = false;
    trace(segment, position, "settle", plain(settle.s));
    wait(settle.s);
  }
  for (const auto& d : protect) {
    auto u = critical("unprotect " + d, [&] { return ctx_.spectrometer.protect(d, false); });
    if (u) {
      protected_.erase(d);
      trace(segment, position, "protect", d + " off");
    }
    if (!u && r) r = u;
  }
  return r;
}

MeasurementEngine::Settle MeasurementEngine::settle_for(const plan::Position& pos) const {
  Settle out{pos.settle_s, false};
  const auto timing = [&](const std::string& det) -> const DetectorTiming* {
    const auto it = ctx_.timing.find(det);
    return it == ctx_.timing.end() ? nullptr : &it->second;
  };
  for (const auto& e : pos.collect) {
    const auto* t = timing(e.detector);
    if (t != nullptr && large_beam_.contains(e.detector)) out.s = std::max(out.s, t->settle_after_large_beam_s);
    const std::string& collector = t != nullptr && !t->collector.empty() ? t->collector : e.detector;
    const auto mode = modes_.find(collector);
    if (mode == modes_.end() || mode->second == e.detector) continue;
    // Either mode's switch time: going into CDD and coming back out both wait.
    out.mode_change = true;
    const auto* before = timing(mode->second);
    out.s = std::max({out.s, t != nullptr ? t->mode_switch_s : 0.0, before != nullptr ? before->mode_switch_s : 0.0});
  }
  return out;
}

void MeasurementEngine::note_modes(const plan::Position& pos) {
  for (const auto& e : pos.collect) {
    const auto it = ctx_.timing.find(e.detector);
    const bool own = it == ctx_.timing.end() || it->second.collector.empty();
    modes_[own ? e.detector : it->second.collector] = e.detector;
  }
}

void MeasurementEngine::note_beams(std::string_view kind, const spectrometer::Reading& reading) {
  if (kind == collect::kBaseline) return;  // owner, 2026-10-15: a baseline does not clear it
  for (const auto& [det, value] : reading.values) {
    const auto it = ctx_.timing.find(det);
    if (it == ctx_.timing.end() || !it->second.large_beam_threshold || !value) continue;
    if (value->mean > *it->second.large_beam_threshold) large_beam_.insert(det);
    else large_beam_.erase(det);
  }
}

Result<void> MeasurementEngine::restore_deflections(const plan::Position* keep, int segment, int position) {
  Result<void> out;
  for (auto it = deflected_.begin(); it != deflected_.end();) {
    const auto& det = it->first;
    const auto sets = [&](const plan::CollectEntry& e) { return e.detector == det && e.deflection; };
    if (keep != nullptr && std::any_of(keep->collect.begin(), keep->collect.end(), sets)) {
      ++it;
      continue;
    }
    auto r = ctx_.spectrometer.set_deflection(det, it->second.original);
    if (!r) {
      if (out) out = fail(r.error());
      ++it;
      continue;
    }
    trace(segment, position, "deflect", det + "=" + plain(*r));
    settle_due_ = true;  // the beam moved on that detector
    it = deflected_.erase(it);
  }
  return out;
}

Result<void> MeasurementEngine::position_magnet(const std::optional<plan::At>& target, double settle_s,
                                                const std::vector<std::string>& protect, int segment, int position) {
  if (!target) return {};  // collect wherever the magnet is
  if (position_ && *position_ == *target) return {};
  for (const auto& d : protect) {
    if (auto r = ctx_.spectrometer.protect(d, true); !r) return r;
    protected_.insert(d);
    trace(segment, position, "protect", d + " on");
  }
  Result<void> r = ctx_.spectrometer.position(*target);
  trace(segment, position, "move", describe(*target));
  if (r) {
    position_ = *target;
    trace(segment, position, "settle", plain(settle_s));
    wait(settle_s);
  } else {
    position_.reset();
  }
  for (const auto& d : protect) {
    auto u = critical("unprotect " + d, [&] { return ctx_.spectrometer.protect(d, false); });
    if (u) {
      protected_.erase(d);
      trace(segment, position, "protect", d + " off");
    }
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
  const auto kind = spec.kind;
  last_reading_ = ctx_.clock.now();
  collector_.begin(std::move(spec), std::move(hook));
  collecting_ = true;

  std::optional<Error> error;
  bool had_gap = false;
  bool ended_by_gap = false;
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
        ended_by_gap = !stopping();
        break;
      }
      reading = std::move(w->reading);
      had_gap = true;
    }
    last_reading_ = ctx_.clock.now();
    note_beams(kind, *reading);
    status = collector_.add(*reading);
    if (ctx_.bus != nullptr)
      ctx_.bus->publish(CountsProgress{in_.run_id, block, collector_.count(), collector_.target()});
    if (had_gap && status == collect::CollectStatus::Running && ctx_.clock.now() >= planned_end) {
      ended_by_gap = true;
      break;
    }
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
  if (ended_by_gap && status == collect::CollectStatus::Running) status = collect::CollectStatus::Complete;
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
    const auto left = std::chrono::duration_cast<pychron::Duration>(deadline - now);
    auto r = ctx_.spectrometer.next_reading(std::min(timeout, left));
    if (r && *r) {
      close_gap(block, (*r)->ts, true);
      return GapWait{GapEnd::Resumed, std::move(**r)};
    }
    if (!r) {
      if (!is_transient(r.error().kind)) {
        close_gap(block, clock.now(), false);
        return fail(r.error());
      }
      silence_error_ = r.error().what;
    }
    // A port that answered sooner than an integration (its link is down) is
    // asked again after one, so the wait is on the clock, not a spin.
    const pychron::Duration step = integration > pychron::Duration::zero() ? integration : std::chrono::seconds(1);
    const auto pause = std::min(step, left) - (clock.now() - now);
    if (pause > pychron::Duration::zero() && !wait(std::chrono::duration<double>(pause).count())) break;
  }
  if (stopping() || clock.now() < max_end) return GapWait{GapEnd::Cap, std::nullopt};
  // max_gap has passed: the acquisition is started again once, if the link is back.
  publish_log(LogLevel::Warn, std::string(to_string(block)) + ": no readings for " +
                                  std::to_string(std::chrono::duration_cast<std::chrono::seconds>(options_.max_gap).count()) +
                                  " s; starting the acquisition again");
  ctx_.spectrometer.stop_acquisition();
  if (auto s = start_acquisition(integration); s) {
    // No further than the cap: the planned end, or the inlet's next move.
    const auto left = std::chrono::duration_cast<pychron::Duration>(cap - clock.now());
    if (left > pychron::Duration::zero()) {
      auto r = ctx_.spectrometer.next_reading(std::min(timeout, left));
      if (r && *r) {
        close_gap(block, (*r)->ts, true);
        return GapWait{GapEnd::Resumed, std::move(**r)};
      }
      if (!r) silence_error_ = r.error().what;
    }
  } else {
    silence_error_ = s.error().what;
  }
  close_gap(block, clock.now(), false);
  return GapWait{GapEnd::NotResumed, std::nullopt};
}

void MeasurementEngine::close_gap(Block block, TimePoint to, bool resumed) {
  if (!gap_from_) return;
  MeasurementGap gap{in_.run_id, block, collector_.seconds(*gap_from_), collector_.seconds(to), resumed,
                     ctx_.clock.now()};
  gap_from_.reset();
  publish_log(resumed ? LogLevel::Info : LogLevel::Warn, describe(gap));
  if (resumed) collector_.mark_gap(gap.to);  // windowed conditionals start again here
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
    if (!r) ctx_.spectrometer.stop_acquisition();  // it may have started after all
  }
  if (r) integration_ = integration;
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
  const auto& inlet = equilibration().inlet;
  if (inlet.empty()) return {};
  if (auto r = critical("close inlet " + inlet, [&] { return ctx_.valves->close(inlet); }); !r) return r;
  inlet_open_ = false;
  return {};
}

Result<void> MeasurementEngine::check_main_counts() {
  if ((!main_gap_ && !main_cut_) || stopping()) return {};  // a stop is its own outcome
  const auto& main = gas_->segments[main_];
  int planned = 0;
  for (const auto& pos : main.positions)
    if (pos.kind == plan::PositionKind::Signal) planned += scaled(pos.counts);  // after a truncation, what it left
  planned *= main_cycles_ > 0 ? main_cycles_ : main.cycles;  // as a live edit left them
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
  if (time_zero_.kind == plan::TimeZeroKind::OnFirstCount && !collector_.time_zero() && !time_zero_by_step_)
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
  if (auto r = restore_deflections(nullptr, -1, -1); !r)
    publish_log(LogLevel::Error, "could not restore a deflection: " + r.error().what);
  // What a set step left standing goes back, and a beam it blanked is let
  // through again.
  for (auto it = standing_.begin(); it != standing_.end();) {
    if (auto r = ctx_.spectrometer.set_deflection(it->first, it->second.original); r) {
      trace(-1, -1, "deflect", it->first + "=" + plain(*r));
      it = standing_.erase(it);
    } else {
      publish_log(LogLevel::Error, "could not restore the deflection of " + it->first + ": " + r.error().what);
      ++it;
    }
  }
  if (blanked_) {
    if (auto r = ctx_.spectrometer.blank_beam(false); r) blanked_ = false;
    else publish_log(LogLevel::Error, "could not unblank the beam: " + r.error().what);
  }
  // The run's tables (a peak center's write_table) end with it, after the
  // table they may be selected as is let go.
  if (!table_.empty()) {
    if (auto r = ctx_.spectrometer.with_table(""); !r)
      publish_log(LogLevel::Error, "could not go back to the current table: " + r.error().what);
    table_.clear();
  }
  if (ctx_.peak_center != nullptr) ctx_.peak_center->drop_run_tables();
  if (abnormal && inlet_open_ && ctx_.valves != nullptr) {
    const auto& inlet = equilibration().inlet;
    if (auto r = ctx_.valves->close(inlet); r) {
      inlet_open_ = false;
    } else {
      publish_log(LogLevel::Error, "could not close inlet " + inlet + ": " + r.error().what);
    }
  }
}

}  // namespace pychron::experiment::measurement
