#pragma once

// Isotopx NGX drivers (NGX driver spec, 2026-10-03). Both go through the
// NgxLink for their controller (ngx_link.hpp); nothing here has been run
// against an instrument (spec section 9 lists what bring-up must confirm).
//
// isotopx_ngx (NgxSpectrometer): magnet (native axis mass), source, intensity
// acquisition. No detector control and no beam blank: the NGX protocol as
// pychron uses it has neither (the magnet's deflect flag is the blanking).
//
// ngx_valves (NgxValves): valves wired to the NGX controller's outputs.
// Each actuation is "SAB 1", OpenValve / CloseValve (must answer E00),
// "SAB 0" on every exit path, and never touches an integration in progress.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/connectable.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/ngx_link.hpp"
#include "pychron/devices/spectrometer/roles.hpp"

namespace pychron::spectrometer {

struct NgxOptions {
  std::string link;  // registry name; default: the driver name
  NgxLinkOptions session;
  // Detector names in pychron order; ACQ values arrive in reverse of it.
  std::vector<ChannelId> channels;
  std::string rcs_id{codec::ngx::kDefaultRcsId};
  // Which event completes an integration: the buffered ACQ.B ("acq_b") or
  // the N-th per-second ACQ ("last_acq").
  enum class Completion { AcqB, LastAcq } completion = Completion::AcqB;
  int settle_ms = 500;            // SetMass settling time
  double deflect_threshold = 0.5; // |Δmass| above which SetMass deflects; 0: never
  Limits limits{0.0, 200.0};      // mass
};

class NgxSpectrometer final : public Device,
                              public IConnectable,
                              public IMassPositioner,
                              public IBeamSource,
                              public IIntensityAcquirer {
 public:
  NgxSpectrometer(std::string name, NgxLinkHandle link, NgxOptions options, const Clock* clock = nullptr);
  ~NgxSpectrometer() override;

  static DriverSchema schema();
  static Result<std::unique_ptr<NgxSpectrometer>> create(const DriverArgs& args);

  // Integration times the NGX accepts, seconds (pychron ISOTOPX_INTEGRATION_TIMES).
  static const std::vector<int>& integration_times();

  // IConnectable: the link session (banner, Login), then StopAcq and
  // SetAcqPeriod 1000.
  Result<void> connect() override;

  // IMassPositioner (mass). A move while an integration runs aborts it: the
  // pending next() returns Cancelled "aborted by magnet move".
  Axis native_axis() const override { return Axis::Mass; }
  Result<void> set(double mass) override;
  // GETMASS when idle; the last commanded mass while integrating (GETMASS
  // would need StopAcq).
  Result<double> read() override;
  Limits limits() const override { return options_.limits; }

  // IBeamSource: HV is IonEnergy; every NGX source parameter through SSO/GSO.
  Result<void> set_hv(double volts) override;
  Result<double> read_hv() override;
  std::span<const ParamSpec> params() const override { return params_; }
  Result<void> set_param(const ParamId& id, double value) override;
  Result<Readback> read_param(const ParamId& id) override;

  // IIntensityAcquirer. trigger() arms one integration (StartAcq N) unless
  // one is armed; next() returns it once its completing event arrives.
  std::vector<ChannelId> channels() const override { return options_.channels; }
  bool integrates() const override { return true; }
  Result<void> configure(Duration integration) override;
  Result<void> start() override;
  Result<void> stop() override;
  Result<void> trigger() override;
  Result<std::optional<Frame>> next(Duration timeout) override;

  struct AcqStats {
    std::uint64_t armed = 0, completed = 0, dropped_events = 0, aborted = 0;
  };
  AcqStats acq_stats() const;

 private:
  // Stopping: an integration was ended here and its StopAcq is not yet
  // answered; trigger() waits it out (a StartAcq before it would be E43).
  enum class State { Idle, Arming, Armed, Stopping };
  Result<std::shared_ptr<NgxLink>> link();
  Result<std::string> ask(const std::string& command);
  void on_event(const codec::ngx::AcqFrame& frame, TimePoint at, std::uint64_t session);
  // Ends the current integration (caller holds acq_mutex_); `why` is queued
  // for the waiting next() when non-empty.
  void abort_locked(const std::string& why);
  // The StopAcq after abort_locked() has been answered (or failed).
  void stopped();
  Result<void> stop_acq();

  NgxLinkHandle link_;
  NgxOptions options_;
  SteadyClock steady_;
  const Clock& clock_;
  std::vector<ParamSpec> params_;

  mutable std::mutex acq_mutex_;
  // Orders StartAcq and StopAcq on the wire: a StopAcq sent to end an
  // integration never overtakes the StartAcq still in flight that began it
  // (it would land first and leave that integration running: the next
  // StartAcq is then E43). Never taken while holding acq_mutex_. Held
  // across the command, which waits in clock time: a ClockMutex.
  ClockMutex wire_order_;
  std::condition_variable acq_cv_;
  bool running_ = false;
  State state_ = State::Idle;
  int seconds_ = 1;  // integration time, one of integration_times()
  int acq_count_ = 0;
  std::uint64_t arm_session_ = 0;
  TimePoint armed_at_{};
  std::deque<Result<Frame>> ready_;
  std::uint64_t seq_ = 0;
  AcqStats stats_;
  std::optional<double> mass_;  // last commanded
};

struct NgxValvesOptions {
  std::string link;
  NgxLinkOptions session;  // used only when this driver owns the link
  int status_retries = 2;  // GetValveStatus answered E00: read again at most this often
};

class NgxValves final : public Device, public IConnectable, public IValveActuator {
 public:
  NgxValves(std::string name, NgxLinkHandle link, NgxValvesOptions options);

  static DriverSchema schema();
  static Result<std::unique_ptr<NgxValves>> create(const DriverArgs& args);

  // Opens the session when this driver owns the link; a borrowed link is the
  // owner's to open.
  Result<void> connect() override;

  Result<void> open(const ValveAddress& address) override;
  Result<void> close(const ValveAddress& address) override;
  Result<ValveState> read(const ValveAddress& address) override;

 private:
  Result<void> actuate(const ValveAddress& address, bool open);

  NgxLinkHandle link_;
  NgxValvesOptions options_;
};

}  // namespace pychron::spectrometer
