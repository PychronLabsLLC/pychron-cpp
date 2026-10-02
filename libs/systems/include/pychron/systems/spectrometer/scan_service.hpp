#pragma once

// Free-running scan lifecycle for one spectrometer's acquisition engine:
// start/stop/pause/resume and integration changes, with every state change
// published as ScanStatus on the SignalBus. The service only stops an engine
// it started itself.
//
// Thread-safe. Mutating calls are serialised on one mutex; no ScanStatus is
// ever published while a service mutex is held, so subscribers may call back
// into the service. Bus handlers share a state block that outlives the
// service, so an event still in flight on a scheduler thread when the
// service is destroyed is dropped safely.

#include <memory>
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
  struct State;

  // Callers hold op_mutex_ and publish the returned status after releasing it.
  Result<void> start_engine(Duration integration, ScanStatus& out);

  Spectrometer& spectrometer_;
  SignalBus& bus_;
  const Clock& clock_;

  std::mutex op_mutex_;  // serialises the engine start/stop sequences
  std::shared_ptr<State> state_;

  SignalBus::Subscription readings_;
  SignalBus::Subscription alarms_;
};

}  // namespace pychron::spectrometer
