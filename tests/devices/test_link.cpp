#include "pychron/devices/link.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pychron/core/events.hpp"
#include "pychron/devices/device.hpp"
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
    if (open_error) return fail(*open_error);
    return {};
  }
  void close() override { ++closes; }
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
  std::atomic<int> closes{0};
  std::optional<Error> open_error;  // every open() fails with it while set
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

// --- Device::call ---------------------------------------------------------------

class Wired : public Device {
 public:
  using Device::call;
  using Device::Device;
};

struct DeviceCall : testing::VirtualTimeTest {
  VirtualClock clock;
  Clock::Participant main{clock, "test"};

  // Every exchange the transport may resend, it resends three times.
  std::unique_ptr<SimTransport> sim() {
    TransportOptions o;
    o.name = "sim1";
    o.timeout = 100ms;
    o.retries = 3;
    o.clock = &clock;
    auto t = SimTransport::hooked([](const Bytes&) { return to_bytes("ok\r\n"); }, o);
    EXPECT_TRUE(t->open());
    return t;
  }

  static std::function<Result<Bytes>(Resend)> sends(Transport& t) {
    return [&t](Resend resend) { return t.exchange(to_bytes("a"), kCrLf, Transport::kDefaultTimeout, resend); };
  }
};

TEST_F(DeviceCall, AnUnsafeCallThatTimesOutIsSentOnceByEitherLayer) {
  auto t = sim();
  ::pychron::Link link(*t, clock);
  Wired device("dev", DeviceOptions{&clock, 3, &link});
  t->drop_next(10);
  const auto r = device.call(sends(*t), Repeat::Unsafe);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(r.error().code, "outcome_unknown");
  EXPECT_EQ(t->written().size(), 1u);
  EXPECT_EQ(device.health().state, DeviceState::Degraded);
}

TEST_F(DeviceCall, ASafeCallMayBeResentByTheTransport) {
  auto t = sim();
  ::pychron::Link link(*t, clock);
  Wired device("dev", DeviceOptions{&clock, 3, &link});
  t->drop_next(1);
  const auto r = device.call(sends(*t), Repeat::Safe);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(t->written().size(), 2u);
  EXPECT_EQ(link.reconnects(), 0u);  // the transport's resend was enough
  EXPECT_EQ(device.health().state, DeviceState::Ok);
}

TEST_F(DeviceCall, ASafeCallIsRepeatedThroughTheLink) {
  auto t = sim();
  ::pychron::Link link(*t, clock);
  Wired device("dev", DeviceOptions{&clock, 3, &link});
  t->down_for(2s);
  const auto r = device.call(sends(*t), Repeat::Safe);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(link.repeats(), 1u);
  EXPECT_EQ(device.health().state, DeviceState::Ok);
}

TEST_F(DeviceCall, WithoutALinkTheCallIsMadeOnceAndObserved) {
  Wired device("dev", DeviceOptions{&clock, 3});
  int count = 0;
  std::optional<Resend> given;
  const std::function<Result<int>(Resend)> op = [&](Resend resend) -> Result<int> {
    ++count;
    given = resend;
    return fail(ErrorKind::Io, "dropped", "");
  };
  const auto r = device.call(op, Repeat::Safe);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_TRUE(r.error().code.empty());
  EXPECT_EQ(r.error().device, "dev");
  EXPECT_EQ(count, 1);
  EXPECT_EQ(given, Resend::Allowed);
  EXPECT_EQ(device.health().consecutive_failures, 1u);

  const auto unsafe = device.call(op, Repeat::Unsafe);
  ASSERT_FALSE(unsafe);
  EXPECT_EQ(given, Resend::Never);
}

// --- The cases of the Reconnector this Link replaced ------------------------------
//
// Under their old names where the behaviour is kept. Two things changed: a
// Timeout is now transient like Io (it reconnects), and the old rate limit
// (no second reconnect within 1 s of the last) is the backoff between the
// attempts of one call.

struct ReconnectTest : testing::VirtualTimeTest {
  VirtualClock clock;
  Clock::Participant main{clock, "test"};
  FakeTransport transport;
  ::pychron::Link link{transport, clock};
  std::atomic<int> connects{0};
  // While set, the connect step fails with it.
  std::optional<Error> connect_error;

  ReconnectTest() {
    link.set_connect([this](Transport&) -> Result<void> {
      ++connects;
      if (connect_error) return fail(*connect_error);
      return {};
    });
  }

