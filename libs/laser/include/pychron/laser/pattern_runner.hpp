#pragma once

// PatternRunner: runs a pattern by moving a stage through its points (laser
// patterns design, section 7). The center is where the stage is when the
// pattern starts; the stage is brought back there at the end.
//
// There is no thread. execute_pattern() sends the first move; each running()
// is one step: while the stage is still moving it says true, when it has
// arrived it sends the next point, and after the last it says false. The
// caller's poll loop (the script host's) is what drives the pattern, as it
// drives a single move.
//
// A point the stage refuses, or a move that fails, ends the pattern: that
// call returns the error, with the pattern and the point in front, and the
// next running() says false. Nothing switches the laser off here: that is
// the run's, when its script ends.
//
// A pattern that follows the glow (a dragonfly) has no path: for its
// duration each step waits for the stage to rest and settle, looks, and asks
// vision::Dragonfly whether to hold or where to move; at the end the stage
// returns to where it started. It needs the device's camera (set_vision). A
// camera that fails part way sends the stage back to the start; then the
// camera's on_failure decides: the pattern holds there until its time is up
// (and says so in last_note()), or ends with an error. It runs for the run's
// duration, or its own when the run has none. With the beam on and no glow
// to follow it searches outward: never beyond its perimeter, nor beyond the
// room its hole has before the neighbours (PatternVision::room_mm). What it
// could not do (never looked, never saw the glow, was held in) is said in
// last_note().
//
// Calls are made from one thread at a time; progress() may be asked from
// another.

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/laser/camera.hpp"
#include "pychron/laser/pattern.hpp"
#include "pychron/vision/finder.hpp"
#include "pychron/vision/source.hpp"

namespace pychron::laser {

// The means to see, for a pattern that follows the glow: the device's camera.
// What it points at must outlive the runner.
struct PatternVision {
  vision::IFrameSource* frames = nullptr;
  const CameraConfig* camera = nullptr;
  const Clock* clock = nullptr;
  // How far from where it starts a pattern may roam before it reaches the
  // neighbouring holes (mm); 0 or less: not known, no limit beyond its own.
  std::function<double()> room_mm;
  explicit operator bool() const noexcept { return frames != nullptr && camera != nullptr && clock != nullptr; }
};

class PatternRunner final : public extraction::IPatternRunner {
 public:
  // `stage` and `patterns` must outlive the runner. `device` is named in errors.
  PatternRunner(std::string device, extraction::IStage& stage, const PatternLibrary& patterns);
  ~PatternRunner() override;

  // Given by the laser system when it has a camera. Without it a pattern
  // that follows the glow (a dragonfly) is refused.
  void set_vision(PatternVision vision);

  // Config error for a pattern the library lacks or could not load, while
  // another is running, or while the stage is still moving (there is no
  // center yet). Nothing is sent unless the stage's position was read.
  Result<void> execute_pattern(std::string_view pattern) override { return execute_pattern_for(pattern, 0); }
  // `duration_s`: how long the run heats for; a pattern that follows the
  // glow runs for that, or for its own duration when the run gives none.
  Result<void> execute_pattern_for(std::string_view pattern, double duration_s) override;
  Result<bool> running() override;
  // Drops what is left and stops the stage where it is. A stage that cannot
  // stop finishes its current move; that is not an error.
  Result<void> stop_pattern() override;
  std::vector<std::string> patterns() const override;
  bool needs_polling() const override { return true; }
  std::string last_note() override;

  // A pattern that follows the glow is running. As the other calls: one
  // thread at a time.
  bool following() const noexcept { return follow_ != nullptr; }

  // "<name>, point <i> of <n>" while one runs; empty when idle.
  std::string progress() const;

 private:
  struct Follow;  // a dragonfly in progress
  Result<void> start_following(const Pattern& pattern, double run_duration_s);
  Result<bool> follow();
  Result<bool> lose_the_camera(std::string why);
  Result<bool> go_home();

  // Sends path_[next_] and advances; ends the pattern on failure.
  Result<void> send_next();
  void end();

  const std::string device_;
  extraction::IStage& stage_;
  const PatternLibrary& patterns_;

  mutable std::mutex mutex_;  // everything below; never held across a stage call
  bool active_ = false;
  std::string name_;
  double velocity_ = 0;
  std::vector<StageXY> path_;  // stage millimetres
  std::size_t next_ = 0;       // the point to send next
  std::string note_;           // of the last pattern that ended; taken once

  PatternVision vision_;
  std::unique_ptr<vision::ITargetFinder> finder_;
  std::unique_ptr<Follow> follow_;  // the caller's thread only
};

}  // namespace pychron::laser
