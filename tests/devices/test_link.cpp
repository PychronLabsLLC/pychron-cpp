#include "pychron/devices/link.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "virtual_time.hpp"

namespace pychron {
namespace {

using namespace std::chrono_literals;

const ReadSpec kCrLf = ReadSpec::until("\r\n");

// Notes when each open() was asked for; everything else goes to `inner`.
class OpenTimes final : public Transport {
 public:
  OpenTimes(Transport& inner, const Clock& clock) : inner_(inner), clock_(clock) {}
  const std::string& name() const override { return inner_.name(); }
  Result<void> open() override {
    times.push_back(clock_.now());
    return inner_.open();
  }
  void close() override { inner_.close(); }
  Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout, Resend resend) override {
    return inner_.exchange(std::move(tx), std::move(rs), timeout, resend);
  }
  Result<void> write(Bytes tx) override { return inner_.write(std::move(tx)); }
  Result<Bytes> read(ReadSpec rs, Duration timeout) override { return inner_.read(std::move(rs), timeout); }
  Result<void> transaction(std::function<Result<void>()> body) override {
    return inner_.transaction(std::move(body));
  }
  Health health() const override { return inner_.health(); }

  // Written on the inner transport's worker, read once the call has returned.
  std::vector<TimePoint> times;

 private:
  Transport& inner_;
  const Clock& clock_;
};

// No wire: counts open() and runs each transaction inline, after running
// `before_next_transaction` once if it is set.
class FakeTransport final : public Transport {
 public:
  const std::string& name() const override { return name_; }
  Result<void> open() override {
    ++opens;
    return {};
  }
  void close() override {}
  Result<Bytes> exchange(Bytes, ReadSpec, Duration, Resend) override { return Bytes{}; }
  Result<void> write(Bytes) override { return {}; }
  Result<Bytes> read(ReadSpec, Duration) override { return Bytes{}; }
  Result<void> transaction(std::function<Result<void>()> body) override {
    if (before_next_transaction) {
      auto before = std::move(before_next_transaction);
      before_next_transaction = nullptr;
      before();
    }
    return body();
  }
  Health health() const override { return {}; }

  std::atomic<int> opens{0};
  std::function<void()> before_next_transaction;

 private:
  std::string name_ = "fake";
};

struct Link : testing::VirtualTimeTest {
  VirtualClock clock;
  Clock::Participant main{clock, "test"};

  std::unique_ptr<SimTransport> sim() {
    TransportOptions o;
    o.name = "sim1";
    o.timeout = 100ms;
    o.clock = &clock;
    auto t = SimTransport::hooked([](const Bytes&) { return to_bytes("ok\r\n"); }, o);
    EXPECT_TRUE(t->open());
    return t;
  }

  static std::function<Result<Bytes>()> sends(Transport& t, int* count) {
    return [&t, count]() -> Result<Bytes> {
      ++*count;
      return t.exchange(to_bytes("a"), kCrLf);
    };
  }
};

TEST_F(Link, ASafeCallIsRepeatedUntilTheLinkReturns) {
  auto t = sim();
  ::pychron::Link link(*t, clock);
  t->down_for(7s);
  int count = 0;
  const TimePoint start = clock.now();
  const auto r = link.call(sends(*t, &count), Repeat::Safe);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(to_string(*r), "ok\r\n");
  EXPECT_EQ(count, 2);
  EXPECT_EQ(link.reconnects(), 1u);
  EXPECT_EQ(link.repeats(), 1u);
  // Reopens at 0, 1, 3 and 7 s: the backoff was 4 s when the link returned.
  EXPECT_GE(clock.now() - start, 7s);
  EXPECT_LE(clock.now() - start, 7s + 4s);
}

TEST_F(Link, ASafeCallGivesUpAtItsWindow) {
  auto t = sim();
  ::pychron::Link link(*t, clock);
  t->set_down(true);
  int count = 0;
  const TimePoint start = clock.now();
  const auto r = link.call(sends(*t, &count), Repeat::Safe);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_TRUE(r.error().code.empty());
  EXPECT_EQ(count, 1);
  EXPECT_EQ(link.reconnects(), 0u);
  EXPECT_GE(clock.now() - start, link.policy().call_window);
  EXPECT_LE(clock.now() - start, link.policy().call_window + link.policy().backoff_max);
}

TEST_F(Link, BackoffDoublesFromMinToMax) {
  auto t = sim();
  OpenTimes opens(*t, clock);
  ::pychron::Link link(opens, clock);
  t->set_down(true);
  int count = 0;
  const auto r = link.call(sends(opens, &count), Repeat::Safe, 100s);
  ASSERT_FALSE(r);
  ASSERT_GE(opens.times.size(), 8u);
  const std::vector<Duration> expected{1s, 2s, 4s, 8s, 16s, 30s, 30s};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(opens.times[i + 1] - opens.times[i], expected[i]) << "between attempts " << i << " and " << i + 1;
  }
}

