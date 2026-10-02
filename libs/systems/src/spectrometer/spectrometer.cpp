#include "pychron/systems/spectrometer/spectrometer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>

#include "pychron/systems/spectrometer/field_table_store.hpp"

namespace pychron::spectrometer {

namespace {

constexpr const char* kDevice = "spectrometer";

Unexpected<Error> config_error(std::string what) { return fail(ErrorKind::Config, std::move(what), kDevice); }

// Presents an acquirer's channels as "<driver>:<channel>", the form detector
// `channel` keys use, so two drivers may reuse a channel name.
class PrefixedAcquirer final : public IIntensityAcquirer {
 public:
  PrefixedAcquirer(std::string driver, IIntensityAcquirer& inner) : prefix_(std::move(driver) + ":"), inner_(inner) {}

  std::vector<ChannelId> channels() const override {
    std::vector<ChannelId> out;
    for (auto& c : inner_.channels()) out.push_back(prefix_ + c);
    return out;
  }
  bool integrates() const override { return inner_.integrates(); }
  Result<void> configure(Duration integration) override { return inner_.configure(integration); }
  Result<void> start() override { return inner_.start(); }
  Result<void> stop() override { return inner_.stop(); }
  Result<void> trigger() override { return inner_.trigger(); }
  Result<std::optional<Frame>> next(Duration timeout) override {
    auto frame = inner_.next(timeout);
    if (!frame || !*frame) return frame;
    for (auto& [channel, value] : (*frame)->values) channel = prefix_ + channel;
    return frame;
  }

 private:
  std::string prefix_;
  IIntensityAcquirer& inner_;
};

DetectorKind to_kind(cfg::DetectorKind kind) noexcept {
  switch (kind) {
    case cfg::DetectorKind::Faraday: return DetectorKind::Faraday;
    case cfg::DetectorKind::Counter: return DetectorKind::Counter;
    case cfg::DetectorKind::Cdd: return DetectorKind::Cdd;
    case cfg::DetectorKind::Atona: return DetectorKind::Atona;
  }
  return DetectorKind::Faraday;
}

FitKind to_fit(cfg::FitKind fit) noexcept {
  switch (fit) {
    case cfg::FitKind::Discrete: return FitKind::Discrete;
    case cfg::FitKind::Linear: return FitKind::Linear;
    case cfg::FitKind::Quadratic: return FitKind::Quadratic;
    case cfg::FitKind::Cubic: return FitKind::Cubic;
  }
  return FitKind::Linear;
}

TableAxis to_table_axis(cfg::Axis axis) noexcept {
  switch (axis) {
    case cfg::Axis::Dac: return TableAxis::Dac;
    case cfg::Axis::Field: return TableAxis::Field;
    case cfg::Axis::Mass: return TableAxis::Mass;
  }
  return TableAxis::Dac;
}

TableAxis to_table_axis(IMassPositioner::Axis axis) noexcept {
  switch (axis) {
    case IMassPositioner::Axis::Dac: return TableAxis::Dac;
    case IMassPositioner::Axis::Field: return TableAxis::Field;
    case IMassPositioner::Axis::Mass: return TableAxis::Mass;
  }
  return TableAxis::Dac;
}

// ---- content hash ----

struct Fnv {
  std::uint64_t h = 1469598103934665603ULL;
  void bytes(const void* data, std::size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
      h ^= p[i];
      h *= 1099511628211ULL;
    }
  }
  void str(const std::string& s) {
    u64(s.size());
    bytes(s.data(), s.size());
  }
  void u64(std::uint64_t v) { bytes(&v, sizeof v); }
  void f64(double v) { u64(std::bit_cast<std::uint64_t>(v)); }
  void opt(const std::optional<double>& v) {
    u64(v.has_value() ? 1 : 0);
    if (v) f64(*v);
  }
};

}  // namespace

// ---- conversions ------------------------------------------------------------

