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
// Whoever drives it (a script, or the laser window's worker) makes its calls
// from one thread. It may be watched from another (snapshot(), view(),
// tray(), calibration()) and stopped from a third (emergency_stop()): every
// call that reaches the driver, the camera or a centring takes one gate, the
// pattern runner's included (laser window design, section 3).

#include <atomic>
#include <filesystem>
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
#include "pychron/laser/camera_scale.hpp"
#include "pychron/laser/correction_store.hpp"
#include "pychron/laser/pattern.hpp"
#include "pychron/laser/tray_camera.hpp"
#include "pychron/laser/tray_map.hpp"
#include "pychron/vision/autocenter.hpp"
#include "pychron/vision/finder.hpp"
#include "pychron/vision/frame.hpp"
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

enum class LaserActivity { Idle, Moving, Centring, Pattern };
// "idle", "moving", "centring", "pattern".
std::string_view to_string(LaserActivity activity) noexcept;

// What a watcher is told: read from the device now, with what the system
// knows about itself. What could not be read is left unset, and `error`
// says the first reason.
struct LaserSnapshot {
  std::string device;
  std::string tray;  // empty: none
  CalibrationState calibration = CalibrationState::Missing;
  std::string calibration_why;
  bool has_stage = false;
  bool has_laser = false;
  bool has_camera = false;
  std::optional<extraction::StagePosition> position;
  std::optional<bool> enabled;
  std::optional<bool> firing;
  std::optional<double> output;
  std::vector<std::string> interlocks;  // the tripped ones
  LaserActivity activity = LaserActivity::Idle;
  std::string pattern_progress;  // "<name>, point <i> of <n>" while one runs
  std::string last_hole;         // the hole the stage was last sent to; empty once it is sent elsewhere
  AutocenterOutcome autocenter;  // how the last centring ended
  bool stopped = false;          // the emergency stop is latched
  std::string error;
};

// What the camera sees now, and what the device's finder makes of it.
struct CameraView {
  vision::Frame frame;
  std::optional<vision::Target> target;  // the one nearest the aim; none: nothing found
  std::vector<vision::Target> targets;   // everything the finder made out
  StageXY aim_px{};                      // where the beam is in the frame, in pixels
  double px_per_mm = 0;
  double expected_radius_px = 0;         // of a hole of the current tray
  // A live camera: how old the picture is, the rate frames arrive at, and
  // what is wrong when they have stopped (the picture is then the last one).
  int age_ms = 0;
  double fps = 0;
  std::string trouble;
};

