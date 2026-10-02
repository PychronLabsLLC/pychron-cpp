// QtegraSpectrometer as a polled, vendor-integrating acquirer, on the Qtegra
// sim hook with a ManualClock; and a replay of a synthetic session trace.

#include <gtest/gtest.h>

#include <array>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <thread>

#include "pychron/devices/spectrometer/thermo_qtegra.hpp"
#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"
#include "spectrometer/legacy/sim_util.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace legacy_test;

namespace {

// Legal Qtegra integration periods used below.
constexpr Duration kHalf = std::chrono::nanoseconds(524'288'000);
constexpr Duration kOne = std::chrono::nanoseconds(1'048'576'000);
constexpr Duration kTick = std::chrono::nanoseconds(1);

std::vector<std::string> written_text(const SimTransport& sim) {
  std::vector<std::string> out;
  for (const auto& tx : sim.written()) out.push_back(to_string(tx));
  return out;
}

std::shared_ptr<QtegraSimModel> make_model() {
  auto model = std::make_shared<QtegraSimModel>();
  model->integration_s = 0.524288;
  model->intensities = {{"H2", 0.1}, {"H1", 0.2}, {"AX", 0.3}, {"L1", 0.4}, {"L2", 0.5}, {"CDD", 0.6}};
  return model;
}

// The driver is connected, so its cached integration period is the model's
// 0.524288 s. Nothing is due later than the clock's start until configure()
// changes the period.
struct QtegraAcquire : ::testing::Test {
  ManualClock clock;
  std::shared_ptr<QtegraSimModel> model = make_model();
  std::unique_ptr<SimTransport> sim = open_hooked(qtegra_sim_hook(model));
  QtegraSpectrometer q{"argus", *sim, {}, &clock};

  void SetUp() override { ASSERT_TRUE(q.connect()); }

  void override_data(std::string text) {
    std::lock_guard lock(model->mutex);
    model->data_override = std::move(text);
  }

  // The frame a zero-timeout next() must deliver now.
  Frame frame() {
    auto r = q.next(Duration::zero());
    EXPECT_TRUE(r) << (r ? "" : to_string(r.error()));
    if (!r || !*r) {
      ADD_FAILURE() << "no frame was due";
      return {};
    }
    return **r;
  }

  void expect_not_due() {
    const auto before = sim->written().size();
    auto r = q.next(Duration::zero());
    ASSERT_TRUE(r) << to_string(r.error());
    EXPECT_FALSE(r->has_value());
    EXPECT_EQ(sim->written().size(), before);
  }
};

}  // namespace

TEST_F(QtegraAcquire, NextBeforeStartIsConfig) {
  auto r = q.next(Duration::zero());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_EQ(r.error().device, "argus");
  EXPECT_EQ(written_text(*sim), (std::vector<std::string>{"GetIntegrationTime\r"}));
}

TEST_F(QtegraAcquire, ConnectSeedsCachedIntegration) {
  // 0.5 s snaps to the 0.524288 s the instrument reported at connect.
  ASSERT_TRUE(q.configure(500ms));
  EXPECT_EQ(written_text(*sim), (std::vector<std::string>{"GetIntegrationTime\r"}));
  // No change, so no settling: a frame is due at once and spans that period.
  ASSERT_TRUE(q.start());
  EXPECT_EQ(frame().span, kHalf);
}

TEST_F(QtegraAcquire, ConfigureSnapsAndSendsOnlyOnChange) {
  ASSERT_TRUE(q.configure(1s));
  EXPECT_EQ(written_text(*sim), (std::vector<std::string>{"GetIntegrationTime\r", "SetIntegrationTime 1.048576\r"}));
  ASSERT_TRUE(q.configure(1s));
  ASSERT_TRUE(q.configure(1100ms));  // snaps to the same period
  EXPECT_EQ(sim->written().size(), 2U);
  std::lock_guard lock(model->mutex);
  EXPECT_DOUBLE_EQ(model->integration_s, 1.048576);
}

TEST_F(QtegraAcquire, ConfigureErrorReplyIsProtocolAndIsRetriedNextTime) {
  auto scripted = open_scripted({step("GetIntegrationTime\r", "0.524288\r\n"),
                                 step("SetIntegrationTime 1.048576\r", "ERROR: busy\r\n"),
                                 step("SetIntegrationTime 1.048576\r", "OK\r\n")});
  QtegraSpectrometer driver("argus", *scripted, {}, &clock);
  ASSERT_TRUE(driver.connect());
  auto r = driver.configure(1s);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  // The period was not taken as changed, so the same request is sent again.
  EXPECT_TRUE(driver.configure(1s));
  expect_verified(*scripted);
}

TEST_F(QtegraAcquire, NonPositiveIntegrationIsConfig) {
  for (Duration d : {Duration::zero(), Duration(-1s)}) {
    auto r = q.configure(d);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Config);
  }
  EXPECT_EQ(sim->written().size(), 1U);
}