DetectorConfig to_detector_config(const cfg::DetectorConfig& d) {
  DetectorConfig out;
  out.name = d.name;
  out.kind = to_kind(d.kind);
  out.channel = d.channel;
  out.units = d.units;
  out.software_gain = d.software_gain;
  out.isotope = d.isotope;
  out.active = d.active;
  if (d.deflection) {
    DeflectionConfig defl;
    defl.control = d.deflection->control;
    defl.correction = d.deflection->correction;
    defl.sign = d.deflection->sign;
    defl.max = d.deflection->max;
    defl.per_volt = d.deflection->per_volt.value_or(0.0);
    out.deflection = std::move(defl);
  }
  if (d.protection) out.protection = ProtectionConfig{d.protection->threshold, d.protection->on_move};
  out.saturation = d.saturation;
  out.dead_time_ns = d.dead_time_ns;
  out.cdd_voltage = d.cdd_voltage;
  return out;
}

FieldTable to_field_table(const cfg::TableFile& table) {
  std::vector<ControlPoint> points;
  points.reserve(table.points.size());
  for (const auto& p : table.points) points.push_back(ControlPoint{p.isotope, p.mass, p.values});
  return FieldTable(to_fit(table.fit), to_table_axis(table.axis), std::move(points));
}

IMassPositioner::Axis to_axis(cfg::Axis axis) noexcept {
  switch (axis) {
    case cfg::Axis::Dac: return IMassPositioner::Axis::Dac;
    case cfg::Axis::Field: return IMassPositioner::Axis::Field;
    case cfg::Axis::Mass: return IMassPositioner::Axis::Mass;
  }
  return IMassPositioner::Axis::Dac;
}

// ---- snapshot ---------------------------------------------------------------

std::uint64_t content_hash(const SpectrometerState& s) {
  Fnv f;
  f.str(s.name);
  f.u64(static_cast<std::uint64_t>(s.axis));
  f.opt(s.magnet);
  f.opt(s.hv);
  f.u64(s.params.size());
  for (const auto& [id, rb] : s.params) {
    f.str(id);
    f.f64(rb.setpoint);
    f.opt(rb.actual);
  }
  f.u64(s.detectors.size());
  for (const auto& d : s.detectors) {
    f.str(d.detector);
    f.u64(d.active ? 1 : 0);
    f.str(d.isotope);
    f.u64(d.protected_ ? 1 : 0);
    f.opt(d.deflection);
    f.opt(d.gain);
    f.opt(d.cdd_voltage);
  }
  f.u64(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(s.integration).count()));
  f.str(s.field_table);
  return f.h;
}

std::string SpectrometerState::hash_hex() const {
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(hash));
  return buf;
}

// ---- construction -----------------------------------------------------------

Spectrometer::Spectrometer(cfg::SpectrometerConfig config, MolecularWeights weights,
                           std::map<std::string, FieldTable> tables, SpectrometerRoles roles,
                           SpectrometerContext context, Options options)
    : config_(std::move(config)),
      weights_(std::move(weights)),
      roles_(std::move(roles)),
      context_(context),
      options_(std::move(options)),
      tables_(std::move(tables)) {}

Spectrometer::~Spectrometer() {
  if (engine_) engine_->stop();
  engine_.reset();
  // Stopped engine first: an in-flight poll may be using a transport.
  for (auto& t : roles_.transports) t->close();
  adapters_.clear();
  roles_.devices.clear();
  roles_.transports.clear();
}

Result<std::unique_ptr<Spectrometer>> Spectrometer::create(cfg::SpectrometerConfig config, MolecularWeights weights,
                                                           std::map<std::string, FieldTable> tables,
                                                           SpectrometerRoles roles, SpectrometerContext context,
                                                           Options options) {
  std::unique_ptr<Spectrometer> s(new Spectrometer(std::move(config), std::move(weights), std::move(tables),
                                                   std::move(roles), context, std::move(options)));
  if (auto built = s->build(); !built) return fail(built.error());
  return s;
}

