#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/transport.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

// Primitive-level fake: scripted results for each do_* call.
class FakeChannel final : public QueuedTransport {
 public:
  explicit FakeChannel(TransportOptions o) : QueuedTransport(std::move(o)) {}
  ~FakeChannel() override { shutdown(); }
  using QueuedTransport::shutdown;

  Result<void> open_result;
  std::deque<Result<Bytes>> reads;
  std::deque<Result<void>> writes;
  std::function<void()> on_read;

  std::atomic<int> write_calls{0}, read_calls{0}, discard_calls{0}, close_calls{0};
  std::atomic<bool> in_flight{false}, overlap{false};
  Duration last_timeout{};

 private:
  Result<void> do_open() override { return open_result; }
  void do_close() override { ++close_calls; }
  void do_discard_input() override { ++discard_calls; }
  Result<void> do_write(const Bytes&, Duration) override {
    if (in_flight.exchange(true)) overlap = true;
    ++write_calls;
    if (writes.empty()) return {};
    auto r = writes.front();
    writes.pop_front();
    return r;
  }
  Result<Bytes> do_read(const ReadSpec&, Duration timeout) override {
    ++read_calls;
    last_timeout = timeout;
    if (on_read) on_read();
    in_flight = false;
    if (reads.empty()) return fail(ErrorKind::Timeout, "no reply");
    auto r = reads.front();
    reads.pop_front();
    return r;
  }
};

TransportOptions opts(int retries = 0, std::uint64_t down_after = 3) {
  TransportOptions o;
  o.name = "bus1";
  o.timeout = 250ms;
  o.retries = retries;
  o.down_after = down_after;
  return o;
}

const ReadSpec kCr = ReadSpec::until("\r");

// Waits, in real time, until `n` threads are asleep in the clock.
[[nodiscard]] bool await_waiters(const VirtualClock& clock, std::size_t n) {
  const auto give_up = std::chrono::steady_clock::now() + 5s;
  while (clock.waiters() != n) {
    if (std::chrono::steady_clock::now() > give_up) return false;
    std::this_thread::yield();
  }
  return true;
}

// A transport that waits past the clock leaves time standing and the test
// asleep in it. Each test on a VirtualClock runs under a real-time bound: when
// it is exceeded the process says so and aborts, well inside the ctest timeout.
class QueuedTransportVirtual : public ::testing::Test {
 protected:
  ~QueuedTransportVirtual() override {
    {
      std::lock_guard lock(mutex_);
      finished_ = true;
    }
    finished_cv_.notify_all();
    deadman_.join();
  }

 private:
  std::mutex mutex_;
  std::condition_variable finished_cv_;
  bool finished_ = false;
  std::thread deadman_{[this] {
    std::unique_lock lock(mutex_);
    if (finished_cv_.wait_for(lock, 30s, [this] { return finished_; })) return;
    std::fputs("QueuedTransport test did not finish within 30 s of real time: a thread is stuck\n",
               stderr);
    std::abort();
  }};
};

}  // namespace

TEST(QueuedTransport, ExchangeBeforeOpenIsNotConnected) {
  FakeChannel t(opts());
  auto r = t.exchange(to_bytes("x"), kCr);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::NotConnected);
  EXPECT_EQ(r.error().device, "bus1");
  EXPECT_EQ(t.health().state, HealthState::Down);
  EXPECT_FALSE(t.write(to_bytes("x")));
  EXPECT_FALSE(t.read(kCr));
}

TEST(QueuedTransport, ExchangeDiscardsWritesThenReads) {
  ManualClock clock(TimePoint{} + 10s);
  auto o = opts();
  o.clock = &clock;
  FakeChannel t(o);
  ASSERT_TRUE(t.open());
  t.reads.push_back(to_bytes("OK\r"));
  auto r = t.exchange(to_bytes("PR1\r"), kCr);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(to_string(*r), "OK\r");
  EXPECT_EQ(t.discard_calls, 1);
  EXPECT_EQ(t.write_calls, 1);
  const auto h = t.health();
  EXPECT_EQ(h.state, HealthState::Connected);
  EXPECT_EQ(h.last_ok, TimePoint{} + 10s);
  EXPECT_EQ(h.consecutive_failures, 0u);
}

TEST(QueuedTransport, ZeroTimeoutUsesConfiguredDefault) {
  FakeChannel t(opts());
  ASSERT_TRUE(t.open());
  t.reads.push_back(Bytes{});
  ASSERT_TRUE(t.exchange(to_bytes("a"), kCr));
  EXPECT_EQ(t.last_timeout, Duration(250ms));
  t.reads.push_back(Bytes{});
  ASSERT_TRUE(t.exchange(to_bytes("a"), kCr, 40ms));
  EXPECT_EQ(t.last_timeout, Duration(40ms));
}

