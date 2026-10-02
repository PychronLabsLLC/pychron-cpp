#pragma once

// Thermo Qtegra RemoteControlServer (Argus, Helix), config kind
// "thermo_qtegra": one driver on one transport playing every spectrometer
// role. All wire text comes from codec::qtegra; nothing here has been checked
// against hardware.
//
// A dropped connection (Io / NotConnected) is repaired once per command:
// reopen the transport, repeat the connect step, retry.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pychron/codecs/thermo_qtegra.hpp"
#include "pychron/devices/connectable.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/reconnect.hpp"
#include "pychron/devices/spectrometer/roles.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::spectrometer {

struct QtegraOptions {
  // Detector names as Qtegra reports them in GetData.
  std::vector<ChannelId> channels{"H2", "H1", "AX", "L1", "L2", "CDD"};
  // Magnet DAC limits in volts.
  Limits limits{0.0, 10.0};
  codec::qtegra::Terminator terminator = codec::qtegra::kDefaultTerminator;
  // Integration periods to wait after an integration change; create()
  // accepts 0..100.
  double settle_periods = 2.0;
};

class QtegraSpectrometer final : public Device,
                                 public IConnectable,
                                 public IMassPositioner,
                                 public IBeamSource,
                                 public IIntensityAcquirer,
                                 public IDetectorControl,
                                 public IBeamBlank {
 public:
  QtegraSpectrometer(std::string name, Transport& transport, QtegraOptions options, const Clock* clock = nullptr);

  static DriverSchema schema();
  static Result<std::unique_ptr<QtegraSpectrometer>> create(const DriverArgs& args);

  // Successful reconnects since construction.
  std::uint64_t reconnects() const noexcept { return reconnector_.reconnects(); }

  // IConnectable: GetIntegrationTime; a numeric reply is success and seeds
  // the cached integration period. Never reconnects.
  Result<void> connect() override;

  // IMassPositioner (magnet DAC volts).
  Axis native_axis() const override { return Axis::Dac; }
  // Setters here and in the blank / detector roles accept any reply that is
  // not an explicit ERROR (pychron ignores these replies).
  Result<void> set(double value) override;
  Result<double> read() override;
  Result<bool> moving() override;
  Limits limits() const override { return options_.limits; }

  // IBeamBlank.
  Result<void> blank(bool on) override;

  // IDetectorControl. A channel outside `channels` is Config.
  Caps caps() const override { return DetectorCap::Gain | DetectorCap::Deflection | DetectorCap::Protect; }
  Result<void> protect(const ChannelId& channel, bool on) override;
  Result<void> set_deflection(const ChannelId& channel, double value) override;
  Result<double> read_deflection(const ChannelId& channel) override;
  Result<void> set_gain(const ChannelId& channel, double value) override;
  Result<double> read_gain(const ChannelId& channel) override;

  // IBeamSource. params() is the codec's canonical map, one spec per canonical
  // name under its preferred hardware name: HV 0..10000 V, every other
  // parameter a nominal -1e6..1e6 with no unit (real ranges are unverified).
  //
  // Only names the codec marks verified (pychron Python sends them) are ever
  // sent. A canonical parameter whose set name is unverified is not writable:
  // it is advertised read-only under its readback name when it has one
  // (emission) and not advertised at all otherwise (ESA+, ESA-). set_param()
  // on anything not writable is Config with nothing sent.
  //
  // Custom{name} is accepted when `name` is a verified hardware name the codec
  // knows; it is sent as given and checked against its canonical parameter's
  // range. An unverified name is refused like an unknown one. A readback name
  // is read-only. HV, however it is named, is written with SetHV and read
  // with GetHighVoltage (reported as setpoint and actual). SetHV and
  // SetParameter must be answered "OK".
  Result<void> set_hv(double volts) override;
  Result<double> read_hv() override;
  std::span<const ParamSpec> params() const override { return params_; }
  Result<void> set_param(const ParamId& id, double value) override;
  Result<Readback> read_param(const ParamId& id) override;

  // IIntensityAcquirer. Qtegra free-runs: start() and stop() are local, and
  // next() polls GetData once per integration period on a fixed cadence.
  // configure() snaps to a legal period and writes it only when it differs
  // from the cached one, then holds frames back for settle_periods.
  //
  // The wire read in next() runs without the acquirer mutex, so stop() never
  // waits for a transport timeout; a frame whose read was in flight when
  // stop() was called, or when configure() changed the period, is dropped
  // (that next() returns nullopt). A reply that names some of `channels`
  // is a frame with the others absent; one that names none of them is
  // Protocol, quoting the reply.
  std::vector<ChannelId> channels() const override { return options_.channels; }
  bool integrates() const override { return true; }
  Result<void> configure(Duration integration) override;
  Result<void> start() override;
  Result<void> stop() override;
  Result<std::optional<Frame>> next(Duration timeout) override;

 private:
  // The connect step on the bare transport. It is also the Reconnector's
  // on_connect, so it must never go through the Reconnector itself.
  Result<void> handshake();
  // One command through the Reconnector; the raw reply is the caller's to decode.
  Result<Bytes> exchange(Result<codec::Command> cmd);
  // For the setters whose reply pychron ignores: any reply but ERROR is success.
  Result<void> command_ack(Result<codec::Command> cmd);
  // For SetHV / SetParameter: the reply must be "OK".
  Result<void> command_ok(Result<codec::Command> cmd);
  Result<double> query_number(Result<codec::Command> cmd);
  // How `id` is written and read; Config when the id is not supported.
  struct ParamTarget {
    std::string hardware;             // name sent with SetParameter / GetParameter
    const ParamSpec* spec = nullptr;  // the canonical parameter's spec
    bool hv = false;                  // SetHV / GetHighVoltage instead
    bool readback = false;            // `hardware` is a readback name: read-only
  };
  Result<ParamTarget> param_target(const ParamId& id) const;
  // The cached integration time as a legal period.
  Duration period() const;
  // Config error unless `channel` is one of `channels`.
  Result<void> check_channel(const ChannelId& channel) const;

  Transport& transport_;
  QtegraOptions options_;
  SteadyClock steady_;  // used when no clock is injected
  const Clock& clock_;
  Reconnector reconnector_;
  std::vector<ParamSpec> params_;
  // Seconds, as last reported by GetIntegrationTime or written by
  // configure(); 0 until connected. Atomic rather than under mutex_ because
  // handshake() stores it from inside any command that reconnects.
  std::atomic<double> integration_s_{0.0};

  // Acquirer state.
  std::mutex mutex_;
  std::condition_variable cv_;
  bool running_ = false;
  // Bumped by stop() and by a configure() that changes the period; a read
  // begun under an earlier value is dropped.
  std::uint64_t run_ = 0;
  TimePoint due_{};
  std::uint64_t seq_ = 0;  // never reset
};

}  // namespace pychron::spectrometer
