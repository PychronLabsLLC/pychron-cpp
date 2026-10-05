#pragma once

// PatternRunner: runs a pattern by moving a stage through its points (laser
// patterns design, section 7). The centre is where the stage is when the
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
// Calls are made from one thread at a time; progress() may be asked from
// another.

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/laser/pattern.hpp"

namespace pychron::laser {

class PatternRunner final : public extraction::IPatternRunner {
 public:
  // `stage` and `patterns` must outlive the runner. `device` is named in errors.
  PatternRunner(std::string device, extraction::IStage& stage, const PatternLibrary& patterns);

  // Config error for a pattern the library lacks or could not load, or while
  // another is running. Nothing is sent unless the stage's position was read.
  Result<void> execute_pattern(std::string_view pattern) override;
  Result<bool> running() override;
  // Drops what is left and stops the stage where it is. A stage that cannot
  // stop finishes its current move; that is not an error.
  Result<void> stop_pattern() override;
  std::vector<std::string> patterns() const override;

  // "<name>, point <i> of <n>" while one runs; empty when idle.
  std::string progress() const;

 private:
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
};

}  // namespace pychron::laser