TEST(QueuedTransport, RetriesTimeoutThenSucceeds) {
  FakeChannel t(opts(/*retries=*/2));
  ASSERT_TRUE(t.open());
  t.reads.push_back(fail(ErrorKind::Timeout, "t1"));
  t.reads.push_back(fail(ErrorKind::Io, "glitch"));
  t.reads.push_back(to_bytes("OK\r"));
  auto r = t.exchange(to_bytes("q"), kCr);
  ASSERT_TRUE(r);
  EXPECT_EQ(t.write_calls, 3);
  EXPECT_EQ(t.discard_calls, 3);
  EXPECT_EQ(t.health().state, HealthState::Connected);
}

TEST(QueuedTransport, RetriesExhaustedReportsLastErrorOnce) {
  FakeChannel t(opts(/*retries=*/1));
  ASSERT_TRUE(t.open());
  auto r = t.exchange(to_bytes("q"), kCr);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(r.error().device, "bus1");
  EXPECT_EQ(t.write_calls, 2);
  const auto h = t.health();
  EXPECT_EQ(h.state, HealthState::Degraded);
  EXPECT_EQ(h.consecutive_failures, 1u);
  EXPECT_NE(h.last_error.find("no reply"), std::string::npos);
}

TEST(QueuedTransport, ProtocolErrorsAreNotRetried) {
  FakeChannel t(opts(/*retries=*/3));
  ASSERT_TRUE(t.open());
  t.writes.push_back(fail(ErrorKind::Protocol, "rejected"));
  auto r = t.exchange(to_bytes("q"), kCr);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(t.write_calls, 1);
  EXPECT_EQ(t.read_calls, 0);
}

TEST(QueuedTransport, WriteIsRetriedOnIo) {
  FakeChannel t(opts(/*retries=*/1));
  ASSERT_TRUE(t.open());
  t.writes.push_back(fail(ErrorKind::Io, "EAGAIN"));
  EXPECT_TRUE(t.write(to_bytes("w")));
  EXPECT_EQ(t.write_calls, 2);
}

TEST(QueuedTransport, GoesDownAfterThresholdAndRecovers) {
  FakeChannel t(opts(0, /*down_after=*/2));
  ASSERT_TRUE(t.open());
  EXPECT_FALSE(t.exchange(to_bytes("q"), kCr));
  EXPECT_EQ(t.health().state, HealthState::Degraded);
  EXPECT_FALSE(t.exchange(to_bytes("q"), kCr));
  EXPECT_EQ(t.health().state, HealthState::Down);
  EXPECT_EQ(t.health().consecutive_failures, 2u);
  t.reads.push_back(to_bytes("OK\r"));
  EXPECT_TRUE(t.exchange(to_bytes("q"), kCr));
  EXPECT_EQ(t.health().state, HealthState::Connected);
  EXPECT_EQ(t.health().consecutive_failures, 0u);
}

TEST(QueuedTransport, OpenFailureLeavesTransportDown) {
  FakeChannel t(opts());
  t.open_result = fail(ErrorKind::Io, "no such port");
  auto r = t.open();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().device, "bus1");
  EXPECT_EQ(t.health().state, HealthState::Down);
  EXPECT_NE(t.health().last_error.find("no such port"), std::string::npos);
  EXPECT_EQ(t.exchange(to_bytes("q"), kCr).error().kind, ErrorKind::NotConnected);
}

TEST(QueuedTransport, CloseMakesFurtherCallsNotConnected) {
  FakeChannel t(opts());
  ASSERT_TRUE(t.open());
  t.close();
  EXPECT_EQ(t.close_calls, 1);
  EXPECT_EQ(t.health().state, HealthState::Down);
  EXPECT_EQ(t.exchange(to_bytes("q"), kCr).error().kind, ErrorKind::NotConnected);
  t.close();  // idempotent
  EXPECT_EQ(t.close_calls, 1);
}

TEST(QueuedTransport, PublishesHealthOnStateChange) {
  SignalBus bus;
  std::vector<TransportHealth> events;
  auto sub = bus.subscribe<TransportHealth>([&](const TransportHealth& e) { events.push_back(e); });
  auto o = opts(0, 2);
  o.bus = &bus;
  FakeChannel t(o);
  ASSERT_TRUE(t.open());
  ASSERT_EQ(events.size(), 1u);
  EXPECT_TRUE(events[0].connected);
  EXPECT_EQ(events[0].transport, "bus1");

  (void)t.exchange(to_bytes("q"), kCr);  // Degraded
  (void)t.exchange(to_bytes("q"), kCr);  // Down
  (void)t.exchange(to_bytes("q"), kCr);  // still Down: no event
  ASSERT_EQ(events.size(), 3u);
  EXPECT_TRUE(events[1].connected);
  EXPECT_EQ(events[1].error_count, 1u);
  EXPECT_FALSE(events[2].connected);
  EXPECT_EQ(events[2].error_count, 2u);
  EXPECT_FALSE(events[2].last_error.empty());
}

