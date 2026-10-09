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

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <QDateTime>
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
    std::map<std::string, double> temperatures;           // cryostat input -> kelvin
    bool started = false;                                 // start()'s Snapshot has arrived
    std::map<std::string, HeaterSample> heaters;          // latest scan or command read-back
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
  bool pending(const std::string& name) const { return state_.pending.contains(name); }

  // Immutable topology; safe to read from the main thread at any time.
  const canvas::Canvas* canvas() const noexcept { return line_.canvas(); }
  const systems::NetworkGraph* network() const noexcept { return line_.network(); }
  const config::SystemConfig& config() const noexcept { return line_.config(); }

  // The time by the line's clock, for a view that shows when, or how long
  // ago, something the line stamped happened: the real time unless the line
  // was given a simulated clock, whose stamps are in its time. Safe from the
  // main thread at any time.
  QDateTime wall_now() const;
  TimePoint now() const { return line_.clock().now(); }

  // Non-blocking. A second request for a valve already pending is rejected
  // at once with a Cancelled result.
  void actuate(const QString& name, systems::SwitchOp op);

  // Software-locks or unlocks a valve or switch. In-memory plus one small
  // state-file write, so it runs on the calling (main) thread. State follows
  // through lockChanged once the core confirms.
  Result<void> set_locked(const QString& name, bool locked);

  // The cryostat's inputs and control loops (empty / 0 without one, or
  // before line.start()). Configuration only: no device I/O.
  std::vector<std::string> cryo_inputs() const;
  int cryo_outputs() const;

  // Non-blocking, on the command executor: sets output `output`'s setpoint,
  // or only reads it back, then posts cryoSetpoint with the setpoint the
  // controller reports (or the error).
  void set_cryo_setpoint(int output, double kelvin);
  void read_cryo_setpoint(int output);

  // Non-blocking, on the command executor: each writes, reads the field
  // back (ExtractionLine), and posts heaterCommandFinished. A fresh
  // HeaterSample follows a success.
  void set_heater_enabled(const QString& name, bool on);
  void set_heater_setpoint(const QString& name, double value);
  void set_heater_pid(const QString& name, bool on);

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
  void temperatureSample(const pychron::TemperatureSample& event);
  // `set`: whether this answers set_cryo_setpoint (a failed set carries its
  // error) or read_cryo_setpoint.
  void cryoSetpoint(int output, bool set, const pychron::Result<double>& setpoint);
  void heaterSample(const pychron::HeaterSample& event);
  void heaterCommandFinished(const QString& name, const pychron::Result<void>& result);

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
  void on_temperature(const TemperatureSample& e);
  void on_heater(const HeaterSample& e);
  void heater_command(const QString& name, std::function<Result<void>(systems::ExtractionLine&)> command);
  void cryo_command(int output, std::optional<double> kelvin);
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
