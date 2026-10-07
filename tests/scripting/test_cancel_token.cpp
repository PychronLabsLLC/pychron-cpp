// CancelToken: a cancel, an abort and a wake reach a thread that waits on a
// clock at the instant they are made, and a cancel or an abort calls the
// callbacks registered on the token.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

#include "pychron/core/virtual_clock.hpp"
#include "pychron/scripting/cancel_token.hpp"
#include "virtual_time.hpp"

using namespace pychron;
using namespace std::chrono_literals;
using pychron::testing::await_waiters;
using pychron::testing::Crew;
using pychron::testing::eventually_real;
using scripting::CancelMode;
using scripting::CancelToken;
using scripting::WaitResult;

namespace {

class CancelTokenVirtual : public pychron::testing::VirtualTimeTest {
 protected:
  // One participant asleep in the token until `deadline`. With `held`, time
  // does not jump meanwhile: the test, which is not a participant, has
  // something to do first.
  void start_waiter(TimePoint deadline, bool held) {
    if (held) hold_ = std::make_unique<Clock::Hold>(clock_);
    crew_.start("waiter", [this, deadline] {
      const auto r = token_.wait_until(clock_, deadline);
      std::lock_guard lock(mutex_);
      result_ = r;
      woke_at_ = clock_.now();
    });
  }

  // Lets time go and waits for the waiter to return.
  void finish() {
    hold_.reset();
    crew_.join();
  }

  VirtualClock clock_;  // first: everything below is given a reference to it
  const TimePoint kStart = clock_.now();
  CancelToken token_;
  std::mutex mutex_;
  std::optional<WaitResult> result_;
  TimePoint woke_at_{};
  std::unique_ptr<Clock::Hold> hold_;
  Crew crew_{clock_};
};

TEST_F(CancelTokenVirtual, CancelWakesAClockWaiter) {
  start_waiter(kStart + 1h, true);
  ASSERT_TRUE(await_waiters(clock_, 1));
  token_.cancel();
  finish();
  ASSERT_TRUE(result_);
  EXPECT_EQ(*result_, WaitResult::Cancelled);
  // Woken by the cancel, not by its deadline.
  EXPECT_EQ(woke_at_, kStart);
  EXPECT_EQ(clock_.now(), kStart);
}

TEST_F(CancelTokenVirtual, AbortWakesAClockWaiter) {
  start_waiter(kStart + 1h, true);
  ASSERT_TRUE(await_waiters(clock_, 1));
  token_.abort();
  finish();
  ASSERT_TRUE(result_);
  EXPECT_EQ(*result_, WaitResult::Cancelled);
  EXPECT_EQ(woke_at_, kStart);
}

TEST_F(CancelTokenVirtual, ElapsesAtTheDeadline) {
  start_waiter(kStart + 30s, false);
  finish();
  ASSERT_TRUE(result_);
  EXPECT_EQ(*result_, WaitResult::Elapsed);
  EXPECT_EQ(woke_at_, kStart + 30s);
  EXPECT_EQ(clock_.now(), kStart + 30s);
}

TEST_F(CancelTokenVirtual, WakeEndsTheWaitEarly) {
  start_waiter(kStart + 1h, true);
  ASSERT_TRUE(await_waiters(clock_, 1));
  token_.wake();
  finish();
  ASSERT_TRUE(result_);
  EXPECT_EQ(*result_, WaitResult::Woken);
  EXPECT_EQ(woke_at_, kStart);
  EXPECT_EQ(clock_.now(), kStart);
}

// The clock is forgotten when the last waiter leaves: a cancel made after the
// clock has gone does not reach for it.
TEST(CancelToken, ACancelAfterTheWaitDoesNotUseTheWaitersClock) {
  CancelToken token;
  {
    VirtualClock clock;
    EXPECT_EQ(token.wait_until(clock, clock.now()), WaitResult::Elapsed);
  }
  token.wake();
  token.cancel();
  token.abort();
  EXPECT_EQ(token.mode(), CancelMode::Abort);
}

TEST(CancelToken, WaitsOnASteadyClockAsBefore) {
  SteadyClock clock;
  CancelToken token;
  EXPECT_EQ(token.wait_until(clock, clock.now() + 1ms), WaitResult::Elapsed);

  std::optional<WaitResult> result;
  std::thread waiter([&] { result = token.wait_until(clock, clock.now() + 1h); });
  token.cancel();
  waiter.join();
  EXPECT_EQ(*result, WaitResult::Cancelled);
}

TEST(CancelToken, OnCancelRunsOncePerRequestAndNotAfterRemoval) {
  CancelToken token;
  int calls = 0;
  const auto id = token.add_on_cancel([&] { ++calls; });
  EXPECT_EQ(calls, 0);
  token.cancel();
  EXPECT_EQ(calls, 1);
  // Already cancelled: nothing was requested.
  token.cancel();
  EXPECT_EQ(calls, 1);

  token.remove_on_cancel(id);
  token.reset();
  token.cancel();
  EXPECT_EQ(calls, 1);
}

TEST(CancelToken, OnCancelRunsAtOnceWhenAlreadyRequested) {
  CancelToken token;
  token.cancel();
  int calls = 0;
  const auto id = token.add_on_cancel([&] { ++calls; });
  EXPECT_EQ(calls, 1);
  token.remove_on_cancel(id);
}

TEST(CancelToken, OnCancelRunsAgainWhenACancelBecomesAnAbort) {
  CancelToken token;
  int calls = 0;
  const auto id = token.add_on_cancel([&] { ++calls; });
  token.cancel();
  token.abort();
  EXPECT_EQ(calls, 2);
  // Already aborted.
  token.abort();
  EXPECT_EQ(calls, 2);
  token.remove_on_cancel(id);
}

TEST(CancelToken, ResetAndWakeCallNothingAndResetKeepsTheCallbacks) {
  CancelToken token;
  int calls = 0;
  const auto id = token.add_on_cancel([&] { ++calls; });
  token.wake();
  token.reset();
  EXPECT_EQ(calls, 0);
  token.abort();
  EXPECT_EQ(calls, 1);
  token.reset();
  EXPECT_EQ(calls, 1);
  token.cancel();
  EXPECT_EQ(calls, 2);
  token.remove_on_cancel(id);
}

TEST(CancelToken, EveryCallbackIsCalledAndEachHasItsOwnId) {
  CancelToken token;
  int a = 0, b = 0;
  const auto ia = token.add_on_cancel([&] { ++a; });
  const auto ib = token.add_on_cancel([&] { ++b; });
  EXPECT_NE(ia, ib);
  token.remove_on_cancel(ia);
  token.cancel();
  EXPECT_EQ(a, 0);
  EXPECT_EQ(b, 1);
  token.remove_on_cancel(ib);
  // An id that is not registered (any more) is ignored.
  token.remove_on_cancel(ib);
  token.remove_on_cancel(12345);
}

// A callback is called with the token unlocked: it may use the token.
TEST(CancelToken, ACallbackMayUseTheToken) {
  CancelToken token;
  CancelMode seen = CancelMode::None;
  std::uint64_t inner = 0;
  const auto id = token.add_on_cancel([&] {
    seen = token.mode();
    token.wake();
    inner = token.add_on_cancel([] {});  // called at once: the token is requested
    token.remove_on_cancel(inner);
  });
  token.cancel();
  EXPECT_EQ(seen, CancelMode::Cancel);
  EXPECT_NE(inner, 0u);
  token.remove_on_cancel(id);
}

// From inside the callback itself there is nothing to wait for.
TEST(CancelToken, ACallbackMayRemoveItself) {
  CancelToken token;
  int calls = 0;
  std::uint64_t id = 0;
  id = token.add_on_cancel([&] {
    ++calls;
    token.remove_on_cancel(id);
  });
  token.cancel();
  token.abort();
  EXPECT_EQ(calls, 1);
}

// A callback removed by an earlier one of the same request is not called.
TEST(CancelToken, ACallbackRemovedByAnEarlierOneIsNotCalled) {
  CancelToken token;
  int later = 0;
  std::uint64_t later_id = 0;
  const auto first = token.add_on_cancel([&] { token.remove_on_cancel(later_id); });
  later_id = token.add_on_cancel([&] { ++later; });
  token.cancel();
  EXPECT_EQ(later, 0);
  token.remove_on_cancel(first);
}

// What the callback refers to may go once remove_on_cancel has returned.
TEST(CancelToken, RemoveWaitsForARunningCallback) {
  CancelToken token;
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, release = false;
  std::atomic<bool> finished{false}, removed{false};
  const auto id = token.add_on_cancel([&] {
    std::unique_lock lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
    finished = true;
  });

  std::thread canceller([&] { token.cancel(); });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return entered; }));
  }
  std::thread remover([&] {
    token.remove_on_cancel(id);
    EXPECT_TRUE(finished);
    removed = true;
  });
  // Long enough for a remove that does not wait to have returned.
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(removed);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  cv.notify_all();
  canceller.join();
  remover.join();
  EXPECT_TRUE(removed);
}