TEST(QueuedTransport, ConcurrentExchangesAreSerialized) {
  FakeChannel t(opts());
  ASSERT_TRUE(t.open());
  constexpr int kThreads = 8, kEach = 50;
  for (int i = 0; i < kThreads * kEach; ++i) t.reads.push_back(to_bytes("OK\r"));
  t.on_read = [] { std::this_thread::yield(); };
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < kEach; ++j)
        if (t.exchange(to_bytes("q"), kCr)) ++ok;
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(ok, kThreads * kEach);
  EXPECT_FALSE(t.overlap);
}

TEST(QueuedTransport, ShutdownCancelsQueuedAndLaterCalls) {
  FakeChannel t(opts());
  ASSERT_TRUE(t.open());
  std::promise<void> entered, release;
  auto release_f = release.get_future().share();
  std::atomic<bool> first_read{true};
  t.on_read = [&, release_f] {
    if (!first_read.exchange(false)) return;
    entered.set_value();
    release_f.wait();
  };
  t.reads.push_back(to_bytes("first\r"));

  auto first = std::async(std::launch::async, [&] { return t.exchange(to_bytes("1"), kCr); });
  entered.get_future().wait();  // worker is now busy with the first call
  auto second = std::async(std::launch::async, [&] { return t.exchange(to_bytes("2"), kCr); });
  std::this_thread::sleep_for(50ms);  // let the second call enqueue

  std::thread stopper([&] { t.shutdown(); });
  std::this_thread::sleep_for(20ms);
  release.set_value();
  stopper.join();

  EXPECT_TRUE(first.get());
  auto r2 = second.get();
  ASSERT_FALSE(r2);
  EXPECT_EQ(r2.error().kind, ErrorKind::Cancelled);
  EXPECT_EQ(t.exchange(to_bytes("3"), kCr).error().kind, ErrorKind::Cancelled);
  EXPECT_EQ(t.close_calls, 1);
}

TEST_F(QueuedTransportVirtual, CallerWaitsForTheWorkerInClockTime) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  auto o = opts();
  o.clock = &clock;
  auto t = SimTransport::hooked(
      [&](const Bytes&) {
        clock.sleep_for(2s);  // the instrument takes its time, on the worker
        return to_bytes("OK\r");
      },
      o);
  ASSERT_TRUE(t->open());

  const auto real_start = std::chrono::steady_clock::now();
  auto r = t->exchange(to_bytes("Q\r"), kCr);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(to_string(*r), "OK\r");
  EXPECT_EQ(clock.now(), kStart + 2s);
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 200ms);
}

// The call the worker is busy with is finished, as on any clock; the one
// queued behind it is the pending call, and it is woken with Cancelled at
// once, without the clock moving.
TEST_F(QueuedTransportVirtual, ShutdownCancelsAPendingCall) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");  // runnable until it sleeps: time stands
  const TimePoint kStart = clock.now();
  auto o = opts();
  o.clock = &clock;
  auto t = SimTransport::hooked(
      [&](const Bytes&) {
        clock.sleep_for(2s);
        return to_bytes("OK\r");
      },
      o);
  ASSERT_TRUE(t->open());
  SimTransport* const raw = t.get();

  auto first = std::async(std::launch::async, [&clock, raw] {
    Clock::Participant caller(clock, "first");
    return raw->exchange(to_bytes("1\r"), kCr);
  });
  ASSERT_TRUE(await_waiters(clock, 2));  // the worker in the hook, and its caller
  auto second = std::async(std::launch::async, [&clock, raw] {
    Clock::Participant caller(clock, "second");
    return raw->exchange(to_bytes("2\r"), kCr);
  });
  ASSERT_TRUE(await_waiters(clock, 3));  // and the caller queued behind it

  std::thread destroyer([&t] { t.reset(); });  // not a participant
  auto r2 = second.get();
  ASSERT_FALSE(r2);
  EXPECT_EQ(r2.error().kind, ErrorKind::Cancelled);
  EXPECT_EQ(clock.now(), kStart);

  clock.sleep_for(2s);  // lets the worker out of the hook
  auto r1 = first.get();
  ASSERT_TRUE(r1) << r1.error().what;
  EXPECT_EQ(to_string(*r1), "OK\r");
  destroyer.join();
  EXPECT_EQ(clock.now(), kStart + 2s);
}
