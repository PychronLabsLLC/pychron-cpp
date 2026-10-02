#pragma once

// Free-running scan lifecycle for one spectrometer's acquisition engine:
// start/stop/pause/resume and integration changes, with every state change
// published as ScanStatus on the SignalBus. The service only stops an engine
// it started itself.
//
// Thread-safe. Mutating calls are serialised; engine calls and bus publishes
// happen outside the state mutex, so bus handlers (including this service's
// own) never wait on a call that is publishing.

#include <mutex>
#include <string>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"

namespace pychron::spectrometer {

struct ScanStatus {
  std::string spectrometer;
  bool running = false;
  bool paused = false;
  Duration integration{};
  std::string error;  // empty when healthy
  TimePoint ts{};
};

class ScanService {
 public:
  ScanService(Spectrometer& spectrometer, SignalBus& bus, const Clock& clock);
  ~ScanService();
  ScanService(const ScanService&) = delete;
  ScanService& operator=(const ScanService&) = delete;

  // Starting at the integration already running is a no-op; a different one
  // restarts the engine. Fails with the engine's error (service stays stopped).
  Result<void> start(Duration integration);
  void stop();

  // Restarts a running scan; while stopped or paused only records the value
  // for the next start()/resume().
  Result<void> set_integration(Duration integration);

  void pause();
  Result<void> resume();  // no-op unless paused

  bool running() const;
  bool paused() const;
  // Requested value, replaced by the engine's snapped value after the first
  // reading of a scan.
  Duration integration() const;
  ScanStatus status() const;

 private:
  Result<void> start_engine(Duration integration);  // caller holds op_mutex_
  ScanStatus status_locked() const;
  void on_reading();
  void on_alarm(const Alarm& alarm);

  Spectrometer& spectrometer_;
  SignalBus& bus_;
  const Clock& clock_;

  std::recursive_mutex op_mutex_;  // serialises start/stop/pause/resume/set_integration
  mutable std::mutex mutex_;       // guards the state below
  bool started_ = false;           // this service started the engine and it is running
  bool paused_ = false;
  bool snap_pending_ = false;  // next reading reports the engine's snapped integration
  Duration integration_{};
  std::string error_;

  SignalBus::Subscription readings_;
  SignalBus::Subscription alarms_;
};

}  // namespace pychron::spectrometer