// A callback that throws is no longer being called: a remove from another
// thread does not wait for it.
TEST(CancelToken, ACallbackThatThrowsLeavesNothingToWaitFor) {
  CancelToken token;
  const auto id = token.add_on_cancel([] { throw std::runtime_error("no"); });
  EXPECT_THROW(token.cancel(), std::runtime_error);
  EXPECT_EQ(token.mode(), CancelMode::Cancel);

  auto removed = std::make_shared<std::atomic<bool>>(false);
  std::thread remover([&token, id, removed] {
    token.remove_on_cancel(id);
    *removed = true;
  });
  if (!eventually_real([&] { return removed->load(); })) {
    remover.detach();  // stuck in the token: it cannot be joined
    FAIL() << "remove_on_cancel waits for a callback that threw";
    std::abort();      // nor can the token be destroyed under it
  }
  remover.join();
}

// Thrown out of add_on_cancel, for which the caller has no id: the callback
// is not left registered.
TEST(CancelToken, ACallbackThatThrowsAtRegistrationIsNotRegistered) {
  CancelToken token;
  token.cancel();
  int calls = 0;
  EXPECT_THROW(token.add_on_cancel([&] {
    ++calls;
    throw std::runtime_error("no");
  }),
               std::runtime_error);
  EXPECT_EQ(calls, 1);
  token.abort();
  EXPECT_EQ(calls, 1);
}

// Registered while a cancel is being made on another thread, a callback is
// called by one of the two and not by both.
TEST(CancelToken, ACallbackAddedDuringACancelIsCalledOnce) {
  for (int round = 0; round < 200; ++round) {
    CancelToken token;
    std::atomic<int> calls{0};
    std::atomic<bool> go{false};
    std::thread canceller([&] {
      while (!go) std::this_thread::yield();
      token.cancel();
    });
    go = true;
    const auto id = token.add_on_cancel([&] { ++calls; });
    canceller.join();
    token.remove_on_cancel(id);
    ASSERT_EQ(calls, 1) << "round " << round;
  }
}

}  // namespace
