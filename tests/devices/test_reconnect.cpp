#include "pychron/devices/reconnect.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "pychron/transport/sim_transport.hpp"

namespace pychron {
namespace {

// Counts open/close calls; nothing else is ever used.
class CountingTransport final : public Transport {
 public:
  const std::string& name() const override { return name_; }
  Result<void> open() override {
    ++opens;
    return {};
  }
  void close() override { ++closes; }
  Result<Bytes> exchange(Bytes, ReadSpec, Duration) override { return Bytes{}; }
  Result<void> write(Bytes) override { return {}; }
  Result<Bytes> read(ReadSpec, Duration) override { return Bytes{}; }
  Result<void> transaction(std::function<Result<void>()> body) override { return body(); }
  Health health() const override { return {}; }

  std::atomic<int> opens{0};
  std::atomic<int> closes{0};

 private:
  std::string name_ = "counting";
};

struct ReconnectTest : ::testing::Test {
  ManualClock clock;
  CountingTransport transport;
  Reconnector reconnector{transport, clock};
  int connects = 0;
  std::function<Result<void>()> on_connect = [this]() -> Result<void> {
    ++connects;
    return {};
  };

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
  auto r = reconnector.run<int>(failing(ErrorKind::Io, 0, &calls), on_connect);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, 7);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(transport.opens, 0);
  EXPECT_EQ(connects, 0);
  EXPECT_EQ(reconnector.reconnects(), 0u);
}

TEST_F(ReconnectTest, IoTriggersReopenOnConnectAndRetry) {
  int calls = 0;
  auto r = reconnector.run<int>(failing(ErrorKind::Io, 1, &calls), on_connect);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, 7);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(transport.closes, 1);
  EXPECT_EQ(transport.opens, 1);
  EXPECT_EQ(connects, 1);
  EXPECT_EQ(reconnector.reconnects(), 1u);
}

TEST_F(ReconnectTest, NotConnectedAlsoTriggers) {
  int calls = 0;
  auto r = reconnector.run<int>(failing(ErrorKind::NotConnected, 1, &calls), on_connect);
  ASSERT_TRUE(r);
  EXPECT_EQ(transport.opens, 1);
  EXPECT_EQ(reconnector.reconnects(), 1u);
}

TEST_F(ReconnectTest, ProtocolAndTimeoutErrorsPassThrough) {
  for (ErrorKind kind : {ErrorKind::Protocol, ErrorKind::Timeout, ErrorKind::Config}) {
    int calls = 0;
    auto r = reconnector.run<int>(failing(kind, 5, &calls), on_connect);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, kind);
    EXPECT_EQ(calls, 1);
  }
  EXPECT_EQ(transport.closes, 0);
  EXPECT_EQ(transport.opens, 0);
  EXPECT_EQ(connects, 0);
}

TEST_F(ReconnectTest, RateLimitedWithinMinInterval) {
  int calls = 0;
  ASSERT_TRUE(reconnector.run<int>(failing(ErrorKind::Io, 1, &calls), on_connect));
  EXPECT_EQ(transport.opens, 1);

  clock.advance(std::chrono::milliseconds(500));
  calls = 0;
  auto limited = reconnector.run<int>(failing(ErrorKind::Io, 1, &calls), on_connect);
  ASSERT_FALSE(limited);
  EXPECT_EQ(limited.error().kind, ErrorKind::Io);
  EXPECT_EQ(limited.error().what, "boom");
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(transport.opens, 1);
  EXPECT_EQ(transport.closes, 1);

  clock.advance(std::chrono::milliseconds(600));
  calls = 0;
  ASSERT_TRUE(reconnector.run<int>(failing(ErrorKind::Io, 1, &calls), on_connect));
  EXPECT_EQ(transport.opens, 2);
  EXPECT_EQ(reconnector.reconnects(), 2u);
}

TEST_F(ReconnectTest, FailedAttemptsAreRateLimitedToo) {
  int calls = 0;
  auto bad_connect = []() -> Result<void> { return fail(ErrorKind::Io, "no", "dev"); };
  ASSERT_FALSE(reconnector.run<int>(failing(ErrorKind::Io, 9, &calls), bad_connect));
  EXPECT_EQ(transport.opens, 1);
  clock.advance(std::chrono::milliseconds(100));
  ASSERT_FALSE(reconnector.run<int>(failing(ErrorKind::Io, 9, &calls), on_connect));
  EXPECT_EQ(transport.opens, 1);
  EXPECT_EQ(connects, 0);
}

TEST_F(ReconnectTest, FailedReopenReturnsItsError) {
  auto sim = SimTransport::scripted({});
  Reconnector rc(*sim, clock);
  sim->fail_open_next();
  int calls = 0;
  auto r = rc.run<int>(failing(ErrorKind::Io, 1, &calls), on_connect);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_NE(r.error().what, "boom");
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(connects, 0);
  EXPECT_EQ(rc.reconnects(), 0u);
}

TEST_F(ReconnectTest, FailedOnConnectReturnsItsError) {
  auto bad_connect = []() -> Result<void> { return fail(ErrorKind::Protocol, "handshake", "dev"); };
  int calls = 0;
  auto r = reconnector.run<int>(failing(ErrorKind::Io, 1, &calls), bad_connect);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(r.error().what, "handshake");
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(reconnector.reconnects(), 0u);
}

TEST_F(ReconnectTest, RetryFailureReturnsRetryError) {
  int n = 0;
  std::function<Result<int>()> op = [&]() -> Result<int> {
    const int attempt = ++n;
    return fail(attempt == 1 ? ErrorKind::Io : ErrorKind::Timeout, "attempt " + std::to_string(attempt), "dev");
  };
  auto r = reconnector.run<int>(op, on_connect);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(r.error().what, "attempt 2");
  EXPECT_EQ(transport.opens, 1);
}

TEST_F(ReconnectTest, VoidResultSupported) {
  int calls = 0;
  std::function<Result<void>()> op = [&]() -> Result<void> {
    if (calls++ == 0) return fail(ErrorKind::Io, "boom", "dev");
    return {};
  };
  EXPECT_TRUE(reconnector.run<void>(op, on_connect));
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(reconnector.reconnects(), 1u);
}

TEST_F(ReconnectTest, ConcurrentRunsReconnectOnce) {
  constexpr int kThreads = 4;
  std::atomic<bool> reopened{false};
  std::atomic<int> started{0};
  std::function<Result<void>()> connect = [&]() -> Result<void> {
    ++connects;
    reopened = true;
    return {};
  };
  // Every thread's first op call fails; none may fail before all have
  // started, so all of them fail against the same (pre-reconnect) generation.
  std::function<Result<int>()> op = [&]() -> Result<int> {
    if (!reopened) {
      ++started;
      while (started < kThreads && !reopened) std::this_thread::yield();
      if (!reopened) return fail(ErrorKind::Io, "dropped", "dev");
    }
    return 7;
  };
  std::vector<std::thread> threads;
  std::atomic<int> ok{0};
  for (int i = 0; i < kThreads; ++i)
    threads.emplace_back([&] {
      if (reconnector.run<int>(op, connect)) ++ok;
    });
  for (auto& t : threads) t.join();
  EXPECT_EQ(ok, kThreads);
  EXPECT_EQ(transport.opens, 1);
  EXPECT_EQ(transport.closes, 1);
  EXPECT_EQ(connects, 1);
  EXPECT_EQ(reconnector.reconnects(), 1u);
}

}  // namespace
}  // namespace pychron
