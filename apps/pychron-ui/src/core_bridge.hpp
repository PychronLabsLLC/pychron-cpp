#pragma once

// CoreBridge (spec section 10.1/10.2): the only QObject that touches the core.
//
// State out: subscribes to the SignalBus; every event, whatever thread it was
// published on, is marshalled to the bridge's (main) thread with a queued
// invokeMethod, folded into the main-thread State, then re-emitted as a Qt
// signal. Widgets read state() and never query the core synchronously.
//
// Commands in: actuate() returns immediately; ExtractionLine::actuate runs on
// one command-executor QThread (so settle waits never block the UI) and its
// Result is posted back as actuationFinished on the main thread.
//
// No core object ever holds a QObject*: the bus handlers reach the bridge only
// through a shared Gate the destructor closes before the bridge goes away.

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QObject>
#include <QString>
#include <QThread>

#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/systems/extraction_line.hpp"

namespace pychron::ui {

class CoreBridge : public QObject {
  Q_OBJECT

 public:
  // Main-thread mirror of everything the core has told the UI.
  struct State {
    std::map<std::string, ValveState> valves;
    std::map<std::string, double> pressures;
    std::map<std::string, std::string> units;
    std::map<std::string, TransportHealth> health;
    std::map<std::string, systems::SwitchInfo> switches;  // lock / owner badges
    std::set<std::string> pending;                        // actuations in flight
  };

  // Actor name the UI actuates as.
  static constexpr const char* kActor = "ui";

  // `line` must outlive the bridge. Construct before line.start() so the
  // start-up Snapshot is seen.
  explicit CoreBridge(systems::ExtractionLine& line, QObject* parent = nullptr);
  ~CoreBridge() override;
  CoreBridge(const CoreBridge&) = delete;
  CoreBridge& operator=(const CoreBridge&) = delete;

  const State& state() const noexcept { return state_; }
  bool pending(const std::string& name) const { return state_.pending.count(name) != 0; }

  // Immutable topology; safe to read from the main thread at any time.
  const canvas::Canvas* canvas() const noexcept { return line_.canvas(); }
  const systems::NetworkGraph* network() const noexcept { return line_.network(); }
  const config::SystemConfig& config() const noexcept { return line_.config(); }

  // Non-blocking. A second request for a valve already pending is rejected
  // at once with a Cancelled result.
  void actuate(const QString& name, systems::SwitchOp op);

  // Software-locks or unlocks a valve or switch. In-memory plus one small
  // state-file write, so it runs on the calling (main) thread. State follows
  // through lockChanged once the core confirms.
  Result<void> set_locked(const QString& name, bool locked);

  // Blocks until every queued command has finished.
  void drain();

 signals:
  void valveChanged(const pychron::ValveChanged& event);
  void pressureSample(const pychron::PressureSample& event);
  void alarm(const pychron::Alarm& event);
  void transportHealth(const pychron::TransportHealth& event);
  void logLine(const pychron::Log& event);
  void snapshot(const pychron::Snapshot& event);
  void lockChanged(const QString& name, bool locked);
  void actuationFailed(const pychron::ActuationFailed& event);
  void actuationStarted(const QString& name);
  void actuationFinished(const QString& name, const pychron::Result<void>& result);

 private:
  struct Gate;

  template <class E>
  void relay(void (CoreBridge::*apply)(const E&));

  void on_valve(const ValveChanged& e);
  void on_pressure(const PressureSample& e);
  void on_alarm(const Alarm& e);
  void on_health(const TransportHealth& e);
  void on_log(const Log& e);
  void on_snapshot(const Snapshot& e);
  void on_lock(const SwitchLockChanged& e);
  void on_failed(const ActuationFailed& e);
  void on_finished(const std::string& name, const Result<void>& result,
                   const std::vector<systems::SwitchInfo>& switches);
  void set_switches(const std::vector<systems::SwitchInfo>& switches);
  void refresh_stats(const std::string& name);

  systems::ExtractionLine& line_;
  State state_;
  std::shared_ptr<Gate> gate_;
  std::vector<SignalBus::Subscription> subscriptions_;
  QThread executor_;
  QObject* worker_ = nullptr;  // lives on executor_
};

}  // namespace pychron::ui
