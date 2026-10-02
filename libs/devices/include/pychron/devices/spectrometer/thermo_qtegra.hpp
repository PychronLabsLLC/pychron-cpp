#pragma once

// Thermo Qtegra RemoteControlServer (Argus, Helix), config kind
// "thermo_qtegra": one driver on one transport playing every spectrometer
// role. All wire text comes from codec::qtegra; nothing here has been checked
// against hardware.
//
// A dropped connection (Io / NotConnected) is repaired once per command:
// reopen the transport, repeat the connect step, retry.

#include <atomic>
#include <cstdint>
#include <memory>
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
  // Integration periods to wait after an integration change.
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

  // IBeamSource.
  Result<void> set_hv(double volts) override;
  Result<double> read_hv() override;
  std::span<const ParamSpec> params() const override { return params_; }
  Result<void> set_param(const ParamId& id, double value) override;
  Result<Readback> read_param(const ParamId& id) override;

  // IIntensityAcquirer.
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
  Result<double> query_number(Result<codec::Command> cmd);
  // Config error unless `channel` is one of `channels`.
  Result<void> check_channel(const ChannelId& channel) const;

  Transport& transport_;
  QtegraOptions options_;
  SteadyClock steady_;  // used when no clock is injected
  const Clock& clock_;
  Reconnector reconnector_;
  std::vector<ParamSpec> params_;
  // Seconds, as last reported by GetIntegrationTime; 0 until connected.
  std::atomic<double> integration_s_{0.0};
};

}  // namespace pychron::spectrometer
