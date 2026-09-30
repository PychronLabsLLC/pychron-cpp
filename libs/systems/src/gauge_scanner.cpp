#include "pychron/systems/gauge_scanner.hpp"

#include <sstream>

namespace pychron {

namespace {
const char* unit_name(config::PressureUnits u) {
  switch (u) {
    case config::PressureUnits::Torr: return "torr";
    case config::PressureUnits::Mbar: return "mbar";
    case config::PressureUnits::Pa: return "pa";
  }
  return "";
}
}  // namespace

struct GaugeScanner::State {
  config::GaugeConfig cfg;
  Reader reader;
  bool high = false;
  bool low = false;
  bool failed = false;
};

GaugeScanner::GaugeScanner(Scheduler& scheduler, SignalBus& bus, const Clock& clock)
    : scheduler_(scheduler), bus_(bus), clock_(clock) {}

GaugeScanner::~GaugeScanner() { stop(); }

Result<void> GaugeScanner::add_gauge(const config::GaugeConfig& gauge, IPressureGauge& driver,
                                     Duration interval) {
  return add_gauge(gauge, Reader([&driver] { return driver.read_pressure(); }), interval);
}

Result<void> GaugeScanner::add_gauge(const config::GaugeConfig& gauge, IChannelPressureGauge& driver,
                                     Duration interval) {
  const int channel = static_cast<int>(gauge.channel);
  return add_gauge(gauge, Reader([&driver, channel] { return driver.read_pressure(channel); }),
                   interval);
}

Result<void> GaugeScanner::add_gauge(const config::GaugeConfig& gauge, Reader reader, Duration interval) {
  if (!reader) return fail(ErrorKind::Config, "no reader for gauge", gauge.name);
  auto state = std::make_shared<State>();
  state->cfg = gauge;
  state->reader = std::move(reader);

  // Scheduler guarantees non-overlap per job, so State needs no lock.
  auto task = [this, state] {
    const auto& cfg = state->cfg;
    const TimePoint ts = clock_.now();
    auto r = state->reader();
    if (!r) {
      if (!state->failed) {
        state->failed = true;
        bus_.publish(Alarm{cfg.name, AlarmSeverity::Warning,
                           "read failed: " + to_string(r.error()), ts});
      }
      return;
    }
    state->failed = false;
    const double v = *r;
    bus_.publish(PressureSample{cfg.name, v, unit_name(cfg.units), ts});

    const bool high = cfg.alarm_high && v > *cfg.alarm_high;
    const bool low = cfg.alarm_low && v < *cfg.alarm_low;
    std::ostringstream m;
    if (high && !state->high) {
      m << "pressure " << v << " above alarm_high " << *cfg.alarm_high;
      bus_.publish(Alarm{cfg.name, AlarmSeverity::Critical, m.str(), ts});
    } else if (low && !state->low) {
      m << "pressure " << v << " below alarm_low " << *cfg.alarm_low;
      bus_.publish(Alarm{cfg.name, AlarmSeverity::Warning, m.str(), ts});
    }
    state->high = high;
    state->low = low;
  };

  auto id = scheduler_.every("gauge:" + gauge.name, interval, std::move(task));
  if (!id) return fail(id.error());
  std::lock_guard lock(mutex_);
  jobs_.push_back(*id);
  states_.push_back(std::move(state));
  return {};
}

void GaugeScanner::stop() {
  std::vector<JobId> jobs;
  {
    std::lock_guard lock(mutex_);
    jobs.swap(jobs_);
  }
  for (auto id : jobs) scheduler_.cancel(id);
}

std::size_t GaugeScanner::gauge_count() const {
  std::lock_guard lock(mutex_);
  return states_.size();
}

}  // namespace pychron