Result<void> Spectrometer::build() {
  std::vector<std::string> problems;
  if (roles_.positioner == nullptr) problems.push_back("no positioner bound");
  if (roles_.acquirers.empty()) problems.push_back("no acquirer bound");
  axis_ = to_axis(config_.magnet.native_axis);
  if (roles_.positioner != nullptr && roles_.positioner->native_axis() != axis_) {
    problems.push_back("positioner native axis is " + std::string(to_string(roles_.positioner->native_axis())) +
                       " but [magnet].native_axis is " + std::string(cfg::to_string(config_.magnet.native_axis)));
  }
  active_table_ = config_.magnet.field_table;
  if (!tables_.contains(active_table_)) problems.push_back("field table '" + active_table_ + "' is not loaded");
  if (config_.magnet.corrections.hv && axis_ == IMassPositioner::Axis::Mass) {
    problems.push_back("native_axis = \"mass\" cannot enable the HV correction");
  }

  std::vector<DetectorConfig> dets;
  for (const auto& d : config_.detectors) {
    dets.push_back(to_detector_config(d));
    auto ref = cfg::parse_channel_ref(d.channel);
    if (!ref) {
      problems.push_back("detector '" + d.name + "': channel '" + d.channel + "' is not <driver>:<channel>");
      continue;
    }
    local_channel_[d.name] = ref->channel;
  }
  if (!problems.empty()) {
    std::string joined;
    for (const auto& p : problems) joined += (joined.empty() ? "" : "; ") + p;
    return config_error(joined);
  }

  auto set = DetectorSet::create(dets, &context_.clock);
  if (!set) return fail(set.error());
  detectors_.emplace(std::move(*set));
  detectors_->on_change([bus = &context_.bus](const DetectorState& s) { bus->publish(s); });

  std::vector<IIntensityAcquirer*> acquirers;
  for (auto& [driver, acquirer] : roles_.acquirers) {
    if (acquirer == nullptr) return config_error("acquirer '" + driver + "' is null");
    adapters_.push_back(std::make_unique<PrefixedAcquirer>(driver, *acquirer));
    acquirers.push_back(adapters_.back().get());
  }
  AcquisitionEngine::Options engine_options = options_.acquisition;
  engine_options.integration = std::chrono::duration_cast<Duration>(
      std::chrono::duration<double>(config_.system.integration_time_s));
  engine_options.timeout_factor = config_.acquisition.timeout_factor;
  auto engine = AcquisitionEngine::create(std::move(acquirers), std::move(dets), context_.scheduler, context_.bus,
                                          context_.clock, engine_options);
  if (!engine) return fail(engine.error());
  engine_ = std::move(*engine);
  return {};
}

void Spectrometer::sleep(Duration d) const {
  if (options_.sleep) {
    options_.sleep(d);
  } else {
    sleep_on(context_.clock, d);
  }
}

Caps Spectrometer::detector_caps() const {
  return roles_.detector_control != nullptr ? roles_.detector_control->caps() : Caps{};
}

// ---- pipeline ---------------------------------------------------------------

const FieldTable& Spectrometer::table_locked() const { return tables_.at(active_table_); }

Result<const FieldTable*> Spectrometer::table_named_locked(const std::string& name) {
  if (auto it = tables_.find(name); it != tables_.end()) return &it->second;
  if (options_.data_root.empty()) return config_error("field table '" + name + "' is not loaded");
  FieldTableStore store(options_.data_root / "tables", name);
  auto loaded = store.load();
  if (!loaded) return fail(loaded.error());
  auto [it, inserted] = tables_.emplace(name, std::move(*loaded));
  return &it->second;
}

Result<double> Spectrometer::mass_of(const PositionTarget& target) const {
  if (const auto* iso = std::get_if<Isotope>(&target.target)) return weights_.mass(iso->name);
  if (const auto* m = std::get_if<Mass>(&target.target)) return m->amu;
  return config_error("native-unit targets have no mass");
}

Result<double> Spectrometer::hv_locked() {
  const TimePoint now = context_.clock.now();
  if (hv_cache_ && now - hv_cache_->second <= options_.hv_cache) return hv_cache_->first;
  if (roles_.source == nullptr) return config_error("HV correction needs a source");
  auto hv = roles_.source->read_hv();
  if (!hv) return fail(hv.error());
  hv_cache_ = std::make_pair(*hv, now);
  return *hv;
}

