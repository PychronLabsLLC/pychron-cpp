// LiveFeed (live camera design, section 2): a camera's blocking read on its
// own thread, so that nobody waits for a camera longer than they chose to.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "pychron/vision/live_feed.hpp"

using namespace pychron;
using namespace pychron::vision;
using namespace std::chrono_literals;
using Steady = std::chrono::steady_clock;

namespace {

// What the fake camera does next, set by the test.
struct Camera {
  std::mutex mutex;
  std::condition_variable changed;
  enum class Mode { Run, Hang, Fail } mode = Mode::Run;
  std::chrono::milliseconds period{5};
  bool opens = true;
  int opened = 0;
  std::atomic<int> reads{0};
  std::atomic<int> alive{0};  // sources not yet destroyed

  void set(Mode m) {
    {
      std::lock_guard lock(mutex);
      mode = m;
    }
    changed.notify_all();
  }
};

class FakeSource final : public IFrameSource {
 public:
  FakeSource(Camera& camera, ClockFn stamp) : camera_(camera), stamp_(std::move(stamp)) { ++camera_.alive; }
  ~FakeSource() override { --camera_.alive; }
  Result<Frame> grab() override {
    std::unique_lock lock(camera_.mutex);
    // a hung read returns only when the camera is let go
    camera_.changed.wait(lock, [&] { return camera_.mode != Camera::Mode::Hang; });
    if (camera_.mode == Camera::Mode::Fail) return fail(ErrorKind::Io, "camera unplugged");
    const auto period = camera_.period;
    lock.unlock();
    std::this_thread::sleep_for(period);
    Frame f = Frame::make(4, 3, 255, static_cast<std::uint16_t>(++seq_ % 200));
    f.seq = seq_;
    f.timestamp = stamp_();
    ++camera_.reads;
    return f;
  }
  FrameInfo info() const override { return {4, 3, 255, 200.0}; }

 private:
  Camera& camera_;
  ClockFn stamp_;
  std::uint64_t seq_ = 0;
};

LiveFeed::Opener opener(Camera& camera) {
  return [&camera](ClockFn stamp) -> Result<std::unique_ptr<IFrameSource>> {
    std::lock_guard lock(camera.mutex);
    ++camera.opened;
    if (!camera.opens) return fail(ErrorKind::Io, "no camera 0");
    return std::unique_ptr<IFrameSource>(std::make_unique<FakeSource>(camera, std::move(stamp)));
  };
}

LiveFeedOptions quick() {
  LiveFeedOptions o;
  o.timeout = 150ms;
  o.reopen = 20ms;
  return o;
}

template <class F>
bool eventually(F done, std::chrono::milliseconds limit = 3000ms) {
  const auto until = Steady::now() + limit;
  while (!done()) {
    if (Steady::now() > until) return false;
    std::this_thread::sleep_for(2ms);
  }
  return true;
}

}  // namespace

TEST(LiveFeed, GrabGivesAFrameReadAfterTheCall) {
  Camera camera;
  camera.period = 20ms;
  LiveFeed feed(opener(camera), quick());
  ASSERT_TRUE(feed.wait_open());
  ASSERT_TRUE(eventually([&] { return camera.reads >= 2; }));
  const int before = camera.reads;
  auto frame = feed.grab();
  ASSERT_TRUE(frame) << frame.error().what;
  // a read that had already begun when it was asked for does not count: the
  // stage may not have been at rest for it
  EXPECT_GE(frame->seq, static_cast<std::uint64_t>(before) + 2);
  auto next = feed.grab();
  ASSERT_TRUE(next);
  EXPECT_GT(next->seq, frame->seq);
  EXPECT_EQ(feed.info().width, 4);
}

TEST(LiveFeed, FramesAreStampedFromTheCallersClock) {
  Camera camera;
  LiveFeedOptions o = quick();
  o.stamp = [] { return TimePoint{} + std::chrono::hours(7); };
  LiveFeed feed(opener(camera), o);
  auto frame = feed.grab();
  ASSERT_TRUE(frame) << frame.error().what;
  EXPECT_EQ(frame->timestamp, TimePoint{} + std::chrono::hours(7));
}

