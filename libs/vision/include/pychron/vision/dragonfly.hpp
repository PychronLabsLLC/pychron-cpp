#pragma once

#include <span>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/vision/calibration.hpp"
#include "pychron/vision/finder.hpp"
#include "pychron/vision/frame.hpp"
#include "pychron/vision/spiral.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::vision {

struct DragonflyParams {
  Duration total_duration{};
  double perimeter_radius_mm = 2.5;
  double saturation_threshold = 0.75;
  double aggressiveness = 1.0;
  double move_threshold_mm = 0.033;
  double max_step_mm = 0.5;
  SpiralKind spiral = SpiralKind::Hexagon;
  double spiral_base_mm = 0.5;
  int frames_per_step = 1;  // hint for the caller; step() uses whatever span it is given
  int miss_frames_before_search = 3;
  double target_radius_mm = 0.5;  // sizes the crop (2.5x diameter) and the mask (1.05x diameter)
};

struct DragonflyStep {
  enum class Action { Hold, Move, Done } action = Action::Hold;
  Vec2 target_mm{};  // offset from the start position, not from the current one
  double saturation = 0;
  enum class Reason { Saturated, Deadband, Track, Search, PerimeterClamp, Miss, Elapsed, Invalid } reason =
      Reason::Invalid;
};

// Keeps a glowing sample under the aim point. Takes frames, returns a
// decision, never moves a stage and never touches laser power. After a Move
// the caller waits for the stage to settle and passes only frames captured
// after that; older frames are an error.
class Dragonfly {
 public:
  Dragonfly(ITargetFinder& finder, CameraStageMap map, double px_per_mm, DragonflyParams params);

  void start(TimePoint now, Vec2 stage_pos_mm);
  Result<DragonflyStep> step(std::span<const FrameView> frames, TimePoint now, Vec2 stage_pos_mm);

 private:
  bool params_ok() const;

  ITargetFinder& finder_;
  CameraStageMap map_;
  double px_per_mm_;
  DragonflyParams params_;
  bool started_ = false;
  TimePoint start_time_{};
  Vec2 start_pos_{};
  Vec2 anchor_{};  // spiral anchor, offset from start_pos_
  int misses_ = 0;
  bool have_move_ = false;
  TimePoint last_move_now_{};
  Spiral spiral_;
};

}  // namespace pychron::vision
