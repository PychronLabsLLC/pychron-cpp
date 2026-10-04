#pragma once

// LaserSystem: an extraction device with trays (laser system design, section
// 3.4). It wraps a driver's IExtractionDevice and is what a run talks to:
// everything goes straight to the driver except named positions, where a
// hole on the current tray is turned into stage millimetres through the
// tray's calibration and sent as the driver's set_xy. A driver moves a stage
// and knows nothing about holes.
//
//   set_tray("221-hole")        chooses the map and reads its calibration
//   move_to_position("12")      hole 12 -> stage (x, y); z is not touched
//   move_to_position("s3")      not a hole: the driver's own named position
//
// Nothing is guessed: no tray, a tray that is not calibrated for this device,
// or a calibration made against another version of the map is a Config error
// and nothing is sent. The calibration is read when the tray is set, so one
// saved by another process is used from the next set_tray on.
//
// Calls are made from one thread at a time (the script host's); tray() and
// calibration() may be asked from another.

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/laser/calibration_store.hpp"
#include "pychron/laser/tray_map.hpp"

namespace pychron::laser {

class LaserSystem final : public extraction::IExtractionDevice, public extraction::IStage {
 public:
  // `driver`, `trays` and `calibrations` must outlive the system. `name` is
  // the device's name in queues and in calibration files.
  LaserSystem(std::string name, extraction::IExtractionDevice& driver, const TrayLibrary& trays,
              const CalibrationStore& calibrations);

  std::string tray() const;               // empty: none
  CalibrationStatus calibration() const;  // of the current tray, as read by set_tray; Missing with no tray

  // IExtractionDevice: the driver's.
  const std::string& device_name() const override { return name_; }
  Result<void> prepare() override { return driver_.prepare(); }
  Result<void> enable() override { return driver_.enable(); }
  Result<void> disable() override { return driver_.disable(); }
  Result<bool> is_enabled() override { return driver_.is_enabled(); }
  Result<void> extract(double value, extraction::ExtractUnits units) override { return driver_.extract(value, units); }
  Result<void> end_extract() override { return driver_.end_extract(); }
  Result<double> output() override { return driver_.output(); }
  bool supports(extraction::ExtractUnits units) const override { return driver_.supports(units); }
  extraction::ILaserDevice* laser() override { return driver_.laser(); }
  extraction::IFurnaceDevice* furnace() override { return driver_.furnace(); }
  // This object when the driver has a stage; null otherwise.
  extraction::IStage* stage() override { return driver_.stage() != nullptr ? this : nullptr; }
  extraction::IPatternRunner* pattern_runner() override { return driver_.pattern_runner(); }
  extraction::IPipetteService* pipettes() override { return driver_.pipettes(); }
  extraction::ICryo* cryo() override { return driver_.cryo(); }
  extraction::IMotorService* motors() override { return driver_.motors(); }
  extraction::IImaging* imaging() override { return driver_.imaging(); }

  // IStage
  // `autocenter` is accepted and has no effect yet.
  Result<void> move_to_position(std::string_view position, bool autocenter) override;
  Result<void> set_axis(Axis axis, double value) override;
  Result<void> set_xy(double x, double y) override;
  Result<extraction::StagePosition> position() override;
  Result<bool> moving() override;
  // An empty name clears the tray. Config error for a tray the lab lacks; the
  // current one is kept.
  Result<void> set_tray(std::string_view tray) override;
  std::vector<std::string> positions() const override;  // the tray's holes, in file order

 private:
  Result<extraction::IStage*> driver_stage();

  const std::string name_;
  extraction::IExtractionDevice& driver_;
  const TrayLibrary& trays_;
  const CalibrationStore& calibrations_;

  mutable std::mutex mutex_;  // tray_ and status_; never held across a driver call
  const TrayMap* tray_ = nullptr;
  CalibrationStatus status_;
};

}  // namespace pychron::laser