Result<CorrectionInputs> Spectrometer::inputs_locked(const DetectorId& det) {
  const auto* d = config_.detector(det);
  if (d == nullptr) return config_error("unknown detector '" + det + "'");
  CorrectionInputs in;
  in.axis = axis_;
  if (config_.magnet.corrections.deflection && d->deflection) {
    in.deflection_enabled = true;
    in.deflection_poly = d->deflection->correction;
    in.deflection_sign = d->deflection->sign;
    auto state = detectors_->state(det);
    if (state && state->deflection) in.deflection = *state->deflection;
  }
  if (config_.magnet.corrections.hv && axis_ == IMassPositioner::Axis::Dac) {
    in.hv_enabled = true;
    in.hv_nominal = config_.source.nominal_hv.value_or(0.0);
    auto hv = hv_locked();
    if (!hv) return fail(hv.error());
    in.hv_actual = *hv;
  }
  return in;
}

Result<double> Spectrometer::correct(double table_value, const DetectorId& det) {
  std::scoped_lock lock(mutex_);
  auto in = inputs_locked(det);
  if (!in) return fail(in.error());
  return spectrometer::correct(table_value, *in);
}

Result<double> Spectrometer::uncorrect(double native, const DetectorId& det) {
  std::scoped_lock lock(mutex_);
  auto in = inputs_locked(det);
  if (!in) return fail(in.error());
  return spectrometer::uncorrect(native, *in);
}

Result<double> Spectrometer::native_for(double mass, const DetectorId& det) {
  std::scoped_lock lock(mutex_);
  auto value = table_locked().value_for(mass, det);
  if (!value) return fail(value.error());
  return correct(*value, det);
}

Result<double> Spectrometer::mass_at(double native, const DetectorId& det) {
  std::scoped_lock lock(mutex_);
  auto value = uncorrect(native, det);
  if (!value) return fail(value.error());
  return table_locked().mass_for(*value, det);
}

std::vector<ChannelId> Spectrometer::plan_protection(double from, double to, ProtectPolicy policy, bool& blank) {
  blank = false;
  if (policy == ProtectPolicy::Never) return {};
  const auto& mp = config_.magnet.protection;
  const bool large = mp.beam_blank_threshold && std::abs(to - from) > *mp.beam_blank_threshold;
  blank = policy == ProtectPolicy::Always || large;

  std::vector<ChannelId> out;
  if (roles_.detector_control == nullptr || !roles_.detector_control->caps().has(DetectorCap::Protect)) return out;
  const double lo = std::min(from, to) - options_.protect_margin;
  const double hi = std::max(from, to) + options_.protect_margin;
  const FieldTable& table = table_locked();
  for (const auto& name : mp.detectors) {
    const auto* d = config_.detector(name);
    if (d == nullptr || !d->protection) continue;
    bool on_path = policy == ProtectPolicy::Always || (d->protection->on_move && large);
    if (!on_path && d->protection->on_move) {
      // A beam above threshold is assumed wherever this detector has a table
      // peak: any control point on the path counts.
      for (const auto& p : table.points()) {
        auto v = p.values.find(name);
        if (v == p.values.end()) continue;
        auto native = correct(v->second, name);
        if (native && *native >= lo && *native <= hi) {
          on_path = true;
          break;
        }
      }
    }
    if (on_path) out.push_back(local_channel_.at(name));
  }
  return out;
}