TEST_F(QtegraAcquire, NotDueReturnsNulloptWithoutTraffic) {
  ASSERT_TRUE(q.configure(1s));
  ASSERT_TRUE(q.start());
  expect_not_due();
}

TEST_F(QtegraAcquire, NotDueWaitsNoLongerThanTimeoutOnAStoppedClock) {
  ASSERT_TRUE(q.configure(1s));
  ASSERT_TRUE(q.start());
  const auto before = sim->written().size();
  // Nobody advances the ManualClock: the wait is bounded by real time.
  auto pending = std::async(std::launch::async, [this] { return q.next(5ms); });
  if (pending.wait_for(10s) != std::future_status::ready) {
    ADD_FAILURE() << "next(5ms) did not return";
    q.stop();  // wakes it
  }
  auto r = pending.get();
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_FALSE(r->has_value());
  EXPECT_EQ(sim->written().size(), before);
}

TEST_F(QtegraAcquire, FirstFrameAfterChangeWaitsSettlePeriods) {
  ASSERT_TRUE(q.configure(1s));
  ASSERT_TRUE(q.start());
  clock.advance(2 * kOne - kTick);
  expect_not_due();
  clock.advance(kTick);
  EXPECT_EQ(frame().ts, TimePoint{} + 2 * kOne);
}

// A next() already waiting on the clock when configure() changes the period
// wakes to the new due time and labels its frame with the new period.
TEST_F(QtegraAcquire, NextWaitingAcrossConfigureUsesTheNewPeriod) {
  ASSERT_TRUE(q.configure(1s));
  ASSERT_TRUE(q.start());
  auto pending = std::async(std::launch::async, [this] { return q.next(60s); });
  const Duration two = 2 * kOne;  // the 2.097152 s period
  ASSERT_TRUE(q.configure(2s));
  clock.advance(2 * two - kTick);
  EXPECT_EQ(pending.wait_for(20ms), std::future_status::timeout);  // old due time has passed; still held
  clock.advance(kTick);
  if (pending.wait_for(10s) != std::future_status::ready) {
    ADD_FAILURE() << "next() did not wake";
    q.stop();
  }
  auto r = pending.get();
  ASSERT_TRUE(r) << to_string(r.error());
  ASSERT_TRUE(r->has_value());
  EXPECT_EQ((*r)->span, two);
  EXPECT_EQ((*r)->ts, TimePoint{} + 2 * two);
  // The cadence continues on the new period.
  clock.advance(two - kTick);
  expect_not_due();
  clock.advance(kTick);
  EXPECT_EQ(frame().span, two);
}

// A failed period change restores the due time it held back.
TEST_F(QtegraAcquire, FailedConfigureDoesNotHoldFramesBack) {
  ASSERT_TRUE(q.start());
  sim->drop_next();
  auto r = q.configure(1s);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(frame().span, kHalf);
}

TEST_F(QtegraAcquire, SettlePeriodsComeFromOptions) {
  QtegraOptions options;
  options.settle_periods = 0.0;
  QtegraSpectrometer driver("argus", *sim, options, &clock);
  ASSERT_TRUE(driver.connect());
  ASSERT_TRUE(driver.configure(1s));
  ASSERT_TRUE(driver.start());
  auto r = driver.next(Duration::zero());
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_TRUE(r->has_value());
}

TEST_F(QtegraAcquire, FramesOnFixedCadenceWithoutDrift) {
  ASSERT_TRUE(q.configure(1s));
  ASSERT_TRUE(q.start());
  clock.advance(2 * kOne);
  const TimePoint first = frame().ts;

  // Polls land at irregular times, each less than a period after the last.
  // A frame is due at every whole period after the first one, however late
  // the previous poll was.
  const std::array<Duration, 4> steps{400ms, 130ms, 777ms, 250ms};
  TimePoint due = first + kOne;
  int frames = 0;
  for (std::size_t i = 0; clock.now() < first + 20 * kOne; ++i) {
    clock.advance(steps[i % steps.size()]);
    auto r = q.next(Duration::zero());
    ASSERT_TRUE(r) << to_string(r.error());
    ASSERT_EQ(r->has_value(), clock.now() >= due) << "poll " << i;
    if (!*r) continue;
    ++frames;
    due += kOne;
  }
  EXPECT_EQ(frames, 20);
  EXPECT_EQ(due, first + 21 * kOne);
}

