#include "pychron/systems/spectrometer/scan_service.hpp"

namespace pychron::spectrometer {

ScanService::ScanService(Spectrometer& spectrometer, SignalBus& bus, const Clock& clock)
    : spectrometer_(spectrometer), bus_(bus), clock_(clock) {
  readings_ = bus_.subscribe<IntensityReading>([this](const IntensityReading&) { on_reading(); });
  alarms_ = bus_.subscribe<Alarm>([this](const Alarm& a) { on_alarm(a); });
}

ScanService::~ScanService() {
  readings_.reset();
  alarms_.reset();
  stop();
}

ScanStatus ScanService::status_locked() const {
  ScanStatus s;
  s.spectrometer = spectrometer_.name();
  s.running = started_;
  s.paused = paused_;
  s.integration = integration_;
  s.error = error_;
  s.ts = clock_.now();
  return s;
}

ScanStatus ScanService::status() const {
  std::lock_guard lock(mutex_);
  return status_locked();
}

bool ScanService::running() const {
  std::lock_guard lock(mutex_);
  return started_;
}

bool ScanService::paused() const {
  std::lock_guard lock(mutex_);
  return paused_;
}

Duration ScanService::integration() const {
  std::lock_guard lock(mutex_);
  return integration_;
}

Result<void> ScanService::start_engine(Duration integration) {
  auto r = spectrometer_.acquisition().start(integration);
  ScanStatus s;
  {
    std::lock_guard lock(mutex_);
    integration_ = integration;
    paused_ = false;
    started_ = r.has_value();
    snap_pending_ = r.has_value();
    error_ = r ? std::string{} : to_string(r.error());
    s = status_locked();
  }
  bus_.publish(s);
  return r;
}

Result<void> ScanService::start(Duration integration) {
  std::lock_guard op(op_mutex_);
  {
    std::lock_guard lock(mutex_);
    if (started_ && integration_ == integration) {
      error_.clear();
      return {};
    }
  }
  if (running()) spectrometer_.acquisition().stop();
  return start_engine(integration);
}

void ScanService::stop() {
  std::lock_guard op(op_mutex_);
  bool owned = false;
  {
    std::lock_guard lock(mutex_);
    owned = started_;
    if (!owned && !paused_) return;
  }
  if (owned) spectrometer_.acquisition().stop();
  ScanStatus s;
  {
    std::lock_guard lock(mutex_);
    started_ = false;
    paused_ = false;
    snap_pending_ = false;
    s = status_locked();
  }
  bus_.publish(s);
}

Result<void> ScanService::set_integration(Duration integration) {
  std::lock_guard op(op_mutex_);
  if (running()) {
    spectrometer_.acquisition().stop();
    return start_engine(integration);
  }
  ScanStatus s;
  {
    std::lock_guard lock(mutex_);
    integration_ = integration;
    error_.clear();
    s = status_locked();
  }
  bus_.publish(s);
  return {};
}

void ScanService::pause() {
  std::lock_guard op(op_mutex_);
  if (!running()) return;
  spectrometer_.acquisition().stop();
  ScanStatus s;
  {
    std::lock_guard lock(mutex_);
    started_ = false;
    paused_ = true;
    snap_pending_ = false;
    s = status_locked();
  }
  bus_.publish(s);
}

Result<void> ScanService::resume() {
  std::lock_guard op(op_mutex_);
  Duration integration{};
  {
    std::lock_guard lock(mutex_);
    if (!paused_) return {};
    integration = integration_;
  }
  return start_engine(integration);
}

void ScanService::on_reading() {
  ScanStatus s;
  {
    std::lock_guard lock(mutex_);
    if (!started_ || !snap_pending_) return;
    snap_pending_ = false;
    integration_ = spectrometer_.acquisition().integration();
    s = status_locked();
  }
  bus_.publish(s);
}

void ScanService::on_alarm(const Alarm& alarm) {
  if (alarm.source != "acquisition") return;
  ScanStatus s;
  {
    std::lock_guard lock(mutex_);
    if (!started_) return;
    error_ = alarm.message;
    s = status_locked();
  }
  bus_.publish(s);
}

}  // namespace pychron::spectrometer
