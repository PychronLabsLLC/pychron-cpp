#include "pychron/devices/spectrometer/ngx.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

#include "pychron/core/env.hpp"
#include "pychron/devices/spectrometer/legacy/polled_acquirer.hpp"

namespace pychron::spectrometer {

namespace ngx = codec::ngx;

namespace {

// The command text without a terminator: the link appends its own.
Result<std::string> body(Result<codec::Command> cmd) {
  if (!cmd) return fail(std::move(cmd).error());
  return ::pychron::to_string(cmd->tx);
}

std::string format_range(const Range& r) { return std::to_string(r.min) + ".." + std::to_string(r.max); }

const Clock& shared_steady_clock() {
  static const SteadyClock clock;
  return clock;
}

constexpr Range kHvRange{0.0, 10000.0};
constexpr Range kNominalRange{-1e6, 1e6};
// How long past its integration time an armed acquisition may go without
// its completing event before next() gives up and sends StopAcq.
constexpr auto kArmGrace = std::chrono::seconds(5);
// How long trigger() waits for an integration ended elsewhere to be stopped.
constexpr auto kStopWait = std::chrono::seconds(5);

// NGX parameter -> canonical parameter, where one exists; the rest are Custom
// under pychron's name (YFocus, ...). TrapCurrent and EmissionCurrent are only
// read by pychron, so they are read-only here.
struct ParamEntry {
  ngx::Param param;
  std::optional<SourceParam> canonical;
  Unit unit;
  bool writable;
};
const std::vector<ParamEntry>& param_table() {
  static const std::vector<ParamEntry> table{
      {ngx::Param::IonEnergy, SourceParam::HV, Unit::Volts, true},
      {ngx::Param::YFocus, std::nullopt, Unit::None, true},
      {ngx::Param::YBias, std::nullopt, Unit::None, true},
      {ngx::Param::ZFocus, SourceParam::ZFocus, Unit::None, true},
      {ngx::Param::ZBias, std::nullopt, Unit::None, true},
      {ngx::Param::ElectronEnergy, SourceParam::ElectronEnergy, Unit::ElectronVolts, true},
      {ngx::Param::IonRepeller, SourceParam::IonRepeller, Unit::Volts, true},
      {ngx::Param::TrapVoltage, SourceParam::TrapVoltage, Unit::Volts, true},
      {ngx::Param::FilamentCurrent, std::nullopt, Unit::None, true},
      {ngx::Param::FilamentVoltage, std::nullopt, Unit::None, true},
      {ngx::Param::TrapCurrent, SourceParam::TrapCurrent, Unit::MicroAmps, false},
      {ngx::Param::EmissionCurrent, SourceParam::Emission, Unit::MicroAmps, false},
      {ngx::Param::ConfinementVoltage, std::nullopt, Unit::None, true},
      {ngx::Param::ESAPlus, SourceParam::ESAPlus, Unit::Volts, true},
      {ngx::Param::ESAMinus, SourceParam::ESAMinus, Unit::Volts, true},
  };
  return table;
}

ParamId id_of(const ParamEntry& e) {
  if (e.canonical) return *e.canonical;
  return Custom{std::string(ngx::name(e.param))};
}

std::vector<ParamSpec> source_specs() {
  std::vector<ParamSpec> out;
  for (const auto& e : param_table()) {
    const bool hv = e.canonical == SourceParam::HV;
    out.push_back({id_of(e), e.unit, hv ? kHvRange : kNominalRange, true, e.writable, std::string(ngx::mnemonic(e.param))});
  }
  return out;
}

// Session options shared by both drivers: terminator, credentials, timeouts.
Result<NgxLinkOptions> session_options(const toml::table& o) {
  NgxLinkOptions s;
  s.send_terminator = o["send_terminator"].value_or(s.send_terminator);
  if (s.send_terminator.empty()) return fail(ErrorKind::Config, "send_terminator must not be empty");
  s.user = o["user"].value_or(std::string{});
  if (auto env = o["password_env"].value<std::string>()) {
    auto value = env_var(env->c_str());
    if (!value) return fail(ErrorKind::Config, "password_env: " + *env + " is not set");
    s.password = *value;
  } else {
    s.password = o["password"].value_or(std::string{});
  }
  if (!s.user.empty() && s.password.empty())
    return fail(ErrorKind::Config, "user is set but there is no password (password_env, or password in *.local.toml)");
  if (auto ms = o["command_timeout_ms"].value<std::int64_t>()) {
    if (*ms < 1) return fail(ErrorKind::Config, "command_timeout_ms must be positive");
    s.command_timeout = std::chrono::milliseconds(*ms);
  }
  return s;
}

const std::vector<ConfigKey>& session_keys() {
  static const std::vector<ConfigKey> keys{
      {"link", KeyType::String, false, "name of the shared NGX link; default: the driver name"},
      {"send_terminator", KeyType::String, false, R"(appended to every command; default "\r" (bring-up item))"},
      {"user", KeyType::String, false, "NGX login user; no Login is sent when empty"},
      {"password_env", KeyType::String, false, "environment variable holding the login password"},
      {"password", KeyType::String, false, "login password; put it in the machine's *.local.toml only"},
      {"command_timeout_ms", KeyType::Integer, false, "reply timeout per command; default 2000"},
  };
  return keys;
}

}  // namespace

// --- NgxSpectrometer ----------------------------------------------------------

const std::vector<int>& NgxSpectrometer::integration_times() {
  static const std::vector<int> times{1, 2, 3, 4, 5, 10, 20, 100};
  return times;
}

NgxSpectrometer::NgxSpectrometer(std::string name, NgxLinkHandle link, NgxOptions options, const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      link_(std::move(link)),
      options_(std::move(options)),
      clock_(clock != nullptr ? *clock : steady_),
      params_(source_specs()),
      wire_order_(clock_) {
  if (auto l = link_.get()) {
    (*l)->set_event_sink([this](const ngx::AcqFrame& f, TimePoint at, std::uint64_t session) { on_event(f, at, session); });
  }
}

NgxSpectrometer::~NgxSpectrometer() {
  // Waits out an event being delivered, so none reaches a destroyed driver.
  if (auto l = link_.get()) (*l)->set_event_sink({});
}

DriverSchema NgxSpectrometer::schema() {
  DriverSchema s{"",
                 "Isotopx NGX: magnet (mass), source, acquisition over the NGX link (one connection shared with "
                 "ngx_valves)",
                 {legacy::kRolesKey,
                  {"channels", KeyType::StringArray, true, "detector names in pychron order (ACQ values arrive reversed)"},
                  {"rcs_id", KeyType::String, false, "acquisition id sent with StartAcq; default NOM"},
                  {"completion", KeyType::String, false, "acq_b (default): ACQ.B completes; last_acq: the N-th ACQ does"},
                  {"settle_ms", KeyType::Integer, false, "SetMass settling time in ms; default 500"},
                  {"deflect_threshold", KeyType::Float, false, "mass step above which SetMass deflects; 0 never; default 0.5"},
                  {"limit_min", KeyType::Float, false, "lowest mass; default 0"},
                  {"limit_max", KeyType::Float, false, "highest mass; default 200"}}};
  for (const auto& k : session_keys()) s.keys.push_back(k);
  return s;
}

Result<std::unique_ptr<NgxSpectrometer>> NgxSpectrometer::create(const DriverArgs& args) {
  const auto& o = args.options;
  NgxOptions options;
  auto session = session_options(o);
  if (!session) return fail(std::move(session).error());
  options.session = std::move(*session);
  options.link = o["link"].value_or(args.name);
  auto channels = legacy::parse_channels(args);
  if (!channels) return fail(std::move(channels).error());
  if (channels->empty()) return fail(ErrorKind::Config, "channels: list the detectors");
  options.channels = std::move(*channels);
  options.rcs_id = o["rcs_id"].value_or(options.rcs_id);
  const auto completion = o["completion"].value_or(std::string("acq_b"));
  if (completion == "acq_b") options.completion = NgxOptions::Completion::AcqB;
  else if (completion == "last_acq") options.completion = NgxOptions::Completion::LastAcq;
  else return fail(ErrorKind::Config, "completion: \"" + completion + "\" is not acq_b or last_acq");
  options.settle_ms = static_cast<int>(o["settle_ms"].value_or(std::int64_t{options.settle_ms}));
  if (options.settle_ms < 0) return fail(ErrorKind::Config, "settle_ms must not be negative");
  options.deflect_threshold = o["deflect_threshold"].value_or(options.deflect_threshold);
  if (!(options.deflect_threshold >= 0)) return fail(ErrorKind::Config, "deflect_threshold must not be negative");
  options.limits = {o["limit_min"].value_or(options.limits.min), o["limit_max"].value_or(options.limits.max)};
  if (!std::isfinite(options.limits.min) || !std::isfinite(options.limits.max) || !(options.limits.min < options.limits.max))
    return fail(ErrorKind::Config, "limits " + format_range(options.limits) + " must be a non-empty range");
  const Clock& clock = args.clock != nullptr ? *args.clock : shared_steady_clock();
  auto link = make_ngx_link(args.transport, options.link, options.session, clock);
  if (!link) return fail(std::move(link).error());
  return std::make_unique<NgxSpectrometer>(args.name, std::move(*link), std::move(options), args.clock);
}

Result<std::shared_ptr<NgxLink>> NgxSpectrometer::link() {
  auto l = link_.get();
  if (!l) return l;
  // A borrowed link may appear after construction: attach to it on first use.
  if (!link_.owner() && !(*l)->has_event_sink()) {
    (*l)->set_event_sink([this](const ngx::AcqFrame& f, TimePoint at, std::uint64_t session) { on_event(f, at, session); });
  }
  return l;
}

Result<std::string> NgxSpectrometer::ask(const std::string& command) {
  auto l = link();
  if (!l) return fail(std::move(l).error());
  return (*l)->ask(command);
}

Result<void> NgxSpectrometer::stop_acq() {
  std::lock_guard order(wire_order_);
  auto r = ask(*body(Result<codec::Command>(ngx::stop_acq(""))));
  // The reply is not checked (pychron ignores it); a dead link is still an error.
  if (!r && (r.error().kind == ErrorKind::NotConnected || r.error().kind == ErrorKind::Io)) return fail(r.error());
  return {};
}

Result<void> NgxSpectrometer::connect() {
  auto l = link();
  if (!l) return observe(Result<void>(fail(std::move(l).error())));
  if (auto c = (*l)->connect(); !c) return observe(c);
  if (auto s = stop_acq(); !s) return observe(s);
  auto period = body(ngx::set_acq_period(1000, ""));
  if (!period) return observe(Result<void>(fail(std::move(period).error())));
  auto r = ask(*period);
  return observe(r ? Result<void>{} : Result<void>(fail(std::move(r).error())));
}

void NgxSpectrometer::abort_locked(const std::string& why) {
  if (state_ == State::Idle || state_ == State::Stopping) return;
  state_ = State::Stopping;
  ++stats_.aborted;
  if (!why.empty()) ready_.emplace_back(fail(ErrorKind::Cancelled, why));
  clock_.notify_all(acq_cv_);
}

void NgxSpectrometer::stopped() {
  {
    std::lock_guard lock(acq_mutex_);
    if (state_ == State::Stopping) state_ = State::Idle;
  }
  clock_.notify_all(acq_cv_);
}

void NgxSpectrometer::on_event(const ngx::AcqFrame& frame, TimePoint at, std::uint64_t session) {
  std::lock_guard lock(acq_mutex_);
  if (state_ != State::Armed || session != arm_session_ || frame.rcs_id != options_.rcs_id) {
    ++stats_.dropped_events;
    return;
  }
  bool complete = false;
  if (!frame.baseline) {
    ++acq_count_;
    complete = options_.completion == NgxOptions::Completion::LastAcq && acq_count_ >= seconds_;
  } else {
    complete = options_.completion == NgxOptions::Completion::AcqB;
  }
  if (!complete) return;
  state_ = State::Idle;
  if (frame.values.size() != options_.channels.size()) {
    ready_.emplace_back(fail(ErrorKind::Protocol, "NGX ACQ event has " + std::to_string(frame.values.size()) +
                                                   " values for " + std::to_string(options_.channels.size()) +
                                                   " configured channels"));
  } else {
    Frame out;
    out.ts = at;
    out.seq = ++seq_;
    out.integrated = true;
    out.span = std::chrono::seconds(seconds_);
    for (std::size_t i = 0; i < options_.channels.size(); ++i) out.values.emplace_back(options_.channels[i], frame.values[i]);
    ready_.emplace_back(std::move(out));
    ++stats_.completed;
  }
  clock_.notify_all(acq_cv_);
}

Result<void> NgxSpectrometer::configure(Duration integration) {
  const double s = std::chrono::duration<double>(integration).count();
  if (!(s > 0)) return fail(ErrorKind::Config, "integration time must be positive");
  const auto& times = integration_times();
  const int snapped = *std::min_element(times.begin(), times.end(), [&](int a, int b) {
    return std::abs(a - s) < std::abs(b - s);
  });
  bool was_armed = false;
  {
    std::lock_guard lock(acq_mutex_);
    if (snapped == seconds_) return {};
    seconds_ = snapped;
    was_armed = state_ == State::Arming || state_ == State::Armed;
    abort_locked(was_armed ? "integration time changed" : "");
  }
  if (!was_armed) return {};
  auto sent = stop_acq();
  stopped();
  return observe(sent);
}

Result<void> NgxSpectrometer::start() {
  if (auto s = stop_acq(); !s) return observe(s);
  auto period = body(ngx::set_acq_period(1000, ""));
  if (!period) return fail(std::move(period).error());
  if (auto r = ask(*period); !r) return observe(Result<void>(fail(std::move(r).error())));
  std::lock_guard lock(acq_mutex_);
  running_ = true;
  state_ = State::Idle;
  ready_.clear();
  return {};
}

Result<void> NgxSpectrometer::stop() {
  bool was_armed = false;
  {
    std::lock_guard lock(acq_mutex_);
    running_ = false;
    was_armed = state_ != State::Idle;
    state_ = State::Idle;
    ready_.clear();
    clock_.notify_all(acq_cv_);
  }
  if (was_armed) return observe(stop_acq());
  return {};
}

Result<void> NgxSpectrometer::trigger() {
  int seconds = 0;
  {
    std::unique_lock lock(acq_mutex_);
    // An integration ended here is still being stopped: StartAcq now would be E43.
    const TimePoint until = clock_.now() + kStopWait;
    while (state_ == State::Stopping && clock_.now() < until) clock_.wait_until(acq_cv_, lock, until);
    if (!running_) return fail(ErrorKind::Config, "acquirer not started");
    if (state_ == State::Stopping) return fail(ErrorKind::Timeout, "the previous integration is still being stopped");
    if (state_ != State::Idle) return {};  // never a second StartAcq (E43)
    state_ = State::Arming;
    acq_count_ = 0;
    seconds = seconds_;
  }
  auto l = link();
  std::uint64_t session = l ? (*l)->session() : 0;
  auto cmd = body(ngx::start_acq(seconds, options_.rcs_id, ""));
  Result<std::string> r = fail(ErrorKind::Config, "");
  {
    std::lock_guard order(wire_order_);
    {
      // Aborted before StartAcq went out: its StopAcq has been sent (or waits
      // for this lock), and nothing must start after it.
      std::lock_guard lock(acq_mutex_);
      if (state_ != State::Arming) return {};
    }
    r = cmd ? ask(*cmd) : Result<std::string>(fail(cmd.error()));
  }
  if (l) session = std::max(session, (*l)->session());
  std::lock_guard lock(acq_mutex_);
  if (state_ != State::Arming) return {};  // stopped or moved meanwhile
  if (!r) {
    state_ = State::Idle;
    return observe(Result<void>(fail(std::move(r).error())));
  }
  state_ = State::Armed;
  arm_session_ = session;
  armed_at_ = clock_.now();
  ++stats_.armed;
  return {};
}

Result<std::optional<Frame>> NgxSpectrometer::next(Duration timeout) {
  using Next = Result<std::optional<Frame>>;
  std::unique_lock lock(acq_mutex_);
  if (!running_) return observe(Next(fail(ErrorKind::Config, "acquirer not started")));
  const TimePoint deadline = clock_.now() + timeout;
  while (ready_.empty()) {
    if (!running_) return std::optional<Frame>{};
    if (state_ == State::Armed && clock_.now() > armed_at_ + std::chrono::seconds(seconds_) + kArmGrace) {
      state_ = State::Stopping;
      ++stats_.aborted;
      lock.unlock();
      (void)stop_acq();
      stopped();
      return observe(Next(fail(ErrorKind::Timeout, "NGX integration did not complete (no " +
                                                       std::string(options_.completion == NgxOptions::Completion::AcqB
                                                                       ? "ACQ.B"
                                                                       : "final ACQ") +
                                                       " event)")));
    }
    if (clock_.now() >= deadline) return std::optional<Frame>{};
    clock_.wait_until(acq_cv_, lock, clock_.now() + std::chrono::milliseconds(10));
  }
  auto front = std::move(ready_.front());
  ready_.pop_front();
  if (!front) return observe(Next(fail(std::move(front).error())));
  return observe(Next(std::optional<Frame>(std::move(*front))));
}

NgxSpectrometer::AcqStats NgxSpectrometer::acq_stats() const {
  std::lock_guard lock(acq_mutex_);
  return stats_;
}

Result<void> NgxSpectrometer::set(double mass) {
  if (!std::isfinite(mass) || mass < options_.limits.min || mass > options_.limits.max)
    return fail(ErrorKind::Config, "mass " + std::to_string(mass) + " is outside " + format_range(options_.limits));
  bool was_armed = false;
  std::optional<double> previous;
  {
    std::lock_guard lock(acq_mutex_);
    was_armed = state_ == State::Arming || state_ == State::Armed;
    abort_locked("aborted by magnet move");
    previous = mass_;
  }
  if (was_armed) {
    auto s = stop_acq();
    stopped();
    if (!s) return observe(s);
  }
  const bool deflect = options_.deflect_threshold > 0 && previous && std::abs(mass - *previous) > options_.deflect_threshold;
  auto cmd = body(ngx::set_mass(mass, options_.settle_ms, deflect, ""));
  if (!cmd) return fail(std::move(cmd).error());
  auto r = ask(*cmd);
  if (!r) return observe(Result<void>(fail(std::move(r).error())));
  std::lock_guard lock(acq_mutex_);
  mass_ = mass;
  return observe(Result<void>{});
}

Result<double> NgxSpectrometer::read() {
  {
    std::lock_guard lock(acq_mutex_);
    if (state_ != State::Idle) {
      if (mass_) return *mass_;
      return fail(ErrorKind::Config, "no mass commanded yet, and GETMASS would end the integration in progress");
    }
  }
  auto r = ask(*body(Result<codec::Command>(ngx::get_mass(""))));
  if (!r) return observe(Result<double>(fail(std::move(r).error())));
  return observe(ngx::decode_mass(to_bytes(*r + "\n")));
}

namespace {

// The NGX parameter `id` names: a canonical id, or Custom with pychron's name
// ("YFocus") or the mnemonic ("YF").
const ParamEntry* entry_for(const ParamId& id) {
  for (const auto& e : param_table()) {
    if (id_of(e) == id) return &e;
    if (const auto* c = std::get_if<Custom>(&id)) {
      if (c->name == ngx::name(e.param) || c->name == ngx::mnemonic(e.param)) return &e;
    }
  }
  return nullptr;
}

}  // namespace

Result<void> NgxSpectrometer::set_param(const ParamId& id, double value) {
  const auto* e = entry_for(id);
  if (e == nullptr) return fail(ErrorKind::Config, "NGX has no source parameter " + to_string(id));
  if (!e->writable) return fail(ErrorKind::Config, to_string(id) + " is read-only on the NGX");
  const Range range = e->canonical == SourceParam::HV ? kHvRange : kNominalRange;
  if (!std::isfinite(value) || value < range.min || value > range.max)
    return fail(ErrorKind::Config, to_string(id) + " " + std::to_string(value) + " is outside " + format_range(range));
  auto cmd = body(ngx::set_source_param(e->param, value, ""));
  if (!cmd) return fail(std::move(cmd).error());
  auto r = ask(*cmd);
  return observe(r ? Result<void>{} : Result<void>(fail(std::move(r).error())));
}

Result<Readback> NgxSpectrometer::read_param(const ParamId& id) {
  const auto* e = entry_for(id);
  if (e == nullptr) return fail(ErrorKind::Config, "NGX has no source parameter " + to_string(id));
  auto cmd = body(ngx::get_source_param(e->param, ""));
  if (!cmd) return fail(std::move(cmd).error());
  auto r = ask(*cmd);
  if (!r) return observe(Result<Readback>(fail(std::move(r).error())));
  auto rb = ngx::decode_source_param(to_bytes(*r + "\n"));
  if (!rb) return observe(Result<Readback>(fail(std::move(rb).error())));
  return observe(Result<Readback>(Readback{rb->setpoint, rb->actual}));
}

Result<void> NgxSpectrometer::set_hv(double volts) { return set_param(SourceParam::HV, volts); }

Result<double> NgxSpectrometer::read_hv() {
  auto rb = read_param(SourceParam::HV);
  if (!rb) return fail(std::move(rb).error());
  return rb->actual.value_or(rb->setpoint);
}

// --- NgxValves ------------------------------------------------------------------

NgxValves::NgxValves(std::string name, NgxLinkHandle link, NgxValvesOptions options)
    : Device(std::move(name)), link_(std::move(link)), options_(std::move(options)) {}

DriverSchema NgxValves::schema() {
  DriverSchema s{"",
                 "Valves on an Isotopx NGX controller's outputs (SAB 1, OpenValve/CloseValve, SAB 0); shares the NGX "
                 "link with isotopx_ngx",
                 {{"status_retries", KeyType::Integer, false,
                   "GetValveStatus answered E00: read again at most this often; default 2"}}};
  for (const auto& k : session_keys()) s.keys.push_back(k);
  return s;
}

Result<std::unique_ptr<NgxValves>> NgxValves::create(const DriverArgs& args) {
  const auto& o = args.options;
  NgxValvesOptions options;
  auto session = session_options(o);
  if (!session) return fail(std::move(session).error());
  options.session = std::move(*session);
  options.link = o["link"].value_or(args.name);
  options.status_retries = static_cast<int>(o["status_retries"].value_or(std::int64_t{options.status_retries}));
  if (options.status_retries < 0 || options.status_retries > 10)
    return fail(ErrorKind::Config, "status_retries must be between 0 and 10");
  const Clock& clock = args.clock != nullptr ? *args.clock : shared_steady_clock();
  auto link = make_ngx_link(args.transport, options.link, options.session, clock);
  if (!link) return fail(std::move(link).error());
  return std::make_unique<NgxValves>(args.name, std::move(*link), std::move(options));
}

Result<void> NgxValves::connect() {
  if (!link_.owner()) return {};
  auto l = link_.get();
  if (!l) return observe(Result<void>(fail(std::move(l).error())));
  return observe((*l)->connect());
}

Result<void> NgxValves::actuate(const ValveAddress& address, bool open) {
  auto l = link_.get();
  if (!l) return observe(Result<void>(fail(std::move(l).error())));
  auto cmd = body(open ? ngx::open_valve(address.value, "") : ngx::close_valve(address.value, ""));
  if (!cmd) return fail(std::move(cmd).error());
  auto sab_on = body(Result<codec::Command>(ngx::sab(true, "")));
  auto sab_off = body(Result<codec::Command>(ngx::sab(false, "")));

  std::lock_guard one_actuation((*l)->valve_mutex());
  if (auto r = (*l)->ask(*sab_on); !r) return observe(Result<void>(fail(std::move(r).error())));
  auto reply = (*l)->ask(*cmd);
  // SAB 0 on every path, whatever the actuation did.
  auto off = (*l)->ask(*sab_off);
  if (!reply) return observe(Result<void>(fail(std::move(reply).error())));
  if (*reply != "E00")
    return observe(Result<void>(fail(ErrorKind::Protocol, "NGX answered \"" + *reply + "\" to " + *cmd + " (expected E00)")));
  if (!off) return observe(Result<void>(fail(std::move(off).error())));
  return observe(Result<void>{});
}

Result<void> NgxValves::open(const ValveAddress& address) { return actuate(address, true); }
Result<void> NgxValves::close(const ValveAddress& address) { return actuate(address, false); }

Result<ValveState> NgxValves::read(const ValveAddress& address) {
  auto l = link_.get();
  if (!l) return observe(Result<ValveState>(fail(std::move(l).error())));
  auto cmd = body(ngx::get_valve_status(address.value, ""));
  if (!cmd) return fail(std::move(cmd).error());
  std::lock_guard serial((*l)->valve_mutex());
  for (int attempt = 0;; ++attempt) {
    auto reply = (*l)->ask(*cmd);
    if (!reply) return observe(Result<ValveState>(fail(std::move(reply).error())));
    if (*reply == "OPEN") return observe(Result<ValveState>(ValveState::Open));
    if (*reply == "CLOSED") return observe(Result<ValveState>(ValveState::Closed));
    // pychron's known oddity: E00 in place of a status. Read again, bounded.
    if (*reply == "E00" && attempt < options_.status_retries) continue;
    return observe(Result<ValveState>(
        fail(ErrorKind::Protocol, "NGX answered \"" + *reply + "\" to " + *cmd + " (expected OPEN or CLOSED)")));
  }
}

REGISTER_DRIVER("isotopx_ngx", NgxSpectrometer);
REGISTER_DRIVER("ngx_valves", NgxValves);

}  // namespace pychron::spectrometer
