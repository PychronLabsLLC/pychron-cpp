#include "core_bridge.hpp"

#include <mutex>
#include <utility>

#include <QMetaObject>
#include <QPointer>
#include <QSemaphore>

namespace pychron::ui {

namespace {

// Same spelling as the PressureSample units the GaugeScanner publishes.
std::string units_name(config::PressureUnits units) {
  switch (units) {
    case config::PressureUnits::Torr:
      return "torr";
    case config::PressureUnits::Mbar:
      return "mbar";
    case config::PressureUnits::Pa:
      return "pa";
  }
  return {};
}

}  // namespace

// Bus handlers run on scheduler/transport threads and may still be mid-call
// while the bridge is destroyed. They reach the bridge only through the Gate,
// which the destructor closes under the same lock; posting a queued call is
// thread-safe and never runs the bridge's code on the publishing thread.
struct CoreBridge::Gate {
  std::mutex mutex;
  CoreBridge* target = nullptr;
};

template <class E>
void CoreBridge::relay(void (CoreBridge::*apply)(const E&)) {
  subscriptions_.push_back(line_.bus().subscribe<E>([gate = gate_, apply](const E& event) {
    std::lock_guard lock(gate->mutex);
    if (CoreBridge* self = gate->target) {
      QMetaObject::invokeMethod(self, [self, apply, event] { (self->*apply)(event); }, Qt::QueuedConnection);
    }
  }));
}

CoreBridge::CoreBridge(systems::ExtractionLine& line, QObject* parent)
    : QObject(parent), line_(line), gate_(std::make_shared<Gate>()) {
  gate_->target = this;
  relay<ValveChanged>(&CoreBridge::on_valve);
  relay<PressureSample>(&CoreBridge::on_pressure);
  relay<Alarm>(&CoreBridge::on_alarm);
  relay<TransportHealth>(&CoreBridge::on_health);
  relay<Log>(&CoreBridge::on_log);
  relay<Snapshot>(&CoreBridge::on_snapshot);
  relay<ActuationFailed>(&CoreBridge::on_failed);
  relay<SwitchLockChanged>(&CoreBridge::on_lock);

  // Locks and owners are in-memory manager state (no device I/O); seed the
  // badges (and last recorded states) now and refresh them after every
  // command. Snapshots carry no units, so take them from the config.
  set_switches(line_.switches().list());
  for (const auto& [name, info] : state_.switches) {
    state_.valves[name] = info.state;
  }
  for (const auto& g : line_.config().gauges) {
    state_.units[g.name] = units_name(g.units);
  }

  executor_.setObjectName(QStringLiteral("pychron-command-executor"));
  worker_ = new QObject;
  worker_->moveToThread(&executor_);
  connect(&executor_, &QThread::finished, worker_, &QObject::deleteLater);
  executor_.start();
}

CoreBridge::~CoreBridge() {
  {
    std::lock_guard lock(gate_->mutex);
    gate_->target = nullptr;
  }
  subscriptions_.clear();
  executor_.quit();  // finishes the command in flight; queued ones are dropped
  executor_.wait();
}

void CoreBridge::actuate(const QString& qname, systems::SwitchOp op) {
  std::string name = qname.toStdString();
  if (pending(name)) {
    emit actuationFinished(qname, fail(ErrorKind::Cancelled, "actuation already pending", name));
    return;
  }
  state_.pending.insert(name);
  emit actuationStarted(qname);

  auto* line = &line_;
  QPointer<CoreBridge> self(this);
  QMetaObject::invokeMethod(
      worker_,
      [line, self, name, op] {
        Result<void> result = line->actuate(name, op, kActor);
        std::vector<systems::SwitchInfo> switches = line->switches().list();
        // `self` is only dereferenced on the main thread, inside the posted call.
        QMetaObject::invokeMethod(
            self.data(),
            [self, name, result, switches] {
              if (self) {
                self->on_finished(name, result, switches);
              }
            },
            Qt::QueuedConnection);
      },
      Qt::QueuedConnection);
}

Result<void> CoreBridge::set_locked(const QString& name, bool locked) {
  return line_.set_locked(name.toStdString(), locked);
}

void CoreBridge::drain() {
  QSemaphore done;
  QMetaObject::invokeMethod(worker_, [&done] { done.release(); }, Qt::QueuedConnection);
  done.acquire();
}

void CoreBridge::on_valve(const ValveChanged& e) {
  state_.valves[e.valve] = e.state;
  if (auto it = state_.switches.find(e.valve); it != state_.switches.end()) {
    it->second.state = e.state;
  }
  emit valveChanged(e);
}

void CoreBridge::on_pressure(const PressureSample& e) {
  state_.pressures[e.gauge] = e.value;
  state_.units[e.gauge] = e.units;
  emit pressureSample(e);
}

void CoreBridge::on_alarm(const Alarm& e) { emit alarm(e); }

void CoreBridge::on_health(const TransportHealth& e) {
  state_.health[e.transport] = e;
  emit transportHealth(e);
}

void CoreBridge::on_log(const Log& e) { emit logLine(e); }

void CoreBridge::on_snapshot(const Snapshot& e) {
  for (const auto& [name, st] : e.valves) {
    state_.valves[name] = st;
    if (auto it = state_.switches.find(name); it != state_.switches.end()) {
      it->second.state = st;
    }
  }
  for (const auto& [name, value] : e.pressures) {
    state_.pressures[name] = value;
  }
  for (auto& [name, info] : state_.switches) {
    info.locked = e.locked.count(name) != 0;
  }
  emit snapshot(e);
}

void CoreBridge::on_lock(const SwitchLockChanged& e) {
  auto it = state_.switches.find(e.name);
  if (it == state_.switches.end() || it->second.locked == e.locked) {
    return;
  }
  it->second.locked = e.locked;
  emit lockChanged(QString::fromStdString(e.name), e.locked);
}

void CoreBridge::on_failed(const ActuationFailed& e) { emit actuationFailed(e); }

void CoreBridge::on_finished(const std::string& name, const Result<void>& result,
                             const std::vector<systems::SwitchInfo>& switches) {
  state_.pending.erase(name);
  set_switches(switches);
  emit actuationFinished(QString::fromStdString(name), result);
}

void CoreBridge::set_switches(const std::vector<systems::SwitchInfo>& switches) {
  for (const auto& info : switches) {
    state_.switches[info.name] = info;
  }
}

}  // namespace pychron::ui
