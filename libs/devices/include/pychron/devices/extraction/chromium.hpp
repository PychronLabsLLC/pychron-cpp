#pragma once

// A Photon Machines Chromium laser system as an extraction device (config
// kind "chromium"): the Chromium program on the laser PC owns the laser, the
// stage and the camera, and this driver sends it commands over one open TCP
// connection. See docs/superpowers/specs/2026-10-04-chromium-protocol-survey.md.
//
// Chromium answers a query with one line and an action with nothing, unless
// the action is refused ("?<n>"). So every action is followed at once by a
// query, in one Transport transaction: if the first line back is "?<n>" the
// action failed (and the query's reply is read and dropped); otherwise it is
// the query's reply and the action stood. Nothing waits on silence.
//
// What the driver checks that legacy pychron did not: that the program on
// the port is a Chromium (prepare), that no interlock is tripped (enable and
// every fire), and that an output setpoint took (read back).
//
// Output is percent, 0 to 100: the only unit. Calls block and are made from
// one thread at a time (the script host's); state queries may come from
// another.

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "pychron/codecs/chromium.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::extraction {

struct ChromiumOptions {
  // Stage travel, mm, per axis (x, y, z): a move outside it is refused.
  std::array<std::pair<double, double>, 3> limits_mm{{{0, 50}, {0, 50}, {0, 50}}};
  // +1 or -1 per axis: stage mm times this is what Chromium is sent.
  std::array<int, 3> signs{1, 1, 1};
  codec::chromium::Microns move_speed{5000, 5000, 100};  // microns per second
  double in_position_um = 10;
  // false for a unit that refuses Laser.Enable: enabling then only checks
  // the interlocks, and "enabled" is the driver's own flag.
  bool use_enable = true;
};

class ChromiumLaser final : public Device, public IExtractionDevice, public ILaserDevice {
 public:
  ChromiumLaser(std::string name, Transport& transport, ChromiumOptions options = {}, DeviceOptions device = {});

  // What Sys.ID? answered; empty before prepare().
  std::string chromium_id() const;

  // IExtractionDevice
  const std::string& device_name() const override { return name(); }
  Result<void> prepare() override;
  Result<void> enable() override;
  Result<void> disable() override;
  Result<bool> is_enabled() override;
  Result<void> extract(double value, ExtractUnits units) override;
  Result<void> end_extract() override;
  Result<double> output() override;
  bool supports(ExtractUnits units) const override { return units == ExtractUnits::Percent; }
  ILaserDevice* laser() override { return this; }

  // ILaserDevice
  Result<void> fire_laser() override;
  Result<void> stop_laser() override;
  Result<bool> is_firing() override;
  Result<void> warmup() override { return {}; }

 private:
  // One reply line for a query; input left unread before it is discarded.
  Result<Bytes> query(const codec::Command& q);
  // `action`, then `confirm`'s reply. See the header comment.
  Result<Bytes> act(const codec::Command& action, const codec::Command& confirm);
  // Interlock error naming what is tripped, if anything is.
  Result<void> check_interlocks(std::string_view doing);
  // A refusal made here, before anything is sent: not a device failure.
  Unexpected<Error> refuse(ErrorKind kind, std::string what) const;

  Transport& transport_;
  ChromiumOptions options_;
  mutable std::mutex state_;
  std::string id_;
  bool enabled_ = false;
  bool firing_ = false;
  double output_ = 0;
};

}  // namespace pychron::extraction
