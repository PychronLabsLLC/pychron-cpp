#include "pychron/systems/jobs/sweep.hpp"

#include <cmath>
#include <thread>

namespace pychron::jobs {

Result<std::vector<double>> sweep_positions(const SweepSpec& spec) {
  if (!std::isfinite(spec.start) || !std::isfinite(spec.stop)) {
    return fail(ErrorKind::Config, "sweep start/stop must be finite", "sweep");
  }
  if (!std::isfinite(spec.step) || spec.step == 0.0) {
    return fail(ErrorKind::Config, "sweep step must be finite and non-zero", "sweep");
  }
  const double step = std::abs(spec.step);
  const double span = std::abs(spec.stop - spec.start);
  // Tolerate floating-point shortfall so a stop on the grid is included.
  const double steps = std::floor(span / step + 1e-9);
  if (steps + 1 > static_cast<double>(Sweep::kMaxPoints)) {
    return fail(ErrorKind::Config, "sweep has too many points", "sweep");
  }
  const double dir = spec.stop >= spec.start ? 1.0 : -1.0;
  const auto n = static_cast<std::size_t>(steps) + 1;
  std::vector<double> xs;
  xs.reserve(spec.bidirectional ? 2 * n - 1 : n);
  for (std::size_t i = 0; i < n; ++i) xs.push_back(spec.start + dir * step * static_cast<double>(i));
  if (spec.bidirectional) {
    for (std::size_t i = n - 1; i-- > 0;) xs.push_back(xs[i]);
  }
  return xs;
}

void Sweep::sleep(Spectrometer& spec, Duration d) const {
  if (d <= Duration::zero()) return;
  if (options_.sleep) {
    options_.sleep(d);
  } else {
    spec.sleep(d);
  }
}

Result<double> Sweep::apply(Spectrometer& spec, const SweepAxis& axis, double x, bool first) {
  switch (axis.kind) {
    case SweepAxis::Kind::Magnet: {
      spectrometer::PositionOptions opts;
      opts.settle = Duration::zero();  // the sweep settles itself
      opts.protect = first ? options_.first_move_protect : spectrometer::ProtectPolicy::Never;
      opts.confirmed = true;
      if (auto r = spec.move_native(x, opts); !r) return fail(r.error());
      return x;
    }
    case SweepAxis::Kind::Hv:
      if (auto r = spec.set_hv(x); !r) return fail(r.error());
      return x;
    case SweepAxis::Kind::Param:
      if (auto r = spec.set_param(axis.param, x); !r) return fail(r.error());
      return x;
    case SweepAxis::Kind::CddVoltage:
      if (auto r = spec.set_cdd_voltage(axis.detector, x); !r) return fail(r.error());
      return x;
    case SweepAxis::Kind::Deflection:
      return spec.set_deflection(axis.detector, x);
  }
  return fail(ErrorKind::Config, "unknown sweep axis", "sweep");
}

namespace {

// Restores the acquisition engine to how the sweep found it.
class IntegrationScope {
 public:
  IntegrationScope(spectrometer::AcquisitionEngine& engine, Duration integration) : engine_(engine) {
    if (integration <= Duration::zero()) return;
    was_running_ = engine_.running();
    previous_ = engine_.integration();
    engine_.stop();
    started_ = engine_.start(integration);
    active_ = true;
  }
  ~IntegrationScope() {
    if (!active_) return;
    engine_.stop();
    if (was_running_) (void)engine_.start(previous_);
  }
  IntegrationScope(const IntegrationScope&) = delete;
  IntegrationScope& operator=(const IntegrationScope&) = delete;

  const Result<void>& started() const { return started_; }

 private:
  spectrometer::AcquisitionEngine& engine_;
  bool active_ = false;
  bool was_running_ = false;
  Duration previous_{};
  Result<void> started_{};
};

}  // namespace

Result<std::vector<SweepPoint>> Sweep::run(Spectrometer& spec, const SweepSpec& sweep, Progress& progress,
                                           CancelToken& cancel) {
  points_.clear();
  auto positions = sweep_positions(sweep);
  if (!positions) return fail(positions.error());

  std::vector<DetectorId> record = sweep.record;
  if (record.empty()) {
    for (const auto& d : spec.detectors().configs()) record.push_back(d.name);
  }
  for (const auto& det : record) {
    if (!spec.detectors().find(det)) return fail(ErrorKind::Config, "unknown detector '" + det + "'", "sweep");
  }

  auto& engine = spec.acquisition();
  IntegrationScope integration(engine, sweep.integration);
  if (!integration.started()) return fail(integration.started().error());
  auto hook = cancel.on_cancel([&engine] { engine.cancel(); });

  const std::size_t total = positions->size();
  progress.report({0, total, "sweep started", std::nullopt});
  for (std::size_t i = 0; i < total; ++i) {
    if (cancel.cancelled()) return fail(ErrorKind::Cancelled, "sweep cancelled", "sweep");
    auto x = apply(spec, sweep.axis, (*positions)[i], i == 0);
    if (!x) return fail(x.error());
    sleep(spec, sweep.settle);

    if (cancel.cancelled()) return fail(ErrorKind::Cancelled, "sweep cancelled", "sweep");
    auto readings = engine.acquire(std::size_t{1});
    if (!readings) return fail(readings.error());
    if (readings->empty()) return fail(ErrorKind::Timeout, "no reading", "sweep");

    const auto& reading = readings->front();
    SweepPoint point;
    point.x = *x;
    point.ts = reading.ts;
    for (const auto& det : record) {
      auto it = reading.values.find(det);
      if (it != reading.values.end() && it->second) point.y[det] = it->second->mean;
    }
    points_.push_back(point);
    progress.report({i + 1, total, {}, std::move(point)});
  }
  return points_;
}

}  // namespace pychron::jobs
