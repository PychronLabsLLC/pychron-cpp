#pragma once

// Granville-Phillips Micro-Ion gauge controller (ion gauge plus two
// Convectrons), config kind "gp_microion". Speaks codec::microion over any
// Transport; several controllers may share one RS-485 bus by address.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pychron/codecs/microion.hpp"
#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/channel_gauge.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

// Channels: 1 = ion gauge (IG), 2 = Convectron A (CG1), 3 = Convectron B
// (CG2). read_pressure() and sample() use the first configured channel.
class GpMicroIon final : public Device, public IPressureGauge, public IChannelPressureGauge, public IScannable {
 public:
  // `address` in 0..255; `channels` non-empty, unique, within 1..3
  // (create() checks). `clock` stamps samples and health; SteadyClock when
  // null.
  GpMicroIon(std::string name, Transport& transport, int address, std::vector<int> channels,
             const Clock* clock = nullptr);

  // Registry hooks (REGISTER_DRIVER).
  static DriverSchema schema();
  static Result<std::unique_ptr<GpMicroIon>> create(const DriverArgs& args);

  Result<double> read_pressure() override;
  Result<double> read_pressure(int channel) override;
  std::vector<int> pressure_channels() const override { return channels_; }
  Result<Sample> sample() override;

  // Ion gauge filament on/off. While off the ion gauge channel reads as a
  // Protocol error ("gauge off").
  Result<void> set_ion_gauge(bool on);

 private:
  Transport& transport_;
  int address_;
  std::vector<int> channels_;
  const Clock* clock_;
};

// SimSystem hook contract. SimSystem owns the physics and answers
// `pressure(channel)` (channel 1..3, nullopt = no sensor fitted); the hook
// speaks the controller's side of the wire at `address`. Called only from
// the transport worker, but must be safe while SimSystem advances its model
// on another thread.
struct MicroIonSimModel {
  int address = 1;
  std::function<std::optional<double>(int channel)> pressure;
  bool ion_gauge_on = true;  // initial filament state
};

// For SimTransport::hooked(): answers requests addressed to `model.address`,
// stays silent for other addresses and unreadable bytes (as an RS-485 slave
// does), and replies "?aa SYNTX ER" to commands it cannot parse. Sensors
// without a model value, and the ion gauge while its filament is off, read
// codec::microion::kGaugeOff.
SimTransport::Hook microion_sim_hook(MicroIonSimModel model);

}  // namespace pychron
