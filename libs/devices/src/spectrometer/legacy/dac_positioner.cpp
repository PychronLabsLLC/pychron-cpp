#include "pychron/devices/spectrometer/legacy/dac_positioner.hpp"

#include <cmath>

#include "pychron/codecs/map215.hpp"
#include "pychron/devices/spectrometer/legacy/polled_acquirer.hpp"

namespace pychron::spectrometer {

namespace {

std::string format_range(const Range& r) { return std::to_string(r.min) + ".." + std::to_string(r.max); }

}  // namespace

DacPositioner::DacPositioner(std::string name, std::unique_ptr<IDacOutput> output, Limits limits,
                             const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      output_(std::move(output)),
      limits_(limits),
      cached_(limits.clamp(0.0)) {}

DriverSchema DacPositioner::schema() {
  return {"",
          "legacy write-only magnet DAC (MAP-215 serial); read() is the cached setpoint",
          {legacy::kRolesKey,
           {"protocol", KeyType::String, false, "DAC protocol; only \"map215\" (default)"},
           {"channel", KeyType::Integer, false, "DAC output channel; MAP-215 has one, 0 (default)"},
           {"range", KeyType::Integer, false, "MAP-215 output range selected before each write, 0..9; default 0"},
           {"full_scale", KeyType::Float, false, "volts at full-scale code; default 10"},
           {"min", KeyType::Float, false, "lower travel limit in volts; default 0"},
           {"max", KeyType::Float, false, "upper travel limit in volts; default full_scale"}}};
}

Result<std::unique_ptr<DacPositioner>> DacPositioner::create(const DriverArgs& args) {
  const auto& o = args.options;
  const std::string protocol = o["protocol"].value_or(std::string("map215"));
  if (protocol != "map215") return fail(ErrorKind::Config, "protocol: unknown DAC protocol \"" + protocol + "\"");
  if (auto channel = o["channel"].value_or(std::int64_t{0}); channel != 0) {
    return fail(ErrorKind::Config, "channel: map215 has one output, channel 0 (got " + std::to_string(channel) + ")");
  }
  const auto range = o["range"].value_or(std::int64_t{0});
  if (range < codec::map215::kMinRange || range > codec::map215::kMaxRange) {
    return fail(ErrorKind::Config, "range: " + std::to_string(range) + " is outside 0..9");
  }
  const double full_scale = o["full_scale"].value_or(10.0);
  if (!(full_scale > 0.0) || !std::isfinite(full_scale)) return fail(ErrorKind::Config, "full_scale must be positive");

  auto output = std::make_unique<Map215Dac>(args.transport, static_cast<int>(range), full_scale);
  const Limits limits{o["min"].value_or(0.0), o["max"].value_or(full_scale)};
  const Range span = output->span();
  if (!(limits.min < limits.max) || !span.contains(limits.min) || !span.contains(limits.max)) {
    return fail(ErrorKind::Config, "limits " + format_range(limits) + " must be a non-empty range inside " +
                                       format_range(span) + " V");
  }
  return std::make_unique<DacPositioner>(args.name, std::move(output), limits, args.clock);
}

Result<void> DacPositioner::set(double value) {
  if (!std::isfinite(value) || !limits_.contains(value)) {
    return observe(Result<void>(fail(ErrorKind::Config, "dac " + std::to_string(value) + " is outside limits " +
                                                            format_range(limits_))));
  }
  auto written = output_->write(value);
  if (!written) return observe(Result<void>(fail(std::move(written).error())));
  cached_.store(*written);
  return observe(Result<void>{});
}

Result<double> DacPositioner::read() { return cached_.load(); }

REGISTER_DRIVER("dac_positioner", DacPositioner);

}  // namespace pychron::spectrometer
