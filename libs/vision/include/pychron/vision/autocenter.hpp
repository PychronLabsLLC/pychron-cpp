#pragma once

#include <span>
#include <string>
#include <string_view>

#include "pychron/core/clock.hpp"
#include "pychron/vision/calibration.hpp"
#include "pychron/vision/finder.hpp"
#include "pychron/vision/frame.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::vision {

struct AutocenterParams {
  double hole_radius_mm = 0.5;
  double tolerance_mm = 0.03;  // about 0.7 px at 23 px/mm; above the finder's noise
  int max_iterations = 4;
  double max_step_mm = 0.5;
  double max_total_mm = 1.0;
  int frames_per_step = 3;  // hint for the caller; step() uses whatever span it is given
  double crop_scale = 2.55;  // crop side = crop_scale * 2 * radius
  Vec2 aim_offset_px{};      // crosshair offset from image centre
};

// Why a step failed. Camera is the caller's: it could not get frames to give.
enum class AutocenterReason { None, NoTarget, MaxIterations, Runaway, MaxTotal, Invalid, Clipped, StaleFrame, Camera };

// "", "no_target", "max_iterations", "runaway", "max_total", "invalid",
// "clipped", "stale_frame", "camera".
std::string_view to_string(AutocenterReason reason) noexcept;

struct AutocenterStep {
  enum class Action { Move, Converged, Failed } action = Action::Failed;
  Vec2 move_mm{};  // relative, stage frame
  Vec2 offset_mm{};
  int iteration = 0;
  AutocenterReason reason = AutocenterReason::None;
};

// Step-function controller: takes frames, returns a decision, never moves a
// stage. The caller applies a Move, waits for the stage to settle, and calls
// step() again with fresh frames.
class Autocenter {
 public:
  Autocenter(ITargetFinder& finder, CameraStageMap map, double px_per_mm, AutocenterParams params);

  AutocenterStep step(std::span<const FrameView> frames);
  void reset();

 private:
  ITargetFinder& finder_;
  CameraStageMap map_;
  double px_per_mm_;
  AutocenterParams params_;
  int iteration_ = 0;
  double total_mm_ = 0;
  double prev_offset_mm_ = -1;  // negative: no previous step
  int grow_count_ = 0;
  bool have_last_ts_ = false;
  TimePoint last_ts_{};
};

}  // namespace pychron::vision