Result<MoveOutcome> Spectrometer::move_locked(double value, const PositionOptions& options) {
  const Limits limits = roles_.positioner->limits();
  if (limits.valid() && !limits.contains(value)) {
    return config_error("magnet target " + std::to_string(value) + " is outside limits [" +
                        std::to_string(limits.min) + ", " + std::to_string(limits.max) + "]");
  }
  auto from = roles_.positioner->read();
  if (!from) return fail(from.error());

  MovePlan plan;
  plan.from = *from;
  plan.to = value;
  plan.protect = plan_protection(plan.from, plan.to, options.protect, plan.blank);
  const auto& af = config_.magnet.af_demag;
  plan.af_demag = AfDemagSettings{af.enabled,
                                  std::chrono::duration_cast<Duration>(std::chrono::duration<double>(af.period_s)),
                                  std::chrono::duration_cast<Duration>(std::chrono::duration<double>(af.duration_s)),
                                  af.start_amplitude, af.threshold};
  plan.settle = options.settle.value_or(std::chrono::milliseconds(config_.magnet.settle_ms));
  plan.wait_moving = options.wait_moving;
  plan.max_wait = options_.max_wait;
  plan.poll_interval = options_.poll_interval;
  plan.epsilon = options_.epsilon;

  std::map<ChannelId, DetectorId> by_channel;
  for (const auto& [det, ch] : local_channel_) by_channel[ch] = det;
  MoveDeps deps{*roles_.positioner,
                roles_.detector_control,
                roles_.beam_blank,
                context_.clock,
                [this](Duration d) { sleep(d); },
                [this, &by_channel](const ChannelId& ch, bool on) {
                  if (auto it = by_channel.find(ch); it != by_channel.end()) {
                    (void)detectors_->record_protected(it->second, on);
                  }
                }};
  auto outcome = execute_move(plan, deps);
  if (!outcome) return outcome;

  MagnetMoved event{outcome->from, outcome->to, axis_, std::nullopt, outcome->elapsed, context_.clock.now()};
  if (auto m = mass_at(outcome->to, reference_detector())) event.mass_on_reference = *m;
  context_.bus.publish(event);
  return outcome;
}

Result<MoveOutcome> Spectrometer::move_native(double value, PositionOptions options) {
  std::scoped_lock lock(mutex_);
  return move_locked(value, options);
}

Result<PositionResult> Spectrometer::position(const PositionTarget& target, PositionOptions options) {
  std::scoped_lock lock(mutex_);
  PositionResult result;
  if (const auto* n = std::get_if<NativeUnits>(&target.target)) {
    result.native = n->value;
  } else {
    const DetectorId det = target.on.empty() ? reference_detector() : target.on;
    auto mass = mass_of(target);
    if (!mass) return fail(mass.error());
    result.mass = *mass;
    auto value = table_locked().value_for(*mass, det);
    if (!value) return fail(value.error());
    result.table_value = *value;
    auto native = correct(*value, det);
    if (!native) return fail(native.error());
    result.native = *native;
  }
  auto moved = move_locked(result.native, options);
  if (!moved) return fail(moved.error());
  result.move = std::move(*moved);
  return result;
}

Result<PositionResult> Spectrometer::position_hv(double mass, const DetectorId& det, PositionOptions options) {
  return position_hv(PositionTarget{Mass{mass}, det}, options);
}

Result<PositionResult> Spectrometer::position_hv(const PositionTarget& target, PositionOptions options) {
  std::scoped_lock lock(mutex_);
  if (roles_.source == nullptr) return config_error("position_hv needs a source");
  PositionResult result;
  auto before = roles_.source->read_hv();
  if (!before) return fail(before.error());
  if (const auto* n = std::get_if<NativeUnits>(&target.target)) {
    result.native = n->value;
  } else {
    if (config_.magnet.hv_table.empty()) return config_error("no [magnet].hv_table configured");
    auto table = table_named_locked(config_.magnet.hv_table);
    if (!table) return fail(table.error());
    auto mass = mass_of(target);
    if (!mass) return fail(mass.error());
    result.mass = *mass;
    const DetectorId det = target.on.empty() ? reference_detector() : target.on;
    auto value = (*table)->value_for(*mass, det);
    if (!value) return fail(value.error());
    result.table_value = *value;
    result.native = *value;
  }
  const TimePoint start = context_.clock.now();
  if (auto r = roles_.source->set_hv(result.native); !r) return fail(r.error());
  hv_cache_ = std::make_pair(result.native, context_.clock.now());
  if (std::abs(result.native - *before) >= options_.epsilon) {
    sleep(options.settle.value_or(std::chrono::milliseconds(config_.magnet.settle_ms)));
  }
  result.move = MoveOutcome{*before, result.native, context_.clock.now() - start, {}, false, {}};
  return result;
}

Result<double> Spectrometer::magnet_native() {
  std::scoped_lock lock(mutex_);
  return roles_.positioner->read();
}

