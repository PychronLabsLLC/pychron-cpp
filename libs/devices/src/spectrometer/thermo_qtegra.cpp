#include "pychron/devices/spectrometer/thermo_qtegra.hpp"

#include <algorithm>
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

// Task 6 replaces every use with the real source / acquirer role.
Error not_implemented(std::string_view what) {
  return Error{ErrorKind::Config, "thermo_qtegra: " + std::string(what) + " not implemented", {}};
}

}  // namespace

QtegraSpectrometer::QtegraSpectrometer(std::string name, Transport& transport, QtegraOptions options,
                                       const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      transport_(transport),
      options_(std::move(options)),
      clock_(clock != nullptr ? *clock : steady_),
      reconnector_(transport_, clock_) {}

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

// --- IBeamSource (Task 6) ------------------------------------------------------

Result<void> QtegraSpectrometer::set_hv(double) {  // Task 6
  return observe(Result<void>(fail(not_implemented("set_hv"))));
}

Result<double> QtegraSpectrometer::read_hv() {  // Task 6
  return observe(Result<double>(fail(not_implemented("read_hv"))));
}

Result<void> QtegraSpectrometer::set_param(const ParamId&, double) {  // Task 6
  return observe(Result<void>(fail(not_implemented("set_param"))));
}

Result<Readback> QtegraSpectrometer::read_param(const ParamId&) {  // Task 6
  return observe(Result<Readback>(fail(not_implemented("read_param"))));
}

// --- IIntensityAcquirer (Task 6) -----------------------------------------------

Result<void> QtegraSpectrometer::configure(Duration) {  // Task 6
  return observe(Result<void>(fail(not_implemented("configure"))));
}

Result<void> QtegraSpectrometer::start() {  // Task 6
  return observe(Result<void>(fail(not_implemented("start"))));
}

Result<void> QtegraSpectrometer::stop() {  // Task 6
  return observe(Result<void>(fail(not_implemented("stop"))));
}

Result<std::optional<Frame>> QtegraSpectrometer::next(Duration) {  // Task 6
  return observe(Result<std::optional<Frame>>(fail(not_implemented("next"))));
}

REGISTER_DRIVER("thermo_qtegra", QtegraSpectrometer);

}  // namespace pychron::spectrometer
