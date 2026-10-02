#include "pychron/systems/spectrometer/scan_service.hpp"

namespace pychron::spectrometer {

// Shared with the bus handlers (which may outlive the service while an event
// is in flight), so it holds everything they touch and never the service.
struct ScanService::State {
  std::mutex mutex;
  bool closed = false;   // set by the destructor; handlers become no-ops
  bool started = false;  // this service started the engine and it is running
  bool paused = false;
  bool snap_pending = false;  // next fresh reading reports the snapped integration
  Duration integration{};
  std::string error;
  TimePoint start_ts{};  // readings/alarms stamped before this belong to an earlier run
  std::string name;
  const Clock* clock = nullptr;

  ScanStatus status_locked() const {
    ScanStatus s;
    s.spectrometer = name;
    s.running = started;
    s.paused = paused;
    s.integration = integration;
    s.error = error;
    s.ts = clock->now();
    return s;
  }
};

ScanService::ScanService(Spectrometer& spectrometer, SignalBus& bus, const Clock& clock)
    : spectrometer_(spectrometer), bus_(bus), clock_(clock), state_(std::make_shared<State>()) {
  state_->name = spectrometer.name();
  state_->clock = &clock;
  SignalBus* b = &bus;
  readings_ = bus.subscribe<IntensityReading>([st = state_, b](const IntensityReading& e) {
    ScanStatus s;
    {
      std::lock_guard lock(st->mutex);
      if (st->closed || !st->started || !st->snap_pending || e.reading.ts < st->start_ts) return;
      st->snap_pending = false;
      st->integration = e.reading.integration;
      s = st->status_locked();
    }
    b->publish(s);
  });
  alarms_ = bus.subscribe<Alarm>([st = state_, b](const Alarm& a) {
    if (a.source != "acquisition") return;
    ScanStatus s;
    {
      std::lock_guard lock(st->mutex);
      if (st->closed || !st->started || a.ts < st->start_ts) return;
      st->error = a.message;
      s = st->status_locked();
    }
    b->publish(s);
  });
}

ScanService::~ScanService() {
  {
    std::lock_guard lock(state_->mutex);
    state_->closed = true;
  }
  readings_.reset();
  alarms_.reset();
  stop();
}

ScanStatus ScanService::status() const {
  std::lock_guard lock(state_->mutex);
  return state_->status_locked();
}

bool ScanService::running() const {
  std::lock_guard lock(state_->mutex);
  return state_->started;
}

bool ScanService::paused() const {
  std::lock_guard lock(state_->mutex);
  return state_->paused;
}

Duration ScanService::integration() const {
  std::lock_guard lock(state_->mutex);
  return state_->integration;
}

Result<void> ScanService::start_engine(Duration integration, ScanStatus& out) {
  // Mark the scan live before the engine can deliver its first reading.
  {
    std::lock_guard lock(state_->mutex);
    state_->start_ts = clock_.now();
    state_->integration = integration;
    state_->paused = false;
    state_->started = true;
    state_->snap_pending = true;
    state_->error.clear();
  }
  auto r = spectrometer_.acquisition().start(integration);
  std::lock_guard lock(state_->mutex);
  if (!r) {
    state_->started = false;
    state_->snap_pending = false;
    state_->error = to_string(r.error());
  }
  out = state_->status_locked();
  return r;
}

Result<void> ScanService::start(Duration integration) {
  ScanStatus s;
  Result<void> r;
  {
    std::lock_guard op(op_mutex_);
    bool same = false;
    bool restart = false;
    {
      std::lock_guard lock(state_->mutex);
      same = state_->started && state_->integration == integration;
      restart = state_->started;
      if (same) {
        if (state_->error.empty()) return {};
        state_->error.clear();  // cleared: tell subscribers
        s = state_->status_locked();
      }
    }
    if (!same) {
      if (restart) spectrometer_.acquisition().stop();
      r = start_engine(integration, s);
    }
  }
  bus_.publish(s);
  return r;
}

void ScanService::stop() {
  ScanStatus s;
  {
    std::lock_guard op(op_mutex_);
    bool owned = false;
    {
      std::lock_guard lock(state_->mutex);
      owned = state_->started;
      if (!owned && !state_->paused) return;
    }
    if (owned) spectrometer_.acquisition().stop();
    std::lock_guard lock(state_->mutex);
    state_->started = false;
    state_->paused = false;
    state_->snap_pending = false;
    s = state_->status_locked();
  }
  bus_.publish(s);
}

Result<void> ScanService::set_integration(Duration integration) {
  ScanStatus s;
  Result<void> r;
  {
    std::lock_guard op(op_mutex_);
    if (running()) {
      spectrometer_.acquisition().stop();
      r = start_engine(integration, s);
    } else {
      std::lock_guard lock(state_->mutex);
      state_->integration = integration;
      state_->error.clear();
      s = state_->status_locked();
    }
  }
  bus_.publish(s);
  return r;
}

void ScanService::pause() {
  ScanStatus s;
  {
    std::lock_guard op(op_mutex_);
    if (!running()) return;
    spectrometer_.acquisition().stop();
    std::lock_guard lock(state_->mutex);
    state_->started = false;
    state_->paused = true;
    state_->snap_pending = false;
    s = state_->status_locked();
  }
  bus_.publish(s);
}

Result<void> ScanService::resume() {
  ScanStatus s;
  Result<void> r;
  {
    std::lock_guard op(op_mutex_);
    Duration integration{};
    {
      std::lock_guard lock(state_->mutex);
      if (!state_->paused) return {};
      integration = state_->integration;
    }
    r = start_engine(integration, s);
  }
  bus_.publish(s);
  return r;
}

}  // namespace pychron::spectrometer
