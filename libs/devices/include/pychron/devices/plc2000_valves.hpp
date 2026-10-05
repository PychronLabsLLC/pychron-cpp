#pragma once

// Valves on an AutomationDirect PLC's coils, over Modbus TCP (config kind
// "plc2000_valves"; legacy PLC2000GPActuator, AELAMS; plan 2026-10-05, A7).
//
// A valve address is a coil number; the wire coil is address + coil_offset
// (legacy: address - 1). Open writes the coil ON, close writes it OFF, and
// read() reads that one coil. The PLC's echo of a write must name the same
// coil and value (legacy ignored it, and swallowed Modbus errors).
// read_many() reads each run of consecutive coils in one request; legacy
// read one coil per valve. No inversion here: a valve's `inverted` does it.
//
// Unit id is confirmed at bring-up (plan task D3).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

struct Plc2000ValvesOptions {
  std::uint8_t unit = 1;
  int coil_offset = -1;
};

class Plc2000Valves final : public Device, public IValveActuator {
 public:
  // Most coils one read asks for (Modbus allows 2000).
  static constexpr int kMaxCoilsPerRead = 2000;

  Plc2000Valves(std::string name, Transport& transport, Plc2000ValvesOptions options = {},
                DeviceOptions device_options = {});

  static DriverSchema schema();
  static Result<std::unique_ptr<Plc2000Valves>> create(const DriverArgs& args);

  Result<void> open(const ValveAddress& address) override;
  Result<void> close(const ValveAddress& address) override;
  Result<ValveState> read(const ValveAddress& address) override;
  std::vector<Result<ValveState>> read_many(const std::vector<ValveAddress>& addresses) override;

  // The wire coil of a valve address; Config error if it is not a number or
  // falls outside 0..65535.
  Result<std::uint16_t> coil(const ValveAddress& address) const;

 private:
  Result<void> write(const ValveAddress& address, bool on);
  Result<std::vector<bool>> read_coils(std::uint16_t start, std::uint16_t count);

  Transport& transport_;
  Plc2000ValvesOptions options_;
};

}  // namespace pychron
