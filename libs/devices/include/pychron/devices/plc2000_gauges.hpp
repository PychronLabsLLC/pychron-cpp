#pragma once

// Gauge values held in an AutomationDirect PLC's registers, read over
// Modbus TCP (config kind "plc2000_gauges"; legacy PLC2000GaugeController,
// AELAMS).
//
// Gauge channel n reads two holding registers (function 03) starting at
// register n + register_offset (legacy: n - 1) and decodes a 32-bit float in
// `word_order` (legacy default cdab: low word first). The PLC reports what
// its program computed, in the gauge's configured units. A value that is
// not finite or is negative is a Protocol error; legacy passed it on.
//
// Unit id and word order are confirmed at bring-up (plan task D3).

#include <memory>
#include <string>
#include <vector>

#include "pychron/codecs/modbus.hpp"
#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/channel_gauge.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

struct Plc2000GaugesOptions {
  std::uint8_t unit = 1;
  codec::modbus::WordOrder word_order = codec::modbus::WordOrder::CDAB;
  int register_offset = -1;
  std::vector<int> channels{1};
};

class Plc2000Gauges final : public Device, public IPressureGauge, public IChannelPressureGauge, public IScannable {
 public:
  Plc2000Gauges(std::string name, Transport& transport, Plc2000GaugesOptions options, const Clock* clock = nullptr);

  static DriverSchema schema();
  static Result<std::unique_ptr<Plc2000Gauges>> create(const DriverArgs& args);

  Result<double> read_pressure() override;  // the first configured channel
  Result<double> read_pressure(int channel) override;
  std::vector<int> pressure_channels() const override { return options_.channels; }
  Result<Sample> sample() override;

 private:
  Transport& transport_;
  Plc2000GaugesOptions options_;
  const Clock* clock_;
};

}  // namespace pychron