TEST_F(QtegraAcquire, MoreThanOnePeriodBehindResetsCadence) {
  ASSERT_TRUE(q.start());
  frame();
  clock.advance(5 * kHalf + 100ms);
  frame();
  // One frame for the missed periods, not five; the cadence restarts from now.
  expect_not_due();
  clock.advance(kHalf - kTick);
  expect_not_due();
  clock.advance(kTick);
  frame();
}

TEST_F(QtegraAcquire, ExactlyOnePeriodBehindKeepsCadence) {
  ASSERT_TRUE(q.start());
  frame();
  clock.advance(2 * kHalf);
  frame();
  frame();
  expect_not_due();
}

TEST_F(QtegraAcquire, FrameShape) {
  ASSERT_TRUE(q.start());
  clock.advance(3s);
  const Frame a = frame();
  clock.advance(kHalf);
  const Frame b = frame();
  EXPECT_TRUE(a.integrated);
  EXPECT_EQ(a.span, kHalf);
  EXPECT_EQ(a.ts, TimePoint{} + 3s);
  EXPECT_EQ(b.ts, TimePoint{} + 3s + kHalf);
  EXPECT_EQ(a.seq, 1U);
  EXPECT_EQ(b.seq, 2U);
  const std::vector<std::pair<ChannelId, double>> expected{{"H2", 0.1}, {"H1", 0.2}, {"AX", 0.3},
                                                           {"L1", 0.4}, {"L2", 0.5}, {"CDD", 0.6}};
  EXPECT_EQ(a.values, expected);
}

TEST_F(QtegraAcquire, SpanFollowsConfiguredPeriod) {
  ASSERT_TRUE(q.configure(1s));
  ASSERT_TRUE(q.start());
  clock.advance(2 * kOne);
  EXPECT_EQ(frame().span, kOne);
}

TEST_F(QtegraAcquire, SeqNotResetAcrossStopStart) {
  ASSERT_TRUE(q.start());
  const auto first = frame().seq;
  ASSERT_TRUE(q.stop());
  auto stopped = q.next(Duration::zero());
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().kind, ErrorKind::Config);
  ASSERT_TRUE(q.start());
  clock.advance(kHalf);
  EXPECT_EQ(frame().seq, first + 1);
}

TEST_F(QtegraAcquire, MissingDetectorIsAbsentNotError) {
  override_data("H1,1.5,AX,2.5");
  ASSERT_TRUE(q.start());
  const Frame f = frame();
  EXPECT_EQ(f.values, (std::vector<std::pair<ChannelId, double>>{{"H1", 1.5}, {"AX", 2.5}}));
  EXPECT_FALSE(f.value("H2").has_value());
  EXPECT_EQ(q.health().state, DeviceState::Ok);
}

TEST_F(QtegraAcquire, ExtraDetectorIgnored) {
  override_data("PM,9,H1,1.5,L3,7");
  ASSERT_TRUE(q.start());
  EXPECT_EQ(frame().values, (std::vector<std::pair<ChannelId, double>>{{"H1", 1.5}}));
}

TEST_F(QtegraAcquire, ErrorReplyIsProtocolAndConsumesSeq) {
  ASSERT_TRUE(q.start());
  const auto first = frame().seq;
  override_data("ERROR: no data");
  clock.advance(kHalf);
  auto r = q.next(Duration::zero());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(r.error().device, "argus");
  EXPECT_EQ(q.reconnects(), 0U);
  // The failed read used up its period too: nothing more is due yet.
  expect_not_due();
  override_data("");
  clock.advance(kHalf);
  EXPECT_EQ(frame().seq, first + 2);
}