class LaserSystem final : public extraction::IExtractionDevice,
                          public extraction::IStage,
                          public extraction::ILaserDevice,
                          public extraction::IImaging {
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
  // A camera for looking only: view() shows its picture and what the finder
  // makes of it; a hole move is not centred by it and a pattern cannot
  // follow the glow with it. For a camera that does not follow this stage
  // (use = "view").
  Result<void> attach_viewer(CameraConfig config, std::unique_ptr<vision::IFrameSource> frames, const Clock& clock);
  // A picture to look at.
  bool has_camera() const noexcept { return frames_ != nullptr; }
  // A camera that may move the stage.
  bool can_centre() const noexcept { return frames_ != nullptr && centres_; }
  // What this system's camera is over, for a simulated one. Valid while the
  // system lives; asks the driver where the stage is.
  TraySightFn sight();
  AutocenterOutcome last_autocenter() const;
  HoleCorrections corrections() const;  // of the current tray
  // How far autocenter may take `hole` from its calibrated position: 45% of
  // the way to the nearest other hole, at most 1 mm. Finding the neighbour
  // is not finding the hole. 0 for a hole the current tray lacks.
  double guard_mm(std::string_view hole) const;

  // Watching. Neither moves anything, and neither asks moving() or
  // running(): those advance a centring or a pattern, and belong to whoever
  // started it. Config error from view() for a device with no camera.
  LaserSnapshot snapshot();
  // `fresh`: a picture taken now (a live camera is waited for, up to its
  // timeout), for whoever has just moved the stage; otherwise the newest
  // there is. `any_size`: targets of whatever size (the scale that says how
  // big a hole should look is what is being measured).
  Result<CameraView> view(bool fresh = false, bool any_size = false);
  // The camera's pixel scale as measured (camera_scale.hpp): used from now
  // on instead of what its configuration says. Nothing with no camera.
  void set_measured_scale(const ScaleMeasurement& measured);
  // CameraConfig::geometry() of the camera; empty with none.
  std::string camera_geometry() const;

  // Everything off, now, from any thread: the beam, the output, the enable,
  // the stage, a pattern, a centring. Every step is tried whatever the
  // others answered; the first error is returned. It latches: until
  // reset_stop(), enabling, an output, firing, a warmup, every move and
  // every pattern are refused with an Interlock error, so a script that has
  // not yet seen its run aborted cannot fire again.
  Result<void> emergency_stop();
  // The latch alone, without waiting for the device or for whoever holds
  // the gate: from now on nothing that makes light or motion is accepted.
  // For the thread the stop button is pressed on; emergency_stop() follows.
  void latch_stop() noexcept { stopped_.store(true); }
  bool stopped() const noexcept { return stopped_.load(); }
  void reset_stop() noexcept { stopped_.store(false); }

  // IExtractionDevice: the driver's.
  const std::string& device_name() const override { return name_; }
  Result<void> prepare() override;
  Result<void> enable() override;
  Result<void> disable() override;
  Result<bool> is_enabled() override;
  Result<void> extract(double value, extraction::ExtractUnits units) override;
  Result<void> end_extract() override;
  Result<double> output() override;
  bool supports(extraction::ExtractUnits units) const override { return driver_.supports(units); }
  // This object when the driver has a laser; null otherwise.
  extraction::ILaserDevice* laser() override { return driver_.laser() != nullptr ? this : nullptr; }
  extraction::IFurnaceDevice* furnace() override { return driver_.furnace(); }
  // This object when the driver has a stage; null otherwise.
  extraction::IStage* stage() override { return driver_.stage() != nullptr ? this : nullptr; }
  // The driver's own if it runs patterns itself; else the system's, given a
  // stage and a pattern library; else null. Either way behind the gate.
  extraction::IPatternRunner* pattern_runner() override;
  extraction::IPipetteService* pipettes() override { return driver_.pipettes(); }
  extraction::ICryo* cryo() override { return driver_.cryo(); }
  extraction::IMotorService* motors() override { return driver_.motors(); }
  // The driver's own when it takes pictures itself; else this object, given
  // a camera; else null.
  extraction::IImaging* imaging() override;

  // IImaging, by the system's camera: <snapshot dir>/<name>.png, never over
  // a picture that is there ("-2", "-3", ... is added); with no name, the
  // UTC time. Returns the file. Config error for a name that is not a plain
  // file name, and when no directory was set; a live camera that has
  // stopped gives its error, not its last picture. Recording is not
  // supported.
  void set_snapshot_dir(std::filesystem::path dir);
  Result<std::string> snapshot(std::string_view name) override;
  Result<void> start_video_recording(std::string_view name) override;
  Result<void> stop_video_recording() override;

  // ILaserDevice: the driver's. Config error for a device with no laser.
  Result<void> fire_laser() override;
  Result<void> stop_laser() override;
  Result<bool> is_firing() override;
  Result<void> warmup() override;
  Result<std::vector<std::string>> tripped_interlocks() override;

  // IStage
  // `autocenter` centres a hole when the system has a camera; without one,
  // and for a position that is not a hole, it has no effect.
  Result<void> move_to_position(std::string_view position, bool autocenter) override;
  Result<void> set_axis(Axis axis, double value) override;
  Result<void> set_xy(double x, double y, double speed_mm_s = 0) override;
  Result<void> stop() override;
  Result<extraction::StagePosition> position() override;
  Result<bool> moving() override;
  bool autocenter_needs_polling() const override { return can_centre(); }
  std::string last_move_note() override;
  // An empty name clears the tray. Config error for a tray the lab lacks; the
  // current one is kept.
  Result<void> set_tray(std::string_view tray) override;
  std::vector<std::string> positions() const override;  // the tray's holes, in file order

 private:
  struct Centering;    // an autocenter in progress
  class GatedRunner;   // the pattern runner, each call behind the gate

  // Interlock error while the emergency stop is latched.
  Result<void> allowed() const;
  // The system's own runner, or the driver's; null when neither.
  extraction::IPatternRunner* inner_runner();

  Result<extraction::IStage*> driver_stage();
  // moving(), under the gate: one step of a centring, or the driver's answer.
  Result<bool> advance();
  // The stage is sent somewhere that is not a hole: a centring is over, and
  // the system no longer knows the stage to be on one.
  void off_the_hole();
  // Ends an autocenter in progress, if any, as `how` (nothing is saved).
  void abandon(AutocenterOutcome::Result how);
  Result<bool> look(extraction::IStage& stage);
  Result<bool> give_up(extraction::IStage& stage, vision::AutocenterReason why);
  double guard_locked(const Hole& hole) const;

  const std::string name_;
  extraction::IExtractionDevice& driver_;
  const TrayLibrary& trays_;
  const CalibrationStore& calibrations_;

  // Taken by every call that reaches the driver, the camera or a centring,
  // and before mutex_. Recursive: a pattern's step moves this system's stage.
  mutable std::recursive_mutex gate_;
  std::atomic<bool> stopped_{false};  // the emergency stop's latch
  std::atomic<bool> moving_{false};   // a move was started, and moving() has not yet said it is over

  std::unique_ptr<PatternRunner> runner_;  // over this system's own stage
  std::unique_ptr<GatedRunner> gated_runner_;

  const CorrectionStore* correction_store_ = nullptr;
  std::optional<CameraConfig> camera_;
  std::unique_ptr<vision::IFrameSource> frames_;
  bool centres_ = false;  // the camera may move the stage (attach_camera)
  const Clock* clock_ = nullptr;
  std::unique_ptr<vision::ITargetFinder> finder_;
  std::unique_ptr<Centering> centering_;  // under the gate
  // How far a pattern may roam from the hole the stage was last sent to
  // before it reaches a neighbour (45% of the way to the nearest); 0: the
  // stage is not known to be on a hole. Under the gate.
  double hole_room_ = 0;

  mutable std::mutex mutex_;  // everything below; never held across a driver call
  const TrayMap* tray_ = nullptr;
  CalibrationStatus status_;
  HoleCorrections corrections_;
  AutocenterOutcome outcome_;
  std::string move_note_;  // of the last centring that ran to its end; taken once
  std::string last_hole_;  // the hole the stage was last sent to
  std::filesystem::path snapshot_dir_;
};

}  // namespace pychron::laser
