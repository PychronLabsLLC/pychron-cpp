#include "pychron/sim/spectrometer/sim_drivers.hpp"

#include <algorithm>
#include <cmath>

namespace pychron::sim {

using namespace spectrometer;

namespace {

double seconds(Duration d) { return std::chrono::duration<double>(d).count(); }

const Clock& clock_of(const DriverArgs& args) {
  static const SteadyClock steady;
  return args.clock != nullptr ? *args.clock : steady;
}

std::shared_ptr<BeamModel> beam_of(const DriverArgs& args) {
  std::string key = args.options["beam"].value_or<std::string>("default");
  return BeamModelRegistry::global().acquire(key, clock_of(args));
}

Result<std::vector<ChannelId>> channels_of(const DriverArgs& args, std::vector<ChannelId> fallback) {
  const toml::node* node = args.options.get("channels");
  if (node == nullptr) return fallback;
  const toml::array* array = node->as_array();
  if (array == nullptr || array->empty()) return fail(ErrorKind::Config, "channels: expected a non-empty array of strings", args.name);
  std::vector<ChannelId> out;
  for (const auto& e : *array) {
    auto s = e.value<std::string>();
    if (!s) return fail(ErrorKind::Config, "channels: expected strings", args.name);
    out.push_back(*s);
  }
  return out;
}

Result<Limits> limits_of(const DriverArgs& args) {
  Limits limits{args.options["limit_min"].value_or(0.0), args.options["limit_max"].value_or(10.0)};
  if (!limits.valid()) return fail(ErrorKind::Config, "limit_min must not exceed limit_max", args.name);
  return limits;
}

Result<double> sample_hz_of(const DriverArgs& args, double fallback) {
  double hz = args.options["sample_hz"].value_or(fallback);
  if (!(hz > 0.0)) return fail(ErrorKind::Config, "sample_hz must be positive", args.name);
  return hz;
}

Result<void> check_limits(const Limits& limits, double value) {
  if (!limits.contains(value)) {
    return fail(ErrorKind::Config, "value " + std::to_string(value) + " is outside limits [" +
                                       std::to_string(limits.min) + ", " + std::to_string(limits.max) + "]");
  }
  return {};
}

const ConfigKey kRoles{"roles", KeyType::StringArray, false, "roles this driver plays (informational)"};
const ConfigKey kBeam{"beam", KeyType::String, false, "name of the shared BeamModel; default \"default\""};

}  // namespace

// --- FramePacer ---------------------------------------------------------------

namespace detail {

void FramePacer::start(Duration period) {
  std::scoped_lock lock(mutex_);
  running_ = true;
  period_ = period;
  due_ = clock_.now() + period;
  seq_ = 0;
}

void FramePacer::stop() {
  {
    std::scoped_lock lock(mutex_);
    running_ = false;
  }
  clock_.notify_all(cv_);
}

bool FramePacer::running() const {
  std::scoped_lock lock(mutex_);
  return running_;
}

Duration FramePacer::period() const {
  std::scoped_lock lock(mutex_);
  return period_;
}

std::optional<TimePoint> FramePacer::wait(Duration timeout, std::uint64_t& seq) {
  std::unique_lock lock(mutex_);
  const TimePoint deadline = clock_.now() + timeout;
  while (running_) {
    TimePoint now = clock_.now();
    if (now >= due_) {
      seq = ++seq_;
      due_ = now + period_;
      return now;
    }
    if (now >= deadline) return std::nullopt;
    clock_.wait_until(cv_, lock, std::min(due_, deadline));
  }
  return std::nullopt;
}

SimSourceCore::SimSourceCore(std::shared_ptr<BeamModel> beam, bool hv_only) : beam_(std::move(beam)) {
  auto add = [&](SourceParam id, Unit unit, Range range, const char* vendor) {
    params_.push_back(ParamSpec{ParamId{id}, unit, range, true, true, vendor});
  };
  add(SourceParam::HV, Unit::Volts, {0.0, 10000.0}, "HV");
  if (hv_only) return;
  add(SourceParam::TrapCurrent, Unit::MicroAmps, {0.0, 500.0}, "Trap Current Set");
  add(SourceParam::ExtractionFocus, Unit::Volts, {-200.0, 200.0}, "Extraction Focus Set");
  add(SourceParam::ExtractionSymmetry, Unit::Volts, {-200.0, 200.0}, "Extraction Symmetry Set");
  add(SourceParam::YSymmetry, Unit::Volts, {-200.0, 200.0}, "Y-Symmetry Set");
  add(SourceParam::ZSymmetry, Unit::Volts, {-200.0, 200.0}, "Z-Symmetry Set");
}

Result<void> SimSourceCore::set_hv(double volts) {
  const ParamSpec* spec = find_spec(params_, ParamId{SourceParam::HV});
  if (!spec->range.contains(volts)) {
    return fail(ErrorKind::Config, "HV " + std::to_string(volts) + " is outside its range");
  }
  beam_->set_hv(volts);
  return {};
}

Result<double> SimSourceCore::read_hv() { return beam_->hv(); }

Result<void> SimSourceCore::set_param(const ParamId& id, double value) {
  const ParamSpec* spec = find_spec(params_, id);
  if (spec == nullptr || !spec->writable) {
    return fail(ErrorKind::Config, "parameter " + to_string(id) + " is not writable");
  }
  if (!spec->range.contains(value)) {
    return fail(ErrorKind::Config, "parameter " + to_string(id) + " value " + std::to_string(value) +
                                       " is outside its range");
  }
  beam_->set_source_param(id, value);
  return {};
}

Result<Readback> SimSourceCore::read_param(const ParamId& id) {
  const ParamSpec* spec = find_spec(params_, id);
  if (spec == nullptr || !spec->readable) {
    return fail(ErrorKind::Config, "parameter " + to_string(id) + " is not readable");
  }
  double v = beam_->source_param(id);
  return Readback{v, v};
}

}  // namespace detail

// --- sim_integrated -----------------------------------------------------------

SimIntegrated::SimIntegrated(std::string name, std::shared_ptr<BeamModel> beam, Options options)
    : Device(std::move(name), DeviceOptions{&beam->clock(), 3}),
      beam_(beam),
      options_(std::move(options)),
      source_(beam, false),
      pacer_(beam->clock()) {
  for (const auto& c : options_.channels) beam_->ensure_detector(c);
}

DriverSchema SimIntegrated::schema() {
  return {"",
          "simulated integrated vendor box: all five spectrometer roles on one BeamModel",
          {kRoles,
           kBeam,
           {"channels", KeyType::StringArray, false, "acquirer channels; default H2 H1 AX L1 L2 CDD"},
           {"move_time_ms", KeyType::Integer, false, "moving() stays true this long after set(); default 200"},
           {"native_axis", KeyType::String, false, "dac | field | mass; default dac"},
           {"limit_min", KeyType::Float, false, "lowest legal position; default 0"},
           {"limit_max", KeyType::Float, false, "highest legal position; default 10"}}};
}

Result<std::unique_ptr<SimIntegrated>> SimIntegrated::create(const DriverArgs& args) {
  Options options;
  auto channels = channels_of(args, options.channels);
  if (!channels) return fail(std::move(channels).error());
  options.channels = std::move(*channels);
  auto limits = limits_of(args);
  if (!limits) return fail(std::move(limits).error());
  options.limits = *limits;
  options.move_time = std::chrono::milliseconds(args.options["move_time_ms"].value_or<std::int64_t>(200));
  if (auto axis = args.options["native_axis"].value<std::string>()) {
    auto parsed = parse_axis(*axis);
    if (!parsed) return fail(std::move(parsed).error());
    options.axis = *parsed;
  }
  return std::make_unique<SimIntegrated>(args.name, beam_of(args), std::move(options));
}

Result<void> SimIntegrated::set(double value) {
  auto ok = check_limits(options_.limits, value);
  if (!ok) return observe(std::move(ok));
  beam_->set_magnet(value);
  std::scoped_lock lock(mutex_);
  moving_until_ = beam_->clock().now() + options_.move_time;
  return observe(Result<void>{});
}

Result<double> SimIntegrated::read() { return observe(Result<double>(beam_->magnet())); }

Result<bool> SimIntegrated::moving() {
  std::scoped_lock lock(mutex_);
  return beam_->clock().now() < moving_until_;
}

Duration SimIntegrated::integration() const {
  std::scoped_lock lock(mutex_);
  return integration_;
}

Result<void> SimIntegrated::configure(Duration integration) {
  if (integration <= Duration::zero()) {
    return observe(Result<void>(fail(ErrorKind::Config, "integration must be positive")));
  }
  auto steps = std::max<std::int64_t>(1, std::llround(seconds(integration) / seconds(kSimIntegrationQuantum)));
  Duration snapped = steps * kSimIntegrationQuantum;
  {
    std::scoped_lock lock(mutex_);
    integration_ = snapped;
  }
  if (pacer_.running()) pacer_.start(snapped);
  return observe(Result<void>{});
}

Result<void> SimIntegrated::start() {
  pacer_.start(integration());
  return observe(Result<void>{});
}

Result<void> SimIntegrated::stop() {
  pacer_.stop();
  return observe(Result<void>{});
}

Result<std::optional<Frame>> SimIntegrated::next(Duration timeout) {
  if (!pacer_.running()) {
    return observe(Result<std::optional<Frame>>(fail(ErrorKind::NotConnected, "acquisition not started")));
  }
  Frame frame;
  auto ts = pacer_.wait(timeout, frame.seq);
  if (!ts) return observe(Result<std::optional<Frame>>(std::optional<Frame>{}));
  frame.ts = *ts;
  frame.integrated = true;
  frame.span = pacer_.period();
  for (const auto& c : options_.channels) {
    auto sample = beam_->intensity(c, *ts, frame.span);
    if (!sample) return observe(Result<std::optional<Frame>>(fail(std::move(sample).error())));
    frame.values.emplace_back(c, sample->value);
  }
  return observe(Result<std::optional<Frame>>(std::optional<Frame>(std::move(frame))));
}

Caps SimIntegrated::caps() const {
  return DetectorCap::Gain | DetectorCap::Deflection | DetectorCap::Protect | DetectorCap::CddVoltage;
}

Result<void> SimIntegrated::check_channel(const ChannelId& channel) const {
  if (std::ranges::find(options_.channels, channel) == options_.channels.end()) {
    return fail(ErrorKind::Config, "unknown channel '" + channel + "'");
  }
  return {};
}

Result<void> SimIntegrated::protect(const ChannelId& channel, bool on) {
  if (auto ok = check_channel(channel); !ok) return observe(std::move(ok));
  return observe(beam_->protect(channel, on));
}

Result<void> SimIntegrated::set_deflection(const ChannelId& channel, double value) {
  if (auto ok = check_channel(channel); !ok) return observe(std::move(ok));
  return observe(beam_->set_deflection(channel, value));
}

Result<double> SimIntegrated::read_deflection(const ChannelId& channel) {
  if (auto ok = check_channel(channel); !ok) return observe(Result<double>(fail(std::move(ok).error())));
  auto d = beam_->detector(channel);
  if (!d) return observe(Result<double>(fail(std::move(d).error())));
  return observe(Result<double>(d->deflection));
}

Result<void> SimIntegrated::set_gain(const ChannelId& channel, double value) {
  if (auto ok = check_channel(channel); !ok) return observe(std::move(ok));
  return observe(beam_->set_gain(channel, value));
}

Result<double> SimIntegrated::read_gain(const ChannelId& channel) {
  if (auto ok = check_channel(channel); !ok) return observe(Result<double>(fail(std::move(ok).error())));
  auto d = beam_->detector(channel);
  if (!d) return observe(Result<double>(fail(std::move(d).error())));
  return observe(Result<double>(d->gain));
}

Result<void> SimIntegrated::set_cdd_voltage(const ChannelId& channel, double volts) {
  if (auto ok = check_channel(channel); !ok) return observe(std::move(ok));
  return observe(beam_->set_cdd_voltage(channel, volts));
}

Result<void> SimIntegrated::blank(bool on) {
  beam_->blank(on);
  return observe(Result<void>{});
}

REGISTER_DRIVER("sim_integrated", SimIntegrated);

// --- sim_dac_positioner -------------------------------------------------------

SimDacPositioner::SimDacPositioner(std::string name, std::shared_ptr<BeamModel> beam, Options options)
    : Device(std::move(name), DeviceOptions{&beam->clock(), 3}), beam_(std::move(beam)), options_(options) {
  cached_ = beam_->magnet();
}

DriverSchema SimDacPositioner::schema() {
  return {"",
          "simulated write-only DAC magnet supply; read() returns the cached setpoint",
          {kRoles,
           kBeam,
           {"channel", KeyType::Integer, false, "DAC channel; default 0"},
           {"limit_min", KeyType::Float, false, "lowest legal value; default 0"},
           {"limit_max", KeyType::Float, false, "highest legal value; default 10"}}};
}

Result<std::unique_ptr<SimDacPositioner>> SimDacPositioner::create(const DriverArgs& args) {
  Options options;
  options.channel = static_cast<int>(args.options["channel"].value_or<std::int64_t>(0));
  auto limits = limits_of(args);
  if (!limits) return fail(std::move(limits).error());
  options.limits = *limits;
  return std::make_unique<SimDacPositioner>(args.name, beam_of(args), options);
}

Result<void> SimDacPositioner::set(double value) {
  auto ok = check_limits(options_.limits, value);
  if (!ok) return observe(std::move(ok));
  beam_->set_magnet(value);
  std::scoped_lock lock(mutex_);
  cached_ = value;
  return observe(Result<void>{});
}

Result<double> SimDacPositioner::read() {
  std::scoped_lock lock(mutex_);
  return cached_;
}

REGISTER_DRIVER("sim_dac_positioner", SimDacPositioner);

// --- raw acquirers ------------------------------------------------------------

SimRawAcquirer::SimRawAcquirer(std::string name, std::shared_ptr<BeamModel> beam, std::vector<ChannelId> channels,
                               double sample_hz, bool counts)
    : Device(std::move(name), DeviceOptions{&beam->clock(), 3}),
      beam_(beam),
      channels_(std::move(channels)),
      period_(std::chrono::duration_cast<Duration>(std::chrono::duration<double>(1.0 / sample_hz))),
      counts_(counts),
      pacer_(beam->clock()) {
  for (const auto& c : channels_) beam_->ensure_detector(c);
}

Result<void> SimRawAcquirer::configure(Duration integration) {
  if (integration <= Duration::zero()) {
    return observe(Result<void>(fail(ErrorKind::Config, "integration must be positive")));
  }
  return observe(Result<void>{});
}

Result<void> SimRawAcquirer::start() {
  pacer_.start(period_);
  return observe(Result<void>{});
}

Result<void> SimRawAcquirer::stop() {
  pacer_.stop();
  return observe(Result<void>{});
}

Result<std::optional<Frame>> SimRawAcquirer::next(Duration timeout) {
  if (!pacer_.running()) {
    return observe(Result<std::optional<Frame>>(fail(ErrorKind::NotConnected, "acquisition not started")));
  }
  Frame frame;
  auto ts = pacer_.wait(timeout, frame.seq);
  if (!ts) return observe(Result<std::optional<Frame>>(std::optional<Frame>{}));
  frame.ts = *ts;
  frame.integrated = false;
  frame.span = period_;
  for (const auto& c : channels_) {
    auto sample = beam_->intensity(c, *ts, period_);
    if (!sample) return observe(Result<std::optional<Frame>>(fail(std::move(sample).error())));
    double v = counts_ ? std::round(sample->value * seconds(period_)) : sample->value;
    frame.values.emplace_back(c, v);
  }
  return observe(Result<std::optional<Frame>>(std::optional<Frame>(std::move(frame))));
}

DriverSchema SimAdcBank::schema() {
  return {"",
          "simulated Faraday ADC bank: raw fA frames at sample_hz",
          {kRoles,
           kBeam,
           {"channels", KeyType::StringArray, false, "channel names; default H1"},
           {"sample_hz", KeyType::Float, false, "frame rate; default 100"}}};
}

Result<std::unique_ptr<SimAdcBank>> SimAdcBank::create(const DriverArgs& args) {
  auto channels = channels_of(args, {"H1"});
  if (!channels) return fail(std::move(channels).error());
  auto hz = sample_hz_of(args, 100.0);
  if (!hz) return fail(std::move(hz).error());
  return std::make_unique<SimAdcBank>(args.name, beam_of(args), std::move(*channels), *hz);
}

REGISTER_DRIVER("sim_adc_bank", SimAdcBank);

DriverSchema SimPulseCounter::schema() {
  return {"",
          "simulated pulse counter: raw count frames at sample_hz",
          {kRoles,
           kBeam,
           {"channels", KeyType::StringArray, false, "channel names; default EM"},
           {"sample_hz", KeyType::Float, false, "frame rate; default 10"}}};
}

Result<std::unique_ptr<SimPulseCounter>> SimPulseCounter::create(const DriverArgs& args) {
  auto channels = channels_of(args, {"EM"});
  if (!channels) return fail(std::move(channels).error());
  auto hz = sample_hz_of(args, 10.0);
  if (!hz) return fail(std::move(hz).error());
  return std::make_unique<SimPulseCounter>(args.name, beam_of(args), std::move(*channels), *hz);
}

REGISTER_DRIVER("sim_pulse_counter", SimPulseCounter);

// --- sim_hv_supply ------------------------------------------------------------

SimHvSupply::SimHvSupply(std::string name, std::shared_ptr<BeamModel> beam)
    : Device(std::move(name), DeviceOptions{&beam->clock(), 3}), source_(std::move(beam), true) {}

DriverSchema SimHvSupply::schema() {
  return {"", "simulated HV supply: the only source parameter is HV", {kRoles, kBeam}};
}

Result<std::unique_ptr<SimHvSupply>> SimHvSupply::create(const DriverArgs& args) {
  return std::make_unique<SimHvSupply>(args.name, beam_of(args));
}

REGISTER_DRIVER("sim_hv_supply", SimHvSupply);

}  // namespace pychron::sim
