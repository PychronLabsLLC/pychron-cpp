#include "pychron/devices/spectrometer/thermo_qtegra.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "pychron/devices/spectrometer/legacy/polled_acquirer.hpp"

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

// One spec per canonical name; the codec lists its preferred hardware name first.
std::vector<ParamSpec> source_specs() {
  std::vector<ParamSpec> specs;
  for (const auto& entry : q::param_names()) {
    const auto id = parse_param_id(entry.canonical);
    if (!id || find_spec(specs, *id) != nullptr) continue;
    const bool hv = *id == ParamId{SourceParam::HV};
    specs.push_back({*id, hv ? Unit::Volts : Unit::None, hv ? kHvRange : kNominalRange, true, true,
                     std::string(entry.hardware)});
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
      transport_(transport),
      options_(std::move(options)),
      clock_(clock != nullptr ? *clock : steady_),
      reconnector_(transport_, clock_),
      params_(source_specs()) {}

DriverSchema QtegraSpectrometer::schema() {
  return {"",
          "Thermo Qtegra RemoteControlServer (Argus, Helix): positioner, source, acquirer, detector control, "
          "beam blank on one transport",
          {legacy::kRolesKey,
           {"channels", KeyType::StringArray, false,
            "detector names as Qtegra reports them in GetData; default H2 H1 AX L1 L2 CDD"},
           {"limit_min", KeyType::Float, false, "lower magnet DAC limit in volts; default 0"},
           {"limit_max", KeyType::Float, false, "upper magnet DAC limit in volts; default 10"},
           {"terminator", KeyType::String, false, "write terminator: cr (default), lf or crlf"},
           {"settle_periods", KeyType::Float, false,
            "integration periods to wait after an integration change; default 2"}}};
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
  if (!std::isfinite(options.settle_periods) || options.settle_periods < 0.0) {
    return fail(ErrorKind::Config, "settle_periods must be zero or positive");
  }
  return std::make_unique<QtegraSpectrometer>(args.name, args.transport, std::move(options), args.clock);
}

Result<void> QtegraSpectrometer::handshake() {
  auto cmd = q::get_integration_time(options_.terminator);
  if (!cmd) return fail(std::move(cmd).error());
  auto reply = transport_.exchange(cmd->tx, *cmd->reply);
  if (!reply) return fail(std::move(reply).error());
  auto seconds = q::decode_number(*reply);
  if (!seconds) return fail(std::move(seconds).error());
  integration_s_.store(*seconds);
  return {};
}

Result<Bytes> QtegraSpectrometer::exchange(Result<codec::Command> cmd) {
  if (!cmd) return fail(std::move(cmd).error());
  return reconnector_.run<Bytes>([&] { return transport_.exchange(cmd->tx, *cmd->reply); },
                                 [this] { return handshake(); });
}

Result<void> QtegraSpectrometer::command_ack(Result<codec::Command> cmd) {
  auto reply = exchange(std::move(cmd));
  if (!reply) return observe(Result<void>(fail(std::move(reply).error())));
  return observe(q::decode_ack(*reply));
}

Result<void> QtegraSpectrometer::command_ok(Result<codec::Command> cmd) {
  auto reply = exchange(std::move(cmd));
  if (!reply) return observe(Result<void>(fail(std::move(reply).error())));
  return observe(q::decode_ok(*reply));
}

Result<double> QtegraSpectrometer::query_number(Result<codec::Command> cmd) {
  auto reply = exchange(std::move(cmd));
  if (!reply) return observe(Result<double>(fail(std::move(reply).error())));
  return observe(q::decode_number(*reply));
}

Result<void> QtegraSpectrometer::check_channel(const ChannelId& channel) const {
  if (std::find(options_.channels.begin(), options_.channels.end(), channel) != options_.channels.end()) return {};
  return fail(ErrorKind::Config, "thermo_qtegra: unknown channel \"" + channel + "\"");
}

Result<void> QtegraSpectrometer::connect() { return observe(handshake()); }

// --- IMassPositioner -----------------------------------------------------------

Result<void> QtegraSpectrometer::set(double value) {
  if (!std::isfinite(value) || !options_.limits.contains(value)) {
    return observe(Result<void>(fail(ErrorKind::Config, "dac " + std::to_string(value) + " is outside limits " +
                                                            format_range(options_.limits))));
  }
  return command_ack(q::set_magnet_dac(value, options_.terminator));
}

Result<double> QtegraSpectrometer::read() { return query_number(q::get_magnet_dac(options_.terminator)); }

Result<bool> QtegraSpectrometer::moving() {
  auto reply = exchange(q::get_magnet_moving(options_.terminator));
  if (!reply) return observe(Result<bool>(fail(std::move(reply).error())));
  return observe(q::decode_bool(*reply));
}

// --- IBeamBlank ----------------------------------------------------------------

Result<void> QtegraSpectrometer::blank(bool on) { return command_ack(q::blank_beam(on, options_.terminator)); }

// --- IDetectorControl ----------------------------------------------------------

Result<void> QtegraSpectrometer::protect(const ChannelId& channel, bool on) {
  if (auto ok = check_channel(channel); !ok) return observe(std::move(ok));
  return command_ack(q::protect_detector(channel, on, options_.terminator));
}

Result<void> QtegraSpectrometer::set_deflection(const ChannelId& channel, double value) {
  if (auto ok = check_channel(channel); !ok) return observe(std::move(ok));
  return command_ack(q::set_deflection(channel, value, options_.terminator));
}

Result<double> QtegraSpectrometer::read_deflection(const ChannelId& channel) {
  if (auto ok = check_channel(channel); !ok) return observe(Result<double>(fail(std::move(ok).error())));
  return query_number(q::get_deflection(channel, options_.terminator));
}

Result<void> QtegraSpectrometer::set_gain(const ChannelId& channel, double value) {
  if (auto ok = check_channel(channel); !ok) return observe(std::move(ok));
  return command_ack(q::set_gain(channel, value, options_.terminator));
}

Result<double> QtegraSpectrometer::read_gain(const ChannelId& channel) {
  if (auto ok = check_channel(channel); !ok) return observe(Result<double>(fail(std::move(ok).error())));
  return query_number(q::get_gain(channel, options_.terminator));
}

// --- IBeamSource ---------------------------------------------------------------

Result<QtegraSpectrometer::ParamTarget> QtegraSpectrometer::param_target(const ParamId& id) const {
  if (const auto* custom = std::get_if<Custom>(&id)) {
    if (q::canonical_name(custom->name)) return ParamTarget{custom->name, nullptr};
  } else if (const ParamSpec* spec = find_spec(params_, id)) {
    return ParamTarget{spec->vendor_name, spec};
  }
  return fail(ErrorKind::Config, "thermo_qtegra: unsupported parameter \"" + to_string(id) + "\"");
}

Result<void> QtegraSpectrometer::set_hv(double volts) {
  if (!std::isfinite(volts) || !kHvRange.contains(volts)) {
    return observe(Result<void>(out_of_range("hv", volts, kHvRange)));
  }
  return command_ok(q::set_hv(volts, options_.terminator));
}

Result<double> QtegraSpectrometer::read_hv() { return query_number(q::get_high_voltage(options_.terminator)); }

Result<void> QtegraSpectrometer::set_param(const ParamId& id, double value) {
  auto target = param_target(id);
  if (!target) return observe(Result<void>(fail(std::move(target).error())));
  if (target->spec != nullptr && (!std::isfinite(value) || !target->spec->range.contains(value))) {
    return observe(Result<void>(out_of_range(to_string(id), value, target->spec->range)));
  }
  return command_ok(q::set_parameter(target->hardware, value, options_.terminator));
}

Result<Readback> QtegraSpectrometer::read_param(const ParamId& id) {
  auto target = param_target(id);
  if (!target) return observe(Result<Readback>(fail(std::move(target).error())));
  auto setpoint = query_number(q::get_parameter(target->hardware, options_.terminator));
  if (!setpoint) return fail(std::move(setpoint).error());
  Readback readback{*setpoint, std::nullopt};
  if (const auto* param = std::get_if<SourceParam>(&id)) {
    if (const auto name = q::readback_name(to_string(*param))) {
      auto actual = query_number(q::get_parameter(*name, options_.terminator));
      if (!actual) return fail(std::move(actual).error());
      readback.actual = *actual;
    }
  }
  return readback;
}

// --- IIntensityAcquirer --------------------------------------------------------

Duration QtegraSpectrometer::period() const { return to_duration(q::snap_integration_time(integration_s_.load())); }

Result<void> QtegraSpectrometer::configure(Duration integration) {
  if (integration <= Duration::zero()) {
    return observe(Result<void>(fail(ErrorKind::Config, "integration time must be positive")));
  }
  const double snapped = q::snap_integration_time(std::chrono::duration<double>(integration).count());
  if (snapped == integration_s_.load()) return {};
  if (auto sent = command_ack(q::set_integration_time(snapped, options_.terminator)); !sent) return sent;
  integration_s_.store(snapped);
  std::lock_guard lock(mutex_);
  due_ = clock_.now() + to_duration(options_.settle_periods * snapped);
  return {};
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
  cv_.notify_all();
  return {};
}

Result<std::optional<Frame>> QtegraSpectrometer::next(Duration timeout) {
  using Next = Result<std::optional<Frame>>;
  const Duration span = period();
  std::uint64_t seq = 0;
  std::uint64_t run = 0;
  {
    std::unique_lock lock(mutex_);
    if (!running_) return observe(Next(fail(ErrorKind::Config, "acquirer not started")));
    // Bounded by clock time and by real time, so a ManualClock nobody
    // advances cannot hang the caller.
    const TimePoint deadline = clock_.now() + timeout;
    const auto real_deadline = std::chrono::steady_clock::now() + timeout;
    while (running_ && clock_.now() < due_) {
      if (clock_.now() >= deadline || std::chrono::steady_clock::now() >= real_deadline) {
        return std::optional<Frame>{};
      }
      clock_.wait_until(cv_, lock, std::min(due_, deadline));
    }
    if (!running_) return std::optional<Frame>{};
    seq = ++seq_;
    run = run_;
    // One period on from the due time, not from now, so the cadence does not
    // drift; more than a period behind, it starts again from now.
    const TimePoint now = clock_.now();
    due_ = now - due_ > span ? now + span : due_ + span;
  }

  // Outside the mutex: stop() must not wait for a transport timeout.
  auto reply = exchange(q::get_data(options_.terminator));
  if (!reply) return observe(Next(fail(std::move(reply).error())));
  auto data = q::decode_data(*reply);
  if (!data) return observe(Next(fail(std::move(data).error())));

  Frame frame;
  frame.ts = clock_.now();
  frame.seq = seq;
  frame.integrated = true;
  frame.span = span;
  // Only the channels the reply names; one it omits is absent, never guessed.
  for (const auto& channel : options_.channels) {
    const auto it = std::find_if(data->begin(), data->end(), [&](const auto& pair) { return pair.first == channel; });
    if (it != data->end()) frame.values.emplace_back(channel, it->second);
  }
  {
    std::lock_guard lock(mutex_);
    if (run != run_) return observe(Next(std::optional<Frame>{}));  // stopped during the read
  }
  return observe(Next(std::optional<Frame>{std::move(frame)}));
}

REGISTER_DRIVER("thermo_qtegra", QtegraSpectrometer);

}  // namespace pychron::spectrometer
