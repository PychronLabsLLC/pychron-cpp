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

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/laser/calibration_store.hpp"
#include "pychron/laser/camera.hpp"
#include "pychron/laser/correction_store.hpp"
#include "pychron/laser/pattern.hpp"
#include "pychron/laser/tray_camera.hpp"
#include "pychron/laser/tray_map.hpp"
#include "pychron/vision/autocenter.hpp"
#include "pychron/vision/source.hpp"

namespace pychron::laser {

class PatternRunner;

// How the last autocenter ended.
struct AutocenterOutcome {
  enum class Result { None, Converged, Failed, Stopped } result = Result::None;
  vision::AutocenterReason reason = vision::AutocenterReason::None;  // why it failed
  std::string hole;
  std::string tray;
  int iterations = 0;       // looks taken
  StageXY found{};          // where the stage ended
  StageXY moved_mm{};       // from where the centring started
  double residual_mm = 0;   // the last measured offset
  std::string note;         // e.g. the correction could not be saved
};

class LaserSystem final : public extraction::IExtractionDevice, public extraction::IStage {
 public:
  // `driver`, `trays` and `calibrations` must outlive the system. `name` is
  // the device's name in queues and in calibration files.
  // With `patterns` (which must outlive the system too) and a driver that has
  // a stage, the system runs the lab's patterns over it.
  LaserSystem(std::string name, extraction::IExtractionDevice& driver, const TrayLibrary& trays,
              const CalibrationStore& calibrations, const PatternLibrary* patterns = nullptr);
  ~LaserSystem() override;

  std::string tray() const;               // empty: none
  CalibrationStatus calibration() const;  // of the current tray, as read by set_tray; Missing with no tray

  // Autocenter (laser autocenter design). Both are set before the first
  // move, and what they are given must outlive the system.
  //
  // With corrections, a hole move goes to where the hole was last found
  // (when that is within its guard of the calibrated position). With a
  // camera, move_to_position(hole, autocenter = true) goes on, once the
  // stage has arrived, to centre the hole: it waits `settle`, looks, nudges
  // the stage, and looks again, until the hole is under the aim point (the
  // position is then saved as the hole's correction) or it gives up (the
  // stage returns to where the centring started). All of that happens in
  // moving(): the caller's poll loop drives it, one stage command per poll.
  void set_corrections(const CorrectionStore& corrections);
  // Config error, and no camera, for one that does not follow the stage (a
  // recording): see usable_for_autocenter().
  Result<void> attach_camera(CameraConfig config, std::unique_ptr<vision::IFrameSource> frames, const Clock& clock);
  bool has_camera() const noexcept { return frames_ != nullptr; }
  // What this system's camera is over, for a simulated one. Valid while the
  // system lives; asks the driver where the stage is.
  TraySightFn sight();
  AutocenterOutcome last_autocenter() const;
  HoleCorrections corrections() const;  // of the current tray
  // How far autocenter may take `hole` from its calibrated position: 45% of
  // the way to the nearest other hole, at most 1 mm. Finding the neighbour
  // is not finding the hole. 0 for a hole the current tray lacks.
  double guard_mm(std::string_view hole) const;

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
  // The driver's own if it runs patterns itself; else the system's, given a
  // stage and a pattern library; else null.
  extraction::IPatternRunner* pattern_runner() override;
  extraction::IPipetteService* pipettes() override { return driver_.pipettes(); }
  extraction::ICryo* cryo() override { return driver_.cryo(); }
  extraction::IMotorService* motors() override { return driver_.motors(); }
  extraction::IImaging* imaging() override { return driver_.imaging(); }

  // IStage
  // `autocenter` centres a hole when the system has a camera; without one,
  // and for a position that is not a hole, it has no effect.
  Result<void> move_to_position(std::string_view position, bool autocenter) override;
  Result<void> set_axis(Axis axis, double value) override;
  Result<void> set_xy(double x, double y, double speed_mm_s = 0) override;
  Result<void> stop() override;
  Result<extraction::StagePosition> position() override;
  Result<bool> moving() override;
  bool autocenter_needs_polling() const override { return has_camera(); }
  std::string last_move_note() override;
  // An empty name clears the tray. Config error for a tray the lab lacks; the
  // current one is kept.
  Result<void> set_tray(std::string_view tray) override;
  std::vector<std::string> positions() const override;  // the tray's holes, in file order

 private:
  struct Centering;  // an autocenter in progress

  Result<extraction::IStage*> driver_stage();
  // Ends an autocenter in progress, if any, as `how` (nothing is saved).
  void abandon(AutocenterOutcome::Result how);
  Result<bool> look(extraction::IStage& stage);
  Result<bool> give_up(extraction::IStage& stage, vision::AutocenterReason why);
  double guard_locked(const Hole& hole) const;

  const std::string name_;
  extraction::IExtractionDevice& driver_;
  const TrayLibrary& trays_;
  const CalibrationStore& calibrations_;

  std::unique_ptr<PatternRunner> runner_;  // over this system's own stage

  const CorrectionStore* correction_store_ = nullptr;
  std::optional<CameraConfig> camera_;
  std::unique_ptr<vision::IFrameSource> frames_;
  const Clock* clock_ = nullptr;
  std::unique_ptr<vision::ITargetFinder> finder_;
  std::unique_ptr<Centering> centering_;  // the caller's thread only

  mutable std::mutex mutex_;  // everything below; never held across a driver call
  const TrayMap* tray_ = nullptr;
  CalibrationStatus status_;
  HoleCorrections corrections_;
  AutocenterOutcome outcome_;
  std::string move_note_;  // of the last centring that ran to its end; taken once
};

}  // namespace pychron::laser