TEST_F(QtegraAcquire, NonNumericValueIsProtocol) {
  override_data("H1,1.5,AX,volts");
  ASSERT_TRUE(q.start());
  auto r = q.next(Duration::zero());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST_F(QtegraAcquire, NoReplyIsTimeoutAndIsNotRetried) {
  ASSERT_TRUE(q.start());
  sim->drop_next();
  auto r = q.next(Duration::zero());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(written_text(*sim), (std::vector<std::string>{"GetIntegrationTime\r", "GetData\r"}));
  EXPECT_EQ(q.reconnects(), 0U);
}

// GetData replies nobody has captured from an instrument. These outcomes are a
// ruling pending a bench capture, enforced by codec::qtegra::decode_data (see
// QtegraCodec.DecodeDataTagged): a reply the codec cannot pair up cleanly is a
// Protocol error and delivers no frame; names are matched case-sensitively, so
// a name in the wrong case is an unknown name and its channel is absent. No
// value is ever assigned to a channel the reply did not name exactly.
TEST_F(QtegraAcquire, GetDataEdgeReplies) {
  using Values = std::vector<std::pair<ChannelId, double>>;
  struct Case {
    std::string name;
    std::string reply;
    std::optional<Values> values;  // nullopt: Protocol error
  };
  const std::vector<Case> cases{
      {"empty reply", " ", std::nullopt},
      {"trailing comma", "H1,1.5,AX,2.5,", std::nullopt},
      {"trailing comma after a name", "H1,1.5,AX,", std::nullopt},
      {"odd field count", "H1,1.5,AX", std::nullopt},
      {"untagged values", "1.5,2.5,3.5", std::nullopt},
      {"non-numeric value", "H1,1.5,AX,volts", std::nullopt},
      {"nan", "H1,nan,AX,2.5", std::nullopt},
      {"inf", "H1,1.5,AX,inf", std::nullopt},
      {"negative infinity", "H1,-Infinity", std::nullopt},
      {"duplicate name", "H1,1.5,AX,2.5,H1,9", std::nullopt},
      {"duplicate unknown name", "PM,1,PM,2,H1,1.5", std::nullopt},
      {"lower-case names", "h1,1.5,ax,2.5", Values{}},
      {"one lower-case name", "h1,1.5,AX,2.5", Values{{"AX", 2.5}}},
  };
  ASSERT_TRUE(q.start());
  for (const auto& c : cases) {
    SCOPED_TRACE(c.name);
    override_data(c.reply);
    auto r = q.next(Duration::zero());
    if (c.values) {
      ASSERT_TRUE(r) << to_string(r.error());
      ASSERT_TRUE(r->has_value());
      EXPECT_EQ((*r)->values, *c.values);
    } else {
      ASSERT_FALSE(r);
      EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
    }
    clock.advance(kHalf);
  }
}

// --- stop() against a next() blocked in the wire read -----------------------------

namespace {

// Holds a GetData inside the transport until released.
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  bool armed = false;
  bool entered = false;
  bool released = false;

  void wait_entered() {
    std::unique_lock lock(mutex);
    cv.wait(lock, [this] { return entered; });
  }
  void release() {
    {
      std::lock_guard lock(mutex);
      released = true;
    }
    cv.notify_all();
  }
};

// Forwards to a transport, calling `on_exchange` first on the caller's thread.
class TapTransport final : public Transport {
 public:
  explicit TapTransport(Transport& inner) : inner_(inner) {}

  const std::string& name() const override { return inner_.name(); }
  Result<void> open() override { return inner_.open(); }
  void close() override { inner_.close(); }
  Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout) override {
    if (on_exchange) on_exchange(tx);
    return inner_.exchange(std::move(tx), std::move(rs), timeout);
  }
  Result<void> write(Bytes tx) override { return inner_.write(std::move(tx)); }
  Result<Bytes> read(ReadSpec rs, Duration timeout) override { return inner_.read(std::move(rs), timeout); }
  Result<void> transaction(std::function<Result<void>()> body) override { return inner_.transaction(std::move(body)); }
  Health health() const override { return inner_.health(); }

  std::function<void(const Bytes&)> on_exchange;  // set before any other thread runs

 private:
  Transport& inner_;
};

struct QtegraBlockedRead : ::testing::Test {
  ManualClock clock;
  std::shared_ptr<QtegraSimModel> model = make_model();
  Gate gate;
  std::unique_ptr<SimTransport> sim = open_hooked([this, inner = qtegra_sim_hook(model)](const Bytes& tx) {
    if (to_string(tx) == "GetData\r") {
      std::unique_lock lock(gate.mutex);
      if (gate.armed) {
        gate.armed = false;
        gate.entered = true;
        gate.cv.notify_all();
        gate.cv.wait(lock, [this] { return gate.released; });
      }
    }
    return inner(tx);
  });
  TapTransport tap{*sim};
  QtegraSpectrometer q{"argus", tap, {}, &clock};

  // Runs next() on another thread and returns once it is inside the read.
  std::thread blocked_next(Result<std::optional<Frame>>& out) {
    {
      std::lock_guard lock(gate.mutex);
      gate.armed = true;
    }
    std::thread t([&] { out = q.next(Duration::zero()); });
    gate.wait_entered();
    return t;
  }
};

}  // namespace

