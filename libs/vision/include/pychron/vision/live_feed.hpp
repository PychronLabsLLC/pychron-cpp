#pragma once

// LiveFeed (live camera design, section 2): a live camera behind a thread of
// its own. A camera's read blocks for as long as the camera likes, which for
// one that has lost its cable is for ever; whoever asks a LiveFeed for a
// frame waits no longer than the feed's timeout, and whoever only wants to
// look (latest()) does not wait at all.
//
//   grab()    a frame whose read began after the call, or an error within
//             `timeout`. After a read has failed or timed out it fails at
//             once, until the camera shows a frame again.
//   latest()  the newest frame there is, how old, and what is wrong.
//
// A camera that is lost, or was never there, is opened again every `reopen`.
// Time here is real time (a camera does not run on a simulated clock); the
// frames are stamped from `stamp`, the caller's clock.
//
// The feed's destructor does not wait for a read that does not return: the
// reader thread is then left to end by itself when the read does, and owns
// everything it still uses. `stamp` is not called after the destructor.

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/vision/opencv_source.hpp"
#include "pychron/vision/source.hpp"

namespace pychron::vision {

struct LiveFeedOptions {
  std::chrono::milliseconds timeout{1000};
  // How long the camera may take to open before that is trouble: a camera
  // takes a moment to wake, longer than anyone should wait for a frame.
  std::chrono::milliseconds open_timeout{5000};
  std::chrono::milliseconds reopen{1000};
  ClockFn stamp;  // empty: steady_clock
};

class LiveFeed final : public IFrameSource {
 public:
  // Opens the camera, on the reader thread; given the clock to stamp its
  // frames from. Must not refer to anything that may go before the feed's
  // thread ends, except through that clock.
  using Opener = std::function<Result<std::unique_ptr<IFrameSource>>(ClockFn stamp)>;

  explicit LiveFeed(Opener open, LiveFeedOptions options = {});
  ~LiveFeed() override;
  LiveFeed(const LiveFeed&) = delete;
  LiveFeed& operator=(const LiveFeed&) = delete;

  // How the first attempt to open the camera went; Timeout if it has not
  // answered within the open timeout (it is still being tried).
  Result<void> wait_open();

  Result<Frame> grab() override;
  FrameInfo info() const override;  // zeros until the camera has opened

  struct Latest {
    std::optional<Frame> frame;       // the newest; none yet: nullopt
    std::chrono::milliseconds age{0}; // since it arrived
    double fps = 0;                   // as seen lately; 0: unknown
    std::string error;                // empty: frames are arriving
  };
  Latest latest() const;

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace pychron::vision