// ---- tables -----------------------------------------------------------------

const std::string& Spectrometer::active_table() const {
  std::scoped_lock lock(mutex_);
  return active_table_;
}

FieldTable Spectrometer::field_table() const {
  std::scoped_lock lock(mutex_);
  return table_locked();
}

Result<void> Spectrometer::update_table(const DetectorId& det, const std::string& isotope, double table_value,
                                        std::optional<bool> propagate) {
  std::scoped_lock lock(mutex_);
  return tables_.at(active_table_).update(det, isotope, table_value, propagate.value_or(config_.magnet.propagate));
}

Result<std::string> Spectrometer::save_table(std::chrono::system_clock::time_point when) {
  std::scoped_lock lock(mutex_);
  if (options_.data_root.empty()) return config_error("save_table needs a data directory");
  FieldTableStore store(options_.data_root / "tables", active_table_);
  return store.save(table_locked(), when);
}

Result<Spectrometer::TableScope> Spectrometer::with_table(const std::string& name) {
  std::scoped_lock lock(mutex_);
  auto table = table_named_locked(name);
  if (!table) return fail(table.error());
  if ((*table)->axis() != to_table_axis(axis_)) {
    return config_error("table '" + name + "' is in " + std::string(to_string((*table)->axis())) +
                        " units, positioner is " + std::string(to_string(axis_)));
  }
  std::string previous = active_table_;
  active_table_ = name;
  return TableScope(this, std::move(previous));
}

void Spectrometer::restore_table(const std::string& name) {
  std::scoped_lock lock(mutex_);
  active_table_ = name;
}

Spectrometer::TableScope::TableScope(TableScope&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), previous_(std::move(other.previous_)) {}

Spectrometer::TableScope& Spectrometer::TableScope::operator=(TableScope&& other) noexcept {
  if (this != &other) {
    release();
    owner_ = std::exchange(other.owner_, nullptr);
    previous_ = std::move(other.previous_);
  }
  return *this;
}

Spectrometer::TableScope::~TableScope() { release(); }

void Spectrometer::TableScope::release() {
  if (owner_ != nullptr) owner_->restore_table(previous_);
  owner_ = nullptr;
}

// ---- detectors --------------------------------------------------------------

Result<DetectorState> Spectrometer::detector_state(const DetectorId& det) const {
  std::scoped_lock lock(mutex_);
  return detectors_->state(det);
}

Result<void> Spectrometer::set_active(const DetectorId& det, bool active) {
  std::scoped_lock lock(mutex_);
  return detectors_->set_active(det, active);
}

Result<void> Spectrometer::set_isotope(const DetectorId& det, std::string isotope) {
  std::scoped_lock lock(mutex_);
  return detectors_->set_isotope(det, std::move(isotope));
}

Result<ChannelId> Spectrometer::control_channel(const DetectorId& det) const {
  auto it = local_channel_.find(det);
  if (it == local_channel_.end()) return config_error("unknown detector '" + det + "'");
  if (roles_.detector_control == nullptr) return config_error("no detector_control bound");
  return it->second;
}

Result<void> Spectrometer::protect(const DetectorId& det, bool on) {
  std::scoped_lock lock(mutex_);
  auto ch = control_channel(det);
  if (!ch) return fail(ch.error());
  if (!detector_caps().has(DetectorCap::Protect)) return fail(unsupported(DetectorCap::Protect));
  if (auto r = roles_.detector_control->protect(*ch, on); !r) return r;
  return detectors_->record_protected(det, on);
}

Result<double> Spectrometer::set_deflection(const DetectorId& det, double value) {
  std::scoped_lock lock(mutex_);
  auto ch = control_channel(det);
  if (!ch) return fail(ch.error());
  if (!detector_caps().has(DetectorCap::Deflection)) return fail(unsupported(DetectorCap::Deflection));
  const auto* cfg = detectors_->find(det);
  if (cfg->deflection) value = cfg->deflection->clamp(value);
  if (auto r = roles_.detector_control->set_deflection(*ch, value); !r) return fail(r.error());
  if (auto r = detectors_->record_deflection(det, value); !r) return fail(r.error());
  return value;
}