// The read runs outside the acquirer mutex, so stop() returns while it is
// still in flight; the frame that read produces is then dropped.
TEST_F(QtegraBlockedRead, StopDuringBlockedNextLeavesStopped) {
  ASSERT_TRUE(q.connect());
  ASSERT_TRUE(q.start());
  Result<std::optional<Frame>> blocked = std::optional<Frame>{Frame{}};
  std::thread t = blocked_next(blocked);
  ASSERT_TRUE(q.stop());  // would deadlock here if stop() waited for the read
  gate.release();
  t.join();

  ASSERT_TRUE(blocked) << to_string(blocked.error());
  EXPECT_FALSE(blocked->has_value());
  auto after = q.next(Duration::zero());
  ASSERT_FALSE(after);
  EXPECT_EQ(after.error().kind, ErrorKind::Config);
  // The dropped read still used a sequence number.
  ASSERT_TRUE(q.start());
  clock.advance(kHalf);
  auto resumed = q.next(Duration::zero());
  ASSERT_TRUE(resumed && resumed->has_value());
  EXPECT_EQ((*resumed)->seq, 2U);
}

TEST_F(QtegraBlockedRead, RestartDuringBlockedNextStillDropsTheOldFrame) {
  ASSERT_TRUE(q.connect());
  ASSERT_TRUE(q.start());
  Result<std::optional<Frame>> blocked = std::optional<Frame>{Frame{}};
  std::thread t = blocked_next(blocked);
  ASSERT_TRUE(q.stop());
  ASSERT_TRUE(q.start());
  gate.release();
  t.join();
  ASSERT_TRUE(blocked) << to_string(blocked.error());
  EXPECT_FALSE(blocked->has_value());
}

// The same for configure(): a GetData in flight when the period changes may be
// sampled on either side of the change, so it is dropped, and frames stay held
// back for the settle time.
TEST_F(QtegraBlockedRead, ConfigureDuringBlockedNextDropsTheFrameAndSettles) {
  ASSERT_TRUE(q.connect());
  ASSERT_TRUE(q.start());
  // The read is released only once configure() is about to send its command,
  // which queues behind the read on the transport.
  tap.on_exchange = [this](const Bytes& tx) {
    if (to_string(tx) == "SetIntegrationTime 1.048576\r") gate.release();
  };
  Result<std::optional<Frame>> blocked = std::optional<Frame>{Frame{}};
  std::thread t = blocked_next(blocked);
  ASSERT_TRUE(q.configure(1s));
  t.join();

  ASSERT_TRUE(blocked) << to_string(blocked.error());
  EXPECT_FALSE(blocked->has_value());
  EXPECT_EQ(sim->written().size(), 3U);  // GetIntegrationTime, GetData, SetIntegrationTime
  clock.advance(2 * kOne - kTick);
  auto early = q.next(Duration::zero());
  ASSERT_TRUE(early) << to_string(early.error());
  EXPECT_FALSE(early->has_value());
  EXPECT_EQ(sim->written().size(), 3U);
  clock.advance(kTick);
  auto settled = q.next(Duration::zero());
  ASSERT_TRUE(settled && settled->has_value());
  EXPECT_EQ((*settled)->span, kOne);
  EXPECT_EQ((*settled)->ts, TimePoint{} + 2 * kOne);
}

// --- replay ----------------------------------------------------------------------

// Synthetic trace: hand-written, not a bench capture.
TEST(QtegraReplay, ReplaysSyntheticSession) {
  auto sim = SimTransport::replay(std::string(PYCHRON_TRACE_DIR) + "/thermo/qtegra_session.trace", bus_options());
  ASSERT_TRUE(sim) << to_string(sim.error());
  ASSERT_TRUE((*sim)->open());
  QtegraSpectrometer q("argus", **sim, {});

  ASSERT_TRUE(q.connect());
  ASSERT_TRUE(q.set(4.5));
  ASSERT_TRUE(q.start());
  auto frame = q.next(Duration::zero());
  ASSERT_TRUE(frame) << to_string(frame.error());
  ASSERT_TRUE(frame->has_value());
  EXPECT_EQ((*frame)->span, kOne);
  EXPECT_EQ((*frame)->values, (std::vector<std::pair<ChannelId, double>>{
                                  {"H2", 0.0012}, {"H1", 0.0034}, {"AX", 1.25}, {"L1", 0.0051}, {"L2", -0.0002}, {"CDD", 153}}));
  auto hv = q.read_hv();
  ASSERT_TRUE(hv) << to_string(hv.error());
  EXPECT_DOUBLE_EQ(*hv, 4500.0);
  expect_verified(**sim);
}