  // Op that fails with `kind` for its first `failures` calls, then returns 7.
  static std::function<Result<int>()> failing(ErrorKind kind, int failures, int* calls) {
    return [=]() -> Result<int> {
      if ((*calls)++ < failures) return fail(kind, "boom", "dev");
      return 7;
    };
  }
};

TEST_F(ReconnectTest, SuccessPassesThroughWithoutReconnect) {
  int calls = 0;
  auto r = link.call(failing(ErrorKind::Io, 0, &calls), Repeat::Safe);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, 7);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(transport.opens, 0);
  EXPECT_EQ(connects, 0);
  EXPECT_EQ(link.reconnects(), 0u);
}

TEST_F(ReconnectTest, IoTriggersReopenOnConnectAndRetry) {
  int calls = 0;
  auto r = link.call(failing(ErrorKind::Io, 1, &calls), Repeat::Safe);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, 7);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(transport.closes, 1);
  EXPECT_EQ(transport.opens, 1);
  EXPECT_EQ(connects, 1);
  EXPECT_EQ(link.reconnects(), 1u);
}

TEST_F(ReconnectTest, NotConnectedAlsoTriggers) {
  int calls = 0;
  auto r = link.call(failing(ErrorKind::NotConnected, 1, &calls), Repeat::Safe);
  ASSERT_TRUE(r);
  EXPECT_EQ(transport.opens, 1);
  EXPECT_EQ(link.reconnects(), 1u);
}

// Was ProtocolAndTimeoutErrorsPassThrough: Protocol and Config still do.
TEST_F(ReconnectTest, ProtocolAndConfigErrorsPassThrough) {
  for (ErrorKind kind : {ErrorKind::Protocol, ErrorKind::Config}) {
    int calls = 0;
    auto r = link.call(failing(kind, 5, &calls), Repeat::Safe);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, kind);
    EXPECT_EQ(calls, 1);
  }
  EXPECT_EQ(transport.closes, 0);
  EXPECT_EQ(transport.opens, 0);
  EXPECT_EQ(connects, 0);
}

// ... and a Timeout, which passed through, is now transient.
TEST_F(ReconnectTest, ATimeoutTriggersReopenOnConnectAndRetry) {
  int calls = 0;
  auto r = link.call(failing(ErrorKind::Timeout, 1, &calls), Repeat::Safe);
  ASSERT_TRUE(r);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(transport.opens, 1);
  EXPECT_EQ(connects, 1);
}

// Was RateLimitedWithinMinInterval: a second reconnect inside one call waits
// backoff_min (1 s) after the first, where the old one refused it.
TEST_F(ReconnectTest, ASecondReconnectWaitsTheBackoff) {
  int calls = 0;
  const TimePoint start = clock.now();
  auto r = link.call(failing(ErrorKind::Io, 2, &calls), Repeat::Safe);
  ASSERT_TRUE(r);
  EXPECT_EQ(calls, 3);
  EXPECT_EQ(transport.opens, 2);
  EXPECT_EQ(clock.now() - start, 1s);
  EXPECT_EQ(link.reconnects(), 2u);

  // A later call starts its own recovery: its first reconnect is at once.
  calls = 0;
  const TimePoint later = clock.now();
  ASSERT_TRUE(link.call(failing(ErrorKind::Io, 1, &calls), Repeat::Safe));
  EXPECT_EQ(transport.opens, 3);
  EXPECT_EQ(clock.now(), later);
}

// Was FailedAttemptsAreRateLimitedToo: failed attempts are spaced by the
// backoff (1 s, then 2 s cut short by the window, which gets a last try).
TEST_F(ReconnectTest, FailedAttemptsBackOffToo) {
  connect_error = Error{ErrorKind::Io, "no", "dev"};
  int calls = 0;
  const TimePoint start = clock.now();
  const auto r = link.call(failing(ErrorKind::Io, 9, &calls), Repeat::Safe, 2500ms);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_EQ(r.error().what, "no");  // the last attempt's error
  EXPECT_EQ(transport.opens, 3);    // at 0, 1 and 2.5 s
  EXPECT_EQ(connects, 3);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(clock.now() - start, 2500ms);
}

// A reopen that fails definitely is returned at once; one that fails
// transiently is tried again at the backoff (Link.ASafeCallGivesUpAtItsWindow).
TEST_F(ReconnectTest, FailedReopenReturnsItsError) {
  transport.open_error = Error{ErrorKind::Config, "no such port", "dev"};
  int calls = 0;
  auto r = link.call(failing(ErrorKind::Io, 1, &calls), Repeat::Safe);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_EQ(r.error().what, "no such port");
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(connects, 0);
  EXPECT_EQ(link.reconnects(), 0u);
}