TEST_F(Link, ACallThatFailsAfterEachReconnectEndsAtItsWindow) {
  FakeTransport transport;
  ::pychron::Link link(transport, clock);
  int count = 0;
  std::function<Result<int>()> op = [&]() -> Result<int> {
    ++count;
    return fail(ErrorKind::Io, "accepted, then dropped", "dev");
  };
  const TimePoint start = clock.now();
  const auto r = link.call(op, Repeat::Safe);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_EQ(clock.now() - start, link.policy().call_window);
  // Reconnects at 0, 1, 3, 7, 15 and 20 s, each followed by a repeat.
  EXPECT_EQ(transport.opens, 6);
  EXPECT_EQ(count, 7);
}

TEST_F(Link, AZeroBackoffStillLetsTimePass) {
  FakeTransport transport;
  LinkPolicy policy;
  policy.backoff_min = Duration::zero();
  policy.backoff_max = Duration::zero();
  ::pychron::Link link(transport, clock, policy);
  std::function<Result<int>()> op = []() -> Result<int> { return fail(ErrorKind::Io, "dropped", "dev"); };
  const TimePoint start = clock.now();
  EXPECT_FALSE(link.call(op, Repeat::Safe, 10ms));
  EXPECT_EQ(clock.now() - start, 10ms);
}

TEST_F(Link, AnUnsafeCallIsSentOnce) {
  auto t = sim();
  ::pychron::Link link(*t, clock);
  int count = 0;
  std::function<Result<Bytes>()> op = [&]() -> Result<Bytes> {
    if (count++ == 0) return fail(ErrorKind::Io, "dropped", "dev");
    return t->exchange(to_bytes("a"), kCrLf);
  };
  const auto r = link.call(op, Repeat::Unsafe);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_EQ(r.error().code, "outcome_unknown");
  EXPECT_EQ(r.error().what, "dropped");
  EXPECT_EQ(count, 1);
  EXPECT_EQ(link.reconnects(), 1u);
  EXPECT_EQ(link.repeats(), 0u);
}

TEST_F(Link, AnUnsafeCallIsUnknownWithoutAReconnectToo) {
  auto t = sim();
  LinkPolicy policy;
  policy.reconnect = false;
  ::pychron::Link link(*t, clock, policy);
  int count = 0;
  std::function<Result<Bytes>()> op = [&]() -> Result<Bytes> {
    ++count;
    return fail(ErrorKind::Timeout, "no reply", "dev");
  };
  const auto r = link.call(op, Repeat::Unsafe);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(r.error().code, "outcome_unknown");
  EXPECT_EQ(count, 1);
  EXPECT_EQ(link.reconnects(), 0u);
}

TEST_F(Link, ADefiniteErrorIsNeverRepeated) {
  auto t = sim();
  OpenTimes opens(*t, clock);
  ::pychron::Link link(opens, clock);
  for (const ErrorKind kind : {ErrorKind::Protocol, ErrorKind::Config, ErrorKind::Interlock, ErrorKind::Cancelled}) {
    for (const Repeat repeat : {Repeat::Safe, Repeat::Unsafe}) {
      int count = 0;
      std::function<Result<int>()> op = [&]() -> Result<int> {
        ++count;
        return fail(kind, "no", "dev");
      };
      const TimePoint start = clock.now();
      const auto r = link.call(op, repeat);
      ASSERT_FALSE(r);
      EXPECT_EQ(r.error().kind, kind);
      EXPECT_TRUE(r.error().code.empty());
      EXPECT_EQ(count, 1);
      EXPECT_EQ(clock.now(), start);
    }
  }
  EXPECT_TRUE(opens.times.empty());
  EXPECT_EQ(link.repeats(), 0u);
}

