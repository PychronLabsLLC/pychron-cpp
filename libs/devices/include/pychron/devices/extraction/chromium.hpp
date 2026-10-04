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
// The stage is Chromium's: millimetres here, whole microns on the wire. A
// position is a scan Chromium has defined ("s3": Scans.MoveTo 3) or a hole on
// the current tray, which a TrayLookup resolves. A move is started, then
// moving() polled: it reads the position (or Scans.InPos?) once per call and
// reports arrival after three good polls in a row, as legacy pychron did.
// Moves outside the configured travel are refused before anything is sent.
//
// Output is percent, 0 to 100: the only unit. Calls block and are made from
// one thread at a time (the script host's); state queries may come from
// another.

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Where a named position is on a tray, in mm, and which names a tray has.
// The laser system supplies this from its tray maps.
struct TrayLookup {
  std::function<std::optional<StagePosition>(std::string_view tray, std::string_view position)> find;
  std::function<std::vector<std::string>(std::string_view tray)> names;
};

class ChromiumLaser final : public Device, public IExtractionDevice, public ILaserDevice, public IStage {
 public:
  ChromiumLaser(std::string name, Transport& transport, ChromiumOptions options = {}, DeviceOptions device = {});

  static DriverSchema schema();
  // Config error, naming the key, for an option out of range.
  static Result<std::unique_ptr<ChromiumLaser>> create(const DriverArgs& args);

  // What Sys.ID? answered; empty before prepare().
  std::string chromium_id() const;
  void set_tray_lookup(TrayLookup lookup);

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
  IStage* stage() override { return this; }

  // ILaserDevice
  Result<void> fire_laser() override;
  Result<void> stop_laser() override;
  Result<bool> is_firing() override;
  Result<void> warmup() override { return {}; }

  // IStage
  Result<void> move_to_position(std::string_view position, bool autocenter) override;
  Result<void> set_axis(Axis axis, double value) override;
  Result<void> set_xy(double x, double y) override;
  Result<StagePosition> position() override;
  Result<bool> moving() override;
  Result<void> set_tray(std::string_view tray) override;
  std::vector<std::string> positions() const override;

 private:
  // One reply line for a query; input left unread before it is discarded.
  Result<Bytes> query(const codec::Command& q);
  // `action`, then `confirm`'s reply. See the header comment.
  Result<Bytes> act(const codec::Command& action, const codec::Command& confirm);
  // Interlock error naming what is tripped, if anything is.
  Result<void> check_interlocks(std::string_view doing);
  // A refusal made here, before anything is sent: not a device failure.
  Unexpected<Error> refuse(ErrorKind kind, std::string what) const;

  // What moving() is waiting for.
  struct Target {
    std::optional<int> scan;        // a scan's start, or
    codec::chromium::Microns at{};  // a point, as sent
  };
  Result<StagePosition> read_position();
  // Config error if `mm` is outside the travel of axis 0 (x), 1 (y) or 2 (z).
  Result<void> check_travel(std::size_t axis, double mm) const;
  // Checks the travel limits, then starts a move to `to` (mm).
  Result<void> start_move(const StagePosition& to);
  codec::chromium::Microns to_wire(const StagePosition& mm) const;
  StagePosition from_wire(const codec::chromium::Microns& um) const;

  Transport& transport_;
  ChromiumOptions options_;
  mutable std::mutex state_;
  std::string id_;
  bool enabled_ = false;
  bool firing_ = false;
  double output_ = 0;
  std::optional<Target> target_;
  int good_polls_ = 0;
  std::string tray_;
  TrayLookup lookup_;
};

}  // namespace pychron::extraction