TEST_F(ReconnectTest, FailedOnConnectReturnsItsError) {
  connect_error = Error{ErrorKind::Protocol, "handshake", "dev"};
  int calls = 0;
  auto r = link.call(failing(ErrorKind::Io, 1, &calls), Repeat::Safe);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(r.error().what, "handshake");
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(link.reconnects(), 0u);
}

TEST_F(ReconnectTest, RetryFailureReturnsRetryError) {
  int n = 0;
  std::function<Result<int>()> op = [&]() -> Result<int> {
    const int attempt = ++n;
    return fail(attempt == 1 ? ErrorKind::Io : ErrorKind::Protocol, "attempt " + std::to_string(attempt), "dev");
  };
  auto r = link.call(op, Repeat::Safe);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(r.error().what, "attempt 2");
  EXPECT_EQ(transport.opens, 1);
}

TEST_F(ReconnectTest, VoidResultSupported) {
  int calls = 0;
  std::function<Result<void>()> op = [&]() -> Result<void> {
    if (calls++ == 0) return fail(ErrorKind::Io, "boom", "dev");
    return {};
  };
  EXPECT_TRUE(link.call(op, Repeat::Safe));
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(link.reconnects(), 1u);
}

TEST_F(ReconnectTest, CallerInFlightDuringAnotherReconnectOnlyRetries) {
  std::mutex m;
  std::condition_variable cv;  // waited on and notified through the clock
  bool b_started = false;
  bool b_release = false;
  int b_calls = 0;
  std::function<Result<int>()> b_op = [&]() -> Result<int> {
    const int call = ++b_calls;
    if (call == 1) {
      std::unique_lock lock(m);
      b_started = true;
      clock.notify_all(cv);
      while (!b_release) clock.wait(cv, lock);
      return fail(ErrorKind::Io, "late drop", "dev");
    }
    return fail(ErrorKind::Protocol, "retry result", "dev");
  };

  Result<int> b_result = 0;
  {
    testing::Crew crew(clock);
    crew.start("b", [&] { b_result = link.call(b_op, Repeat::Safe); });
    {
      std::unique_lock lock(m);
      while (!b_started) clock.wait(cv, lock);
    }

    // B is mid-op (on generation 0); A runs to completion and reconnects.
    int a_calls = 0;
    ASSERT_TRUE(link.call(failing(ErrorKind::Io, 1, &a_calls), Repeat::Safe));
    ASSERT_EQ(transport.opens, 1);

    {
      const std::lock_guard lock(m);
      b_release = true;
      clock.notify_all(cv);
    }
    crew.join();
  }

  EXPECT_EQ(b_calls, 2);
  ASSERT_FALSE(b_result);
  EXPECT_EQ(b_result.error().what, "retry result");
  EXPECT_EQ(transport.opens, 1);
  EXPECT_EQ(transport.closes, 1);
  EXPECT_EQ(connects, 1);
  EXPECT_EQ(link.reconnects(), 1u);
}

TEST_F(ReconnectTest, ConcurrentRunsReconnectOnce) {
  constexpr int kThreads = 4;
  std::mutex m;
  std::condition_variable cv;  // waited on and notified through the clock
  int failed = 0;
  bool reopened = false;
  link.set_connect([&](Transport&) -> Result<void> {
    ++connects;
    const std::lock_guard lock(m);
    reopened = true;
    return {};
  });
  // Every thread's first op call fails; none fails before all have tried, so
  // all of them fail against the same (pre-reconnect) generation.
  std::function<Result<int>()> op = [&]() -> Result<int> {
    std::unique_lock lock(m);
    if (reopened) return 7;
    ++failed;
    clock.notify_all(cv);
    while (failed < kThreads) clock.wait(cv, lock);
    return fail(ErrorKind::Io, "dropped", "dev");
  };
  std::atomic<int> ok{0};
  {
    testing::Crew crew(clock);
    for (int i = 0; i < kThreads; ++i) {
      crew.start("caller " + std::to_string(i), [&] {
        if (link.call(op, Repeat::Safe)) ++ok;
      });
    }
    crew.join();
  }
  EXPECT_EQ(ok, kThreads);
  EXPECT_EQ(transport.opens, 1);
  EXPECT_EQ(transport.closes, 1);
  EXPECT_EQ(connects, 1);
  EXPECT_EQ(link.reconnects(), 1u);
}

}  // namespace
}  // namespace pychron