TEST(LiveFeed, AHungReadTimesOutAndThenFailsAtOnce) {
  Camera camera;
  LiveFeed feed(opener(camera), quick());
  ASSERT_TRUE(feed.grab());
  camera.set(Camera::Mode::Hang);
  std::this_thread::sleep_for(30ms);  // the read in flight ends; the next hangs
  auto started = Steady::now();
  auto frame = feed.grab();
  auto took = Steady::now() - started;
  ASSERT_FALSE(frame);
  EXPECT_EQ(frame.error().kind, ErrorKind::Timeout);
  EXPECT_GE(took, 140ms);
  EXPECT_LT(took, 600ms);
  // and nobody waits for it again until it shows a frame
  started = Steady::now();
  frame = feed.grab();
  took = Steady::now() - started;
  ASSERT_FALSE(frame);
  EXPECT_LT(took, 50ms);
  EXPECT_FALSE(feed.latest().error.empty());
  // it comes back
  camera.set(Camera::Mode::Run);
  ASSERT_TRUE(eventually([&] { return feed.grab().has_value(); }));
  EXPECT_TRUE(feed.latest().error.empty());
}

TEST(LiveFeed, ALostCameraIsSaidAtOnceAndReopened) {
  Camera camera;
  LiveFeed feed(opener(camera), quick());
  ASSERT_TRUE(feed.grab());
  camera.set(Camera::Mode::Fail);
  ASSERT_TRUE(eventually([&] { return !feed.latest().error.empty(); }));
  const auto started = Steady::now();
  const auto frame = feed.grab();
  ASSERT_FALSE(frame);
  EXPECT_EQ(frame.error().kind, ErrorKind::Io);
  EXPECT_NE(frame.error().what.find("camera unplugged"), std::string::npos) << frame.error().what;
  EXPECT_LT(Steady::now() - started, 50ms);
  // the last picture is still there to show, and getting older
  const LiveFeed::Latest last = feed.latest();
  ASSERT_TRUE(last.frame.has_value());
  // reopened, again and again, until it is back
  ASSERT_TRUE(eventually([&] {
    std::lock_guard lock(camera.mutex);
    return camera.opened >= 3;
  }));
  camera.set(Camera::Mode::Run);
  ASSERT_TRUE(eventually([&] { return feed.grab().has_value(); }));
}

TEST(LiveFeed, ACameraThatDoesNotOpenIsSaidAndTriedAgain) {
  Camera camera;
  camera.opens = false;
  LiveFeed feed(opener(camera), quick());
  const auto opened = feed.wait_open();
  ASSERT_FALSE(opened);
  EXPECT_NE(opened.error().what.find("no camera 0"), std::string::npos) << opened.error().what;
  EXPECT_FALSE(feed.grab());
  EXPECT_FALSE(feed.latest().frame.has_value());
  {
    std::lock_guard lock(camera.mutex);
    camera.opens = true;
  }
  ASSERT_TRUE(eventually([&] { return feed.grab().has_value(); }));
}

TEST(LiveFeed, LatestNeverWaitsAndSaysHowOld) {
  Camera camera;
  LiveFeed feed(opener(camera), quick());
  EXPECT_LT(feed.latest().age, 1000ms);  // nothing yet: no frame, and it did not wait
  ASSERT_TRUE(feed.grab());
  LiveFeed::Latest now = feed.latest();
  ASSERT_TRUE(now.frame.has_value());
  EXPECT_LT(now.age, 100ms);
  camera.set(Camera::Mode::Hang);
  std::this_thread::sleep_for(120ms);
  const auto started = Steady::now();
  now = feed.latest();
  EXPECT_LT(Steady::now() - started, 20ms);
  ASSERT_TRUE(now.frame.has_value());
  EXPECT_GE(now.age, 80ms);
  camera.set(Camera::Mode::Run);
  ASSERT_TRUE(eventually([&] { return feed.latest().fps > 20; }));
}

TEST(LiveFeed, GoesAwayWhileAReadHangs) {
  Camera camera;
  const auto started = Steady::now();
  {
    LiveFeed feed(opener(camera), quick());
    ASSERT_TRUE(feed.grab());
    camera.set(Camera::Mode::Hang);
    std::this_thread::sleep_for(30ms);
  }  // must not wait for a read that may never return
  EXPECT_LT(Steady::now() - started, 1500ms);
  // when the read does return, its thread ends and lets the camera go
  camera.set(Camera::Mode::Run);
  EXPECT_TRUE(eventually([&] { return camera.alive == 0; }));
}

TEST(LiveFeed, GoesAwayCleanlyWhileRunning) {
  Camera camera;
  {
    LiveFeed feed(opener(camera), quick());
    ASSERT_TRUE(feed.grab());
  }
  EXPECT_EQ(camera.alive, 0) << "a feed that could stop its thread waited for it";
}
