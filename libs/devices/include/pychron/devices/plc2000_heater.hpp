#pragma once

// A heater run by an AutomationDirect PLC, over Modbus TCP (config kind
// "plc2000_heater"; legacy PLC2000Heater, AELAMS; plan 2026-10-05, E2).
//
// Addresses are 1-based, as in the legacy device file's [Register] section;
// the wire address is one less.
//
//   enable    coil: read coils (01), write single coil (05)
//   use_pid   coil: read coils (01), write single coil (05)
//   setpoint  read: 2 input registers (04), float32
//             write: 2 holding registers (16), int32 (or float32 with
//             setpoint_write_format = "float32")
//   readback  2 input registers (04), float32
//
// The setpoint asymmetry (an int32 written to holding registers, a float32
// read from input registers) is legacy's wire behaviour and almost
// certainly mirrors the PLC program; it is kept. With int32 a fractional
// setpoint is a Config error, not truncated as legacy's int(value) did. An
// address left out makes its operations not_supported(), not a silent
// no-op. Values are in whatever units the PLC program uses.
//
// Unit id, word order and the write-then-read round trip are confirmed at
// bring-up (plan task D3).

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "pychron/codecs/modbus.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/heater.hpp"
#include "pychron/devices/modbus_device_sim.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

enum class SetpointWriteFormat { Int32, Float32 };

struct Plc2000HeaterOptions {
  std::uint8_t unit = 1;
  codec::modbus::WordOrder word_order = codec::modbus::WordOrder::CDAB;
  // 1-based, as legacy's cfg; nullopt: that function is not supported.
  std::optional<std::uint16_t> enable;
  std::optional<std::uint16_t> use_pid;
  std::optional<std::uint16_t> setpoint;
  std::optional<std::uint16_t> readback;
  SetpointWriteFormat setpoint_write_format = SetpointWriteFormat::Int32;
};

class Plc2000Heater final : public Device, public IHeater {
 public:
  Plc2000Heater(std::string name, Transport& transport, Plc2000HeaterOptions options, const Clock* clock = nullptr);

  static DriverSchema schema();
  static Result<std::unique_ptr<Plc2000Heater>> create(const DriverArgs& args);
  // The [drivers.*] keys of schema(); SimSystem builds its sim from them.
  static Result<Plc2000HeaterOptions> parse_options(const toml::table& options);

  Result<void> set_enabled(bool on) override;
  Result<bool> enabled() override;
  Result<void> set_setpoint(double value) override;
  Result<double> setpoint() override;
  Result<double> readback() override;
  Result<void> set_use_pid(bool on) override;
  Result<bool> use_pid() override;

 private:
  Result<bool> read_coil(const std::optional<std::uint16_t>& address, const char* what);
  Result<void> write_coil(const std::optional<std::uint16_t>& address, const char* what, bool on);
  Result<double> read_float(const std::optional<std::uint16_t>& address, const char* what);

  Transport& transport_;
  Plc2000HeaterOptions options_;
};

// The PLC's side of one heater, for SimSystem and tests, on an injected
// clock. While enabled the readback approaches the setpoint with a
// first-order lag `tau`; disabled, it decays toward `ambient`. The coils and
// registers are those of `options`; anything else is an illegal address.
class Plc2000HeaterSim {
 public:
  Plc2000HeaterSim(const Clock& clock, Plc2000HeaterOptions options, double ambient = 25.0,
                   Duration tau = std::chrono::seconds(60));
  Plc2000HeaterSim(const Plc2000HeaterSim&) = delete;
  Plc2000HeaterSim& operator=(const Plc2000HeaterSim&) = delete;

  SimTransport::Hook hook();  // the sim must outlive the transport
  // The heater's coils and registers alone, for a PLC that also serves
  // other drivers (modbus_bus_hook). set_offline() does not reach it.
  ModbusDeviceSim device();

  bool enabled() const;
  bool use_pid() const;
  double setpoint() const;
  double readback() const;
  // When set, a written setpoint is stored this far off (a program that
  // clamps or scales).
  void set_setpoint_error(double error);
  // Offline: answers nothing (a PLC off the network).
  void set_offline(bool offline);

 private:
  void advance_locked() const;

  const Clock& clock_;
  Plc2000HeaterOptions options_;
  double ambient_;
  Duration tau_;
  mutable std::mutex mutex_;
  mutable TimePoint last_{};
  bool enabled_ = false;
  bool use_pid_ = false;
  double setpoint_ = 0.0;
  mutable double readback_;
  double setpoint_error_ = 0.0;
  std::atomic<bool> offline_{false};
  std::map<std::uint16_t, std::uint16_t> held_;  // holding registers as written
};

}  // namespace pychron
