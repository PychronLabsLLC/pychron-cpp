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
  bool opening = true;                               // and it is an open
  // The camera last did what was hoped of it (began to open, opened, gave a
  // frame), and whether it has given a frame since it opened: until it has,
  // it is given the time a camera takes to wake.
  Steady::time_point progress = Steady::now();
  bool warm = false;
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
          // The first open has been waited for since the feed was made (the
          // state's initial times): wait_open() measures from there, and so
          // must latest(), or a thread slow to start (sanitizers, a loaded
          // machine) would leave an open that timed out looking fine. A
          // reopen starts its wait now.
          if (s.opened) {
            s.waiting_since = Steady::now();
            s.progress = s.waiting_since;
          }
          s.opening = true;
          s.warm = false;
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
        s.progress = Steady::now();
        s.changed.notify_all();
        if (s.quit) break;
      }
      const Steady::time_point began = Steady::now();
      {
        std::lock_guard lock(s.mutex);
        s.waiting_since = began;
        s.opening = false;
      }
      auto frame = source->grab();  // may not return for a long time
      const Steady::time_point now = Steady::now();
      std::unique_lock lock(s.mutex);
      if (s.quit) break;
      if (!frame) {
        // A read that comes back empty is not yet a camera that has gone: one
        // still waking gives nothing for a while, and a frame can be late.
        // Closing it and opening it again takes seconds; it is read again,
        // and given up only when nothing has come for as long as anyone waits.
        const auto patience = s.warm ? s.options.timeout : s.options.open_timeout;
        if (now - s.progress <= patience) {
          if (!s.stalled) s.error = frame.error();  // why, should it go on
          s.changed.wait_for(lock, std::chrono::milliseconds(15), [&s] { return s.quit; });
          if (s.quit) break;
          continue;
        }
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
      s.progress = now;
      s.warm = true;
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
  const auto deadline = s.waiting_since + s.options.open_timeout;
  if (!s.changed.wait_until(lock, deadline, [&s] { return s.opened; })) {
    return fail(ErrorKind::Timeout,
                "the camera has not opened in " + std::to_string(s.options.open_timeout.count()) + " ms");
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
  // Whether or not a read is in flight: nothing has come of the camera for
  // longer than it is given.
  const auto allowed = s.warm ? s.options.timeout : s.options.open_timeout;
  const bool hung = Steady::now() - s.progress >= allowed;
  // NOLINTNEXTLINE(bugprone-branch-clone): in order of precedence; two of the cases say the same
  if (s.lost || s.stalled) out.error = s.error.what;
  else if (hung && s.opening) out.error = "the camera has not opened in " + std::to_string(allowed.count()) + " ms";
  else if (hung && !s.error.what.empty()) out.error = s.error.what;  // its reads are coming back empty
  else if (hung) out.error = "no frame from the camera in " + std::to_string(allowed.count()) + " ms";
  out.fps = out.error.empty() ? s.fps : 0.0;
  return out;
}

}  // namespace pychron::vision