TEST_F(Link, TwoThreadsOneReconnect) {
  FakeTransport transport;
  ::pychron::Link link(transport, clock);
  std::mutex m;
  std::condition_variable cv;  // waited on and notified through the clock
  int failed = 0;
  bool reconnected = false;
  link.set_connect([&](Transport&) -> Result<void> {
    const std::lock_guard lock(m);
    reconnected = true;
    return {};
  });
  // Neither caller's first try fails before both have tried, so both fail
  // on the connection the reconnect replaces.
  std::function<Result<int>()> op = [&]() -> Result<int> {
    std::unique_lock lock(m);
    if (reconnected) return 7;
    ++failed;
    clock.notify_all(cv);
    while (failed < 2) clock.wait(cv, lock);
    return fail(ErrorKind::Io, "dropped", "dev");
  };
  std::atomic<int> ok{0};
  {
    testing::Crew crew(clock);
    for (const char* name : {"a", "b"}) {
      crew.start(name, [&] {
        if (link.call(op, Repeat::Safe)) ++ok;
      });
    }
    crew.join();
  }
  EXPECT_EQ(ok, 2);
  EXPECT_EQ(transport.opens, 1);
  EXPECT_EQ(link.reconnects(), 1u);
  EXPECT_EQ(link.repeats(), 2u);
}

TEST_F(Link, AReconnectThatStartedAfterMyFailureIsNotSkipped) {
  FakeTransport transport;
  ::pychron::Link link(transport, clock);
  int a_calls = 0;
  std::function<Result<int>()> a = [&]() -> Result<int> {
    if (a_calls++ == 0) return fail(ErrorKind::Io, "first drop", "dev");
    return 1;
  };
  int b_calls = 0;
  std::function<Result<int>()> b = [&]() -> Result<int> {
    if (b_calls++ == 0) return fail(ErrorKind::Io, "drop on the new connection", "dev");
    return 2;
  };
  // B asks first, but A's call (a failure and its reconnect) runs before B's
  // op reaches the transport: B's failure is on the connection A made.
  transport.before_next_transaction = [&] {
    const auto r = link.call(a, Repeat::Safe);
    EXPECT_TRUE(r);
  };
  const auto r = link.call(b, Repeat::Safe);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(*r, 2);
  EXPECT_EQ(b_calls, 2);
  EXPECT_EQ(transport.opens, 2);
  EXPECT_EQ(link.reconnects(), 2u);
}

TEST_F(Link, EachReconnectIsPublished) {
  SignalBus bus;
  std::vector<LinkReconnected> seen;
  auto sub = bus.subscribe<LinkReconnected>([&](const LinkReconnected& e) { seen.push_back(e); });
  auto t = sim();
  ::pychron::Link link(*t, clock, {}, &bus);
  t->down_for(7s);
  int count = 0;
  ASSERT_TRUE(link.call(sends(*t, &count), Repeat::Safe));
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].transport, "sim1");
  EXPECT_EQ(seen[0].attempts, 4u);
  EXPECT_EQ(seen[0].down_for, 7s);
  EXPECT_EQ(seen[0].ts, clock.now());
}

TEST_F(Link, NoReconnectWhenPolicySaysSo) {
  auto t = sim();
  OpenTimes opens(*t, clock);
  LinkPolicy policy;
  policy.reconnect = false;
  ::pychron::Link link(opens, clock, policy);
  t->set_down(true);
  int count = 0;
  const TimePoint start = clock.now();
  const auto r = link.call(sends(opens, &count), Repeat::Safe);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_EQ(count, 1);
  EXPECT_TRUE(opens.times.empty());
  EXPECT_EQ(clock.now(), start);
}

TEST_F(Link, AProbeTriesOnceAndReconnectsOnce) {
  auto t = sim();
  OpenTimes opens(*t, clock);
  ::pychron::Link link(opens, clock);
  t->set_down(true);
  ASSERT_FALSE(t->exchange(to_bytes("a"), kCrLf));  // the drop closes the channel
  const TimePoint start = clock.now();
  const auto down = link.probe();
  ASSERT_FALSE(down);
  EXPECT_EQ(down.error().kind, ErrorKind::Io);
  EXPECT_EQ(clock.now(), start);
  EXPECT_EQ(opens.times.size(), 2u);  // the probe's own open, and one reconnect

  t->set_down(false);
  EXPECT_TRUE(link.probe());
  EXPECT_EQ(link.reconnects(), 0u);  // the probe's open was enough
}

}  // namespace
}  // namespace pychron