Result<void> Spectrometer::set_gain(const DetectorId& det, double value) {
  std::scoped_lock lock(mutex_);
  auto ch = control_channel(det);
  if (!ch) return fail(ch.error());
  if (!detector_caps().has(DetectorCap::Gain)) return fail(unsupported(DetectorCap::Gain));
  if (auto r = roles_.detector_control->set_gain(*ch, value); !r) return r;
  return detectors_->record_gain(det, value);
}

Result<void> Spectrometer::set_cdd_voltage(const DetectorId& det, double volts) {
  std::scoped_lock lock(mutex_);
  auto ch = control_channel(det);
  if (!ch) return fail(ch.error());
  if (!detector_caps().has(DetectorCap::CddVoltage)) return fail(unsupported(DetectorCap::CddVoltage));
  if (auto r = roles_.detector_control->set_cdd_voltage(*ch, volts); !r) return r;
  return detectors_->record_cdd_voltage(det, volts);
}

// ---- source -----------------------------------------------------------------

Result<void> Spectrometer::set_hv(double volts) {
  std::scoped_lock lock(mutex_);
  if (roles_.source == nullptr) return config_error("no source bound");
  hv_cache_.reset();
  return roles_.source->set_hv(volts);
}

Result<double> Spectrometer::read_hv() {
  std::scoped_lock lock(mutex_);
  if (roles_.source == nullptr) return config_error("no source bound");
  auto hv = roles_.source->read_hv();
  if (hv) hv_cache_ = std::make_pair(*hv, context_.clock.now());
  return hv;
}

Result<std::vector<ParamSpec>> Spectrometer::source_params() const {
  std::scoped_lock lock(mutex_);
  if (roles_.source == nullptr) return config_error("no source bound");
  auto specs = roles_.source->params();
  return std::vector<ParamSpec>(specs.begin(), specs.end());
}

Result<void> Spectrometer::set_param(const ParamId& id, double value) {
  std::scoped_lock lock(mutex_);
  if (roles_.source == nullptr) return config_error("no source bound");
  if (const auto* p = std::get_if<SourceParam>(&id); p != nullptr && *p == SourceParam::HV) hv_cache_.reset();
  return roles_.source->set_param(id, value);
}

Result<Readback> Spectrometer::read_param(const ParamId& id) {
  std::scoped_lock lock(mutex_);
  if (roles_.source == nullptr) return config_error("no source bound");
  return roles_.source->read_param(id);
}

// ---- snapshot ---------------------------------------------------------------

SpectrometerState Spectrometer::snapshot() {
  std::scoped_lock lock(mutex_);
  SpectrometerState s;
  s.name = name();
  s.axis = axis_;
  if (auto m = roles_.positioner->read()) s.magnet = *m;
  if (roles_.source != nullptr) {
    if (auto hv = roles_.source->read_hv()) s.hv = *hv;
    for (const auto& spec : roles_.source->params()) {
      if (!spec.readable) continue;
      if (auto rb = roles_.source->read_param(spec.id)) s.params[to_string(spec.id)] = *rb;
    }
  }
  if (roles_.detector_control != nullptr) {
    const Caps caps = roles_.detector_control->caps();
    for (const auto& d : detectors_->configs()) {
      const ChannelId& ch = local_channel_.at(d.name);
      if (caps.has(DetectorCap::Deflection) && d.deflection && d.deflection->control) {
        if (auto v = roles_.detector_control->read_deflection(ch)) (void)detectors_->record_deflection(d.name, *v);
      }
      if (caps.has(DetectorCap::Gain)) {
        if (auto v = roles_.detector_control->read_gain(ch)) (void)detectors_->record_gain(d.name, *v);
      }
    }
  }
  s.detectors = detectors_->states();
  for (auto& d : s.detectors) d.ts = TimePoint{};
  s.integration = engine_->integration();
  s.field_table = active_table_;
  s.ts = context_.clock.now();
  s.hash = content_hash(s);
  return s;
}

}  // namespace pychron::spectrometer
