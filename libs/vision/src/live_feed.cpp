#include "pychron/vision/live_feed.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace pychron::vision {

namespace {

using Steady = std::chrono::steady_clock;

}  // namespace

// Shared with the reader thread, which may outlive the feed.
struct LiveFeed::State {
  Opener open;
  LiveFeedOptions options;

  std::mutex mutex;
  std::condition_variable changed;
  bool quit = false;
  bool exited = false;   // the reader thread has ended
  bool opened = false;   // the first attempt has answered
  Result<void> open_result;
  FrameInfo info{0, 0, 255, 0.0};

  std::optional<Frame> frame;
  Steady::time_point read_began{};  // of `frame`
  Steady::time_point arrived{};
  Steady::time_point waiting_since = Steady::now();  // the open or read now in flight began
  double fps = 0;
  bool lost = false;      // the last open or read failed: the camera is being reopened
  bool stalled = false;   // somebody waited the whole timeout: no waiting until a frame comes
  Error error;

  // Held while the caller's clock is asked; the destructor takes it to say
  // the clock is gone.
  std::mutex stamp_mutex;
  bool stamp_gone = false;

  std::thread thread;
};

LiveFeed::LiveFeed(Opener open, LiveFeedOptions options) : state_(std::make_shared<State>()) {
  state_->open = std::move(open);
  state_->options = std::move(options);
  if (!state_->options.stamp) state_->options.stamp = &Steady::now;
  state_->thread = std::thread([state = state_] {
    State& s = *state;
    // The camera's clock, for as long as the feed is there.
    const ClockFn stamp = [state] {
      std::lock_guard lock(state->stamp_mutex);
      return state->stamp_gone ? TimePoint{} : state->options.stamp();
    };
    std::unique_ptr<IFrameSource> source;
    const auto ended = [&s] {
      std::lock_guard lock(s.mutex);
      s.exited = true;
      s.changed.notify_all();
    };
    // Waits out the pause before the camera is tried again; false: quit.
    const auto pause = [&s] {
      std::unique_lock lock(s.mutex);
      s.changed.wait_for(lock, s.options.reopen, [&s] { return s.quit; });
      return !s.quit;
    };
    for (;;) {
      {
        std::lock_guard lock(s.mutex);
        if (s.quit) break;
      }
      if (source == nullptr) {
        {
          std::lock_guard lock(s.mutex);
          s.waiting_since = Steady::now();
        }
        auto opened = s.open(stamp);
        std::unique_lock lock(s.mutex);
        const bool first = !s.opened;
        s.opened = true;
        if (!opened) {
          if (first) s.open_result = fail(opened.error());
          s.error = std::move(opened).error();
          s.lost = true;
          s.changed.notify_all();
          lock.unlock();
          if (!pause()) break;
          continue;
        }
        source = std::move(*opened);
        s.info = source->info();
        s.changed.notify_all();
        if (s.quit) break;
      }
      const Steady::time_point began = Steady::now();
      {
        std::lock_guard lock(s.mutex);
        s.waiting_since = began;
      }
      auto frame = source->grab();  // may not return for a long time
      const Steady::time_point now = Steady::now();
      std::unique_lock lock(s.mutex);
      if (s.quit) break;
      if (!frame) {
        s.error = std::move(frame).error();
        s.lost = true;
        s.changed.notify_all();
        lock.unlock();
        source.reset();  // let go of it before it is opened again
        if (!pause()) break;
        continue;
      }
      if (s.frame && !s.lost && !s.stalled) {
        const double dt = std::chrono::duration<double>(now - s.arrived).count();
        if (dt > 0) s.fps = s.fps > 0 ? 0.8 * s.fps + 0.2 / dt : 1.0 / dt;
      } else {
        s.fps = 0;
      }
      s.frame = std::move(*frame);
      s.read_began = began;
      s.arrived = now;
      s.lost = false;
      s.stalled = false;
      s.error = {};
      s.changed.notify_all();
    }
    source.reset();
    ended();
  });
}

LiveFeed::~LiveFeed() {
  State& s = *state_;
  {
    std::lock_guard lock(s.stamp_mutex);
    s.stamp_gone = true;
  }
  bool exited = false;
  {
    std::unique_lock lock(s.mutex);
    s.quit = true;
    s.changed.notify_all();
    // A reader that is between reads ends at once; one inside a read that
    // does not return is not waited for.
    exited = s.changed.wait_for(lock, s.options.timeout, [&s] { return s.exited; });
  }
  if (exited) s.thread.join();
  else s.thread.detach();
}

Result<void> LiveFeed::wait_open() {
  State& s = *state_;
  std::unique_lock lock(s.mutex);
  // Asked again later, it does not wait again for an open already known to be late.
  const auto deadline = s.waiting_since + s.options.timeout;
  if (!s.changed.wait_until(lock, deadline, [&s] { return s.opened; })) {
    return fail(ErrorKind::Timeout, "the camera has not opened in " + std::to_string(s.options.timeout.count()) + " ms");
  }
  return s.open_result;
}

Result<Frame> LiveFeed::grab() {
  State& s = *state_;
  const Steady::time_point asked = Steady::now();
  std::unique_lock lock(s.mutex);
  const auto no_frame = [&s] {
    return fail(ErrorKind::Timeout, "no frame from the camera in " + std::to_string(s.options.timeout.count()) + " ms");
  };
  // Known to be gone: nobody waits for it again until it shows a frame.
  if (s.lost) return fail(s.error);
  if (s.stalled) return no_frame();
  const bool got = s.changed.wait_until(lock, asked + s.options.timeout,
                                        [&] { return s.lost || (s.frame && s.read_began >= asked); });
  if (!got) {
    s.stalled = true;
    s.error = Error{ErrorKind::Timeout,
                    "no frame from the camera in " + std::to_string(s.options.timeout.count()) + " ms", {}};
    return no_frame();
  }
  if (s.lost) return fail(s.error);
  return *s.frame;
}

FrameInfo LiveFeed::info() const {
  std::lock_guard lock(state_->mutex);
  return state_->info;
}

LiveFeed::Latest LiveFeed::latest() const {
  State& s = *state_;
  std::lock_guard lock(s.mutex);
  Latest out;
  out.frame = s.frame;
  if (s.frame) out.age = std::chrono::duration_cast<std::chrono::milliseconds>(Steady::now() - s.arrived);
  // Whether or not anybody has waited for it: an open or a read that has
  // been in flight longer than the timeout is a camera that has stopped.
  const bool hung = Steady::now() - s.waiting_since > s.options.timeout;
  if (s.lost || s.stalled) out.error = s.error.what;
  else if (hung) out.error = "no frame from the camera in " + std::to_string(s.options.timeout.count()) + " ms";
  out.fps = out.error.empty() ? s.fps : 0.0;
  return out;
}

}  // namespace pychron::vision
