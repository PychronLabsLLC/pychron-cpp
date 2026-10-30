#include "pychron/devices/spectrometer/thermo_qtegra.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string_view>

#include "pychron/devices/spectrometer/legacy/polled_acquirer.hpp"
#include "pychron/transport/link_transport.hpp"

namespace pychron::spectrometer {

namespace q = codec::qtegra;

namespace {

std::string format_range(const Range& r) { return std::to_string(r.min) + ".." + std::to_string(r.max); }

Result<q::Terminator> parse_terminator(const std::string& text) {
  if (text == "cr") return q::Terminator::CR;
  if (text == "lf") return q::Terminator::LF;
  if (text == "crlf") return q::Terminator::CRLF;
  return fail(ErrorKind::Config, "terminator: \"" + text + "\" is not one of cr, lf, crlf");
}

// Accelerating voltage range in volts.
constexpr Range kHvRange{0.0, 10000.0};
// Nominal range for every other source parameter: the real limits are not
// known without an instrument.
constexpr Range kNominalRange{-1e6, 1e6};

Duration to_duration(double seconds) { return std::chrono::round<Duration>(std::chrono::duration<double>(seconds)); }

// settle_periods above this is a configuration mistake, not a settling time.
constexpr double kMaxSettlePeriods = 100.0;
// Bytes of a rejected reply quoted in an error.
constexpr std::size_t kMaxQuotedReply = 120;

// At most one spec per canonical name, from its preferred entry (the codec
// lists it first). Only names pychron Python sends reach the wire: a preferred
// name it does not send is not writable, and is advertised at all only when
// the parameter has a readback name, which then serves as its read name.
std::vector<ParamSpec> source_specs() {
  std::vector<ParamSpec> specs;
  std::vector<std::string_view> seen;
  for (const auto& entry : q::param_names()) {
    if (std::find(seen.begin(), seen.end(), entry.canonical) != seen.end()) continue;
    seen.push_back(entry.canonical);
    const auto id = parse_param_id(entry.canonical);
    if (!id) continue;
    const bool hv = *id == ParamId{SourceParam::HV};
    const Unit unit = hv ? Unit::Volts : Unit::None;
    const Range range = hv ? kHvRange : kNominalRange;
    if (entry.verified) {
      specs.push_back({*id, unit, range, true, true, std::string(entry.hardware)});
    } else if (!entry.readback.empty()) {
      specs.push_back({*id, unit, range, true, false, std::string(entry.readback)});
    }
  }
  return specs;
}

Unexpected<Error> out_of_range(std::string_view what, double value, const Range& range) {
  return fail(ErrorKind::Config, std::string(what) + " " + std::to_string(value) + " is outside " + format_range(range));
}

}  // namespace

QtegraSpectrometer::QtegraSpectrometer(std::string name, Transport& transport, QtegraOptions options,
                                       const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      options_(std::move(options)),
      clock_(clock != nullptr ? *clock : steady_),
      link_(std::make_shared<QtegraLink>(transport, options_.terminator, clock_)),
      params_(source_specs()) {
  link_->set_handshake([this](Transport& t, const q::ServerInfo& server) { return handshake(t, server); });
}

QtegraSpectrometer::QtegraSpectrometer(std::string name, QtegraLinkHandle handle, QtegraOptions options,
                                       const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      options_(std::move(options)),
      clock_(clock != nullptr ? *clock : steady_),
      handle_(std::move(handle)),
      link_(*handle_->get()),
      params_(source_specs()) {
  link_->set_handshake([this](Transport& t, const q::ServerInfo& server) { return handshake(t, server); });
}

// A borrower may still hold the link: stop it calling back into this driver.
QtegraSpectrometer::~QtegraSpectrometer() { link_->set_handshake({}); }

DriverSchema QtegraSpectrometer::schema() {
  return {"",
          "Thermo Qtegra RemoteControlServer (Argus, Helix): positioner, source, acquirer, detector control, "
          "beam blank on one transport",
          {legacy::kRolesKey,
           {"channels", KeyType::StringArray, false,
            "detector names as Qtegra reports them in GetData, each one of the detectors Describe lists; default H2 H1 "
            "AX L1 L2 CDD"},
           {"limit_min", KeyType::Float, false, "lower magnet DAC limit in volts; default 0"},
           {"limit_max", KeyType::Float, false, "upper magnet DAC limit in volts; default 10"},
           {"terminator", KeyType::String, false, "write terminator: cr (default), lf or crlf"},
           {"settle_periods", KeyType::Float, false,
            "integration periods to wait after an integration change, 0 to 100; default 2"},
           {"link", KeyType::String, false,
            "name the shared Qtegra connection is registered under, for qtegra_valves and qtegra_gauges on a "
            "kind = \"link\" transport; default: the driver name"}}};
}

Result<std::unique_ptr<QtegraSpectrometer>> QtegraSpectrometer::create(const DriverArgs& args) {
  const auto& o = args.options;
  QtegraOptions options;
  if (o.contains("channels")) {
    auto channels = legacy::parse_channels(args);
    if (!channels) return fail(std::move(channels).error());
    for (const auto& channel : *channels) {
      if (auto ok = q::validate_name(channel); !ok) {
        return fail(ErrorKind::Config, "channels: \"" + channel + "\": " + ok.error().what);
      }
    }
    options.channels = std::move(*channels);
  }
  options.limits = {o["limit_min"].value_or(options.limits.min), o["limit_max"].value_or(options.limits.max)};
  if (!std::isfinite(options.limits.min) || !std::isfinite(options.limits.max) ||
      !(options.limits.min < options.limits.max)) {
    return fail(ErrorKind::Config, "limits " + format_range(options.limits) + " must be a non-empty range");
  }
  auto terminator = parse_terminator(o["terminator"].value_or(std::string("cr")));
  if (!terminator) return fail(std::move(terminator).error());
  options.terminator = *terminator;
  options.settle_periods = o["settle_periods"].value_or(options.settle_periods);
  if (!std::isfinite(options.settle_periods) || options.settle_periods < 0.0 ||
      options.settle_periods > kMaxSettlePeriods) {
    return fail(ErrorKind::Config, "settle_periods must be between 0 and 100");
  }
  if (dynamic_cast<const LinkTransport*>(&args.transport) != nullptr) {
    return fail(ErrorKind::Config,
                "thermo_qtegra owns its Qtegra connection: give it a tcp transport, not kind = \"link\"");
  }
  static const SteadyClock steady;
  auto handle = make_qtegra_link(args.transport, o["link"].value_or(args.name), options.terminator,
                                 args.clock != nullptr ? *args.clock : steady, policy_of(args));
  if (!handle) return fail(std::move(handle).error());
  return std::make_unique<QtegraSpectrometer>(args.name, std::move(*handle), std::move(options), args.clock);
}

Result<void> QtegraSpectrometer::handshake(Transport& transport, const q::ServerInfo& server) {
  if (auto ok = check_detectors(server); !ok) return ok;
  auto cmd = q::get_integration_time(options_.terminator);
  if (!cmd) return fail(std::move(cmd).error());
  auto reply = transport.exchange(cmd->tx, *cmd->reply);
  if (!reply) return fail(std::move(reply).error());
  auto seconds = q::decode_number(*reply);
  if (!seconds) return fail(std::move(seconds).error());
  integration_s_.store(*seconds);
  return {};
}

Result<void> QtegraSpectrometer::check_detectors(const q::ServerInfo& server) const {
  std::string missing;
  for (const auto& channel : options_.channels) {
    const auto named = [&](const q::Detector& detector) { return detector.name == channel; };
    if (std::ranges::any_of(server.detectors, named)) continue;
    missing += (missing.empty() ? "" : ", ") + channel;
  }
  if (missing.empty()) return {};
  std::string known;
  for (const auto& detector : server.detectors) known += (known.empty() ? "" : ", ") + detector.name;
  return fail(ErrorKind::Config, "thermo_qtegra: channels " + missing + " are not detectors of " + server.instrument +
                                     ", which has: " + known);
}

template <class T>
Result<T> QtegraSpectrometer::command(std::string_view what, Result<codec::Command> cmd,
                                      const std::function<Result<T>(const Bytes&)>& decode) {
  if (!cmd) return observe(begin(what), Result<T>(fail(std::move(cmd).error())));
  const codec::Command& sent = *cmd;
  QtegraLink& link = *link_;
  return call<T>(what,
      &link.link(),
      [&](Resend resend) -> Result<T> {
        auto reply = link.exchange(sent, resend);
        if (!reply) return fail(std::move(reply).error());
        return decode(*reply);
      },
      Repeat::Safe);
}

Result<void> QtegraSpectrometer::command_ack(std::string_view what, Result<codec::Command> cmd) {
  return command<void>(what, std::move(cmd), [](const Bytes& reply) { return q::decode_ack(reply); });
}

Result<void> QtegraSpectrometer::command_ok(std::string_view what, Result<codec::Command> cmd) {
  return command<void>(what, std::move(cmd), [](const Bytes& reply) { return q::decode_ok(reply); });
}

Result<double> QtegraSpectrometer::query_number(std::string_view what, Result<codec::Command> cmd) {
  return command<double>(what, std::move(cmd), [](const Bytes& reply) { return q::decode_number(reply); });
}

Result<void> QtegraSpectrometer::check_channel(const ChannelId& channel) const {
  if (std::find(options_.channels.begin(), options_.channels.end(), channel) != options_.channels.end()) return {};
  return fail(ErrorKind::Config, "thermo_qtegra: unknown channel \"" + channel + "\"");
}

Result<void> QtegraSpectrometer::connect() {
  const Operation op = begin("connect");
  return observe(op, link_->handshake());
}

// --- IMassPositioner -----------------------------------------------------------

Result<void> QtegraSpectrometer::set(double value) {
  const Operation op = begin("set");
  if (!std::isfinite(value) || !options_.limits.contains(value)) {
    return observe(op, Result<void>(fail(ErrorKind::Config, "dac " + std::to_string(value) + " is outside limits " +
                                                                format_range(options_.limits))));
  }
  return command_ack("set", q::set_magnet_dac(value, options_.terminator));
}

Result<double> QtegraSpectrometer::read() { return query_number("read", q::get_magnet_dac(options_.terminator)); }

Result<bool> QtegraSpectrometer::moving() {
  return command<bool>("moving", q::get_magnet_moving(options_.terminator),
                       [](const Bytes& reply) { return q::decode_bool(reply); });
}

// --- IBeamBlank ----------------------------------------------------------------

Result<void> QtegraSpectrometer::blank(bool on) { return command_ack("blank", q::blank_beam(on, options_.terminator)); }

// --- IDetectorControl ----------------------------------------------------------

Result<void> QtegraSpectrometer::protect(const ChannelId& channel, bool on) {
  const Operation op = begin("protect");
  if (auto ok = check_channel(channel); !ok) return observe(op, std::move(ok));
  auto ping = q::ping(options_.terminator);
  if (!ping) return observe(op, Result<void>(fail(std::move(ping).error())));
  auto protect = q::protect_detector(channel, on, options_.terminator);
  if (!protect) return observe(op, Result<void>(fail(std::move(protect).error())));
  QtegraLink& link = *link_;
  // Ping is the Safe part: a connection the server dropped while idle fails
  // there and is repaired, and ProtectDetector then goes out on a connection
  // just shown to be up. A transient failure of that command is Unsafe.
  return call<void>("protect",
      &link.link(),
      [&](Resend resend) -> Result<void> {
        auto alive = link.exchange(*ping, resend);
        if (!alive) return fail(std::move(alive).error());
        if (auto ok = q::decode_ok(*alive); !ok) return ok;
        auto reply = link.exchange(*protect, Resend::Never);
        if (!reply) return fail(outcome_unknown_if_transient(std::move(reply).error()));
        return q::decode_ack(*reply);
      },
      Repeat::Safe);
}

Result<void> QtegraSpectrometer::set_deflection(const ChannelId& channel, double value) {
  const Operation op = begin("set_deflection");
  if (auto ok = check_channel(channel); !ok) return observe(op, std::move(ok));
  return command_ack("set_deflection", q::set_deflection(channel, value, options_.terminator));
}

Result<double> QtegraSpectrometer::read_deflection(const ChannelId& channel) {
  const Operation op = begin("read_deflection");
  if (auto ok = check_channel(channel); !ok) return observe(op, Result<double>(fail(std::move(ok).error())));
  return query_number("read_deflection", q::get_deflection(channel, options_.terminator));
}

Result<void> QtegraSpectrometer::set_gain(const ChannelId& channel, double value) {
  const Operation op = begin("set_gain");
  if (auto ok = check_channel(channel); !ok) return observe(op, std::move(ok));
  return command_ack("set_gain", q::set_gain(channel, value, options_.terminator));
}

Result<double> QtegraSpectrometer::read_gain(const ChannelId& channel) {
  const Operation op = begin("read_gain");
  if (auto ok = check_channel(channel); !ok) return observe(op, Result<double>(fail(std::move(ok).error())));
  return query_number("read_gain", q::get_gain(channel, options_.terminator));
}

// --- IBeamSource ---------------------------------------------------------------

Result<QtegraSpectrometer::ParamTarget> QtegraSpectrometer::param_target(const ParamId& id) const {
  if (const auto* custom = std::get_if<Custom>(&id)) {
    // A custom name stands for the canonical parameter the codec files it
    // under. A name pychron Python never sends is treated as unknown.
    const auto canonical = q::verified_name(custom->name) ? q::canonical_name(custom->name) : std::nullopt;
    if (canonical) {
      const auto param = parse_param_id(*canonical);
      if (const ParamSpec* spec = param ? find_spec(params_, *param) : nullptr) {
        return ParamTarget{custom->name, spec, *param == ParamId{SourceParam::HV},
                           q::readback_name(*canonical) == std::string_view(custom->name)};
      }
    }
  } else if (const ParamSpec* spec = find_spec(params_, id)) {
    // A spec that is not writable is advertised under its readback name.
    return ParamTarget{spec->vendor_name, spec, id == ParamId{SourceParam::HV}, !spec->writable};
  }
  return fail(ErrorKind::Config, "thermo_qtegra: unsupported parameter \"" + to_string(id) + "\"");
}

Result<void> QtegraSpectrometer::set_hv(double volts) {
  const Operation op = begin("set_hv");
  if (!std::isfinite(volts) || !kHvRange.contains(volts)) {
    return observe(op, Result<void>(out_of_range("hv", volts, kHvRange)));
  }
  return command_ok("set_hv", q::set_hv(volts, options_.terminator));
}

Result<double> QtegraSpectrometer::read_hv() {
  return query_number("read_hv", q::get_high_voltage(options_.terminator));
}

Result<void> QtegraSpectrometer::set_param(const ParamId& id, double value) {
  const Operation op = begin("set_param");
  auto target = param_target(id);
  if (!target) return observe(op, Result<void>(fail(std::move(target).error())));
  if (target->readback || !target->spec->writable) {
    return observe(op,
                   Result<void>(fail(ErrorKind::Config, "thermo_qtegra: \"" + target->hardware + "\" is read-only")));
  }
  if (!std::isfinite(value) || !target->spec->range.contains(value)) {
    return observe(op, Result<void>(out_of_range(to_string(id), value, target->spec->range)));
  }
  if (target->hv) return command_ok("set_param", q::set_hv(value, options_.terminator));
  return command_ok("set_param", q::set_parameter(target->hardware, value, options_.terminator));
}

Result<Readback> QtegraSpectrometer::read_param(const ParamId& id) {
  const Operation op = begin("read_param");
  auto target = param_target(id);
  if (!target) return observe(op, Result<Readback>(fail(std::move(target).error())));
  if (target->hv) {
    // Qtegra has one HV read; it serves as both.
    auto volts = read_hv();
    if (!volts) return fail(std::move(volts).error());
    return Readback{*volts, *volts};
  }
  auto setpoint = query_number("read_param", q::get_parameter(target->hardware, options_.terminator));
  if (!setpoint) return fail(std::move(setpoint).error());
  Readback readback{*setpoint, std::nullopt};
  if (target->readback) return Readback{*setpoint, *setpoint};  // one read serves as both
  if (const auto* param = std::get_if<SourceParam>(&id)) {
    if (const auto name = q::readback_name(to_string(*param))) {
      auto actual = query_number("read_param_actual", q::get_parameter(*name, options_.terminator));
      if (!actual) return fail(std::move(actual).error());
      readback.actual = *actual;
    }
  }
  return readback;
}

// --- IIntensityAcquirer --------------------------------------------------------

Duration QtegraSpectrometer::period() const { return to_duration(q::snap_integration_time(integration_s_.load())); }

Result<void> QtegraSpectrometer::configure(Duration integration) {
  const Operation op = begin("configure");
  if (integration <= Duration::zero()) {
    return observe(op, Result<void>(fail(ErrorKind::Config, "integration time must be positive")));
  }
  const double snapped = q::snap_integration_time(std::chrono::duration<double>(integration).count());
  if (snapped == integration_s_.load()) return {};
  // Before the instrument hears of the change: drop any read already in
  // flight and hold frames back until the settle time is known.
  TimePoint previous_due;
  {
    std::lock_guard lock(mutex_);
    ++run_;
    previous_due = due_;
    due_ = TimePoint::max();
  }
  auto sent = command_ack("configure", q::set_integration_time(snapped, options_.terminator));
  {
    std::lock_guard lock(mutex_);
    if (sent) {
      integration_s_.store(snapped);
      due_ = clock_.now() + to_duration(options_.settle_periods * snapped);
    } else {
      due_ = previous_due;
    }
  }
  clock_.notify_all(cv_);
  return sent;
}

Result<void> QtegraSpectrometer::start() {
  std::lock_guard lock(mutex_);
  running_ = true;
  return {};
}

Result<void> QtegraSpectrometer::stop() {
  {
    std::lock_guard lock(mutex_);
    running_ = false;
    ++run_;
  }
  clock_.notify_all(cv_);
  return {};
}

Result<std::optional<Frame>> QtegraSpectrometer::next(Duration timeout) {
  const Operation op = begin("next");
  using Next = Result<std::optional<Frame>>;
  Duration span{};
  std::uint64_t seq = 0;
  std::uint64_t run = 0;
  {
    std::unique_lock lock(mutex_);
    if (!running_) return observe(op, Next(fail(ErrorKind::Config, "acquirer not started")));
    // Bounded by the clock's time alone.
    const TimePoint deadline = clock_.now() + timeout;
    while (running_ && clock_.now() < due_) {
      if (clock_.now() >= deadline) return std::optional<Frame>{};
      clock_.wait_until(cv_, lock, std::min(due_, deadline));  // configure() and stop() notify
    }
    if (!running_) return std::optional<Frame>{};
    seq = ++seq_;
    run = run_;
    span = period();  // after the wait: configure() may have changed it meanwhile
    // One period on from the due time, not from now, so the cadence does not
    // drift; more than a period behind, it starts again from now.
    const TimePoint now = clock_.now();
    due_ = now - due_ > span ? now + span : due_ + span;
  }

  // Outside the mutex: stop() must not wait for a transport timeout. The
  // frame is made inside the call, which observes its outcome once.
  return command<std::optional<Frame>>("next", q::get_data(options_.terminator), [&](const Bytes& reply) -> Next {
    auto data = q::decode_data(reply);
    if (!data) {
      // No scan yet: no frame, as when the period has not come round. The
      // engine's health check reports a scan that never starts.
      if (data.error().code == q::kNoData) return std::optional<Frame>{};
      return fail(std::move(data).error());
    }

    Frame frame;
    frame.ts = clock_.now();
    frame.seq = seq;
    frame.integrated = true;
    frame.span = span;
    // Only the channels the reply names; one it omits is absent, never guessed.
    for (const auto& channel : options_.channels) {
      const auto it =
          std::find_if(data->begin(), data->end(), [&](const auto& pair) { return pair.first == channel; });
      if (it != data->end()) frame.values.emplace_back(channel, it->second);
    }
    {
      std::lock_guard lock(mutex_);
      if (run != run_) return std::optional<Frame>{};  // stopped or reconfigured during the read
    }
    // A reply naming none of them (wrong case, other detector names, untagged
    // values) is not an empty frame: the operator would see a blank plot and
    // no error.
    if (frame.values.empty() && !options_.channels.empty()) {
      const bool truncated = reply.size() > kMaxQuotedReply;
      const auto shown = static_cast<std::ptrdiff_t>(truncated ? kMaxQuotedReply : reply.size());
      const Bytes quoted(reply.begin(), reply.begin() + shown);
      return codec::protocol_error(
          std::string("GetData reply names none of the configured channels") + (truncated ? " (reply truncated)" : ""),
          quoted);
    }
    return std::optional<Frame>{std::move(frame)};
  });
}

REGISTER_DRIVER("thermo_qtegra", QtegraSpectrometer);

}  // namespace pychron::spectrometer
