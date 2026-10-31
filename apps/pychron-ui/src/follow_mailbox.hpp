#pragma once

// The followed steps waiting for the main thread (follow plots design,
// section 4.4). The runner's thread adds, the main thread takes: a stalled
// main thread loses the oldest rows, never the newest. Qt-free and with no
// thread of its own; the data is handed over under this mutex, so the call
// that wakes the taker carries nothing.

#include <cstddef>
#include <mutex>
#include <vector>

#include "pychron/laser/laser_system.hpp"

namespace pychron::ui {

class FollowMailbox {
 public:
  static constexpr std::size_t kMaxPending = 4096;

  // Appends `sample`. True: nothing is posted yet, so the caller must post a
  // call that takes.
  [[nodiscard]] bool add(const laser::FollowSample& sample);
  // Everything pending, oldest first; the next add asks for a post again.
  [[nodiscard]] std::vector<laser::FollowSample> take();

 private:
  std::mutex mutex_;
  std::vector<laser::FollowSample> pending_;
  bool posted_ = false;
};

}  // namespace pychron::ui
