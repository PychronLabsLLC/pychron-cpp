#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"

using namespace pychron;

TEST(SignalBus, DeliversByType) {
  SignalBus bus;
  std::vector<std::string> valves;
  int pressures = 0;
  auto s1 = bus.subscribe<ValveChanged>([&](const ValveChanged& e) { valves.push_back(e.valve); });
  auto s2 = bus.subscribe<PressureSample>([&](const PressureSample&) { ++pressures; });

  bus.publish(ValveChanged{"A", ValveState::Open, {}});
  bus.publish(PressureSample{"IG1", 1e-8, "torr", {}});
  bus.publish(ValveChanged{"B", ValveState::Closed, {}});

  EXPECT_EQ(valves, (std::vector<std::string>{"A", "B"}));
  EXPECT_EQ(pressures, 1);
}

TEST(SignalBus, EventsArriveAsCopies) {
  SignalBus bus;
  ValveChanged received;
  auto s = bus.subscribe<ValveChanged>([&](const ValveChanged& e) { received = e; });
  ValveChanged sent{"A", ValveState::Open, {}};
  bus.publish(sent);
  sent.valve = "mutated";
  EXPECT_EQ(received.valve, "A");
}

TEST(SignalBus, SubscriptionUnsubscribesOnDestruction) {
  SignalBus bus;
  int count = 0;
  {
    auto s = bus.subscribe<Alarm>([&](const Alarm&) { ++count; });
    EXPECT_EQ(bus.subscriber_count<Alarm>(), 1u);
    bus.publish(Alarm{});
  }
  EXPECT_EQ(bus.subscriber_count<Alarm>(), 0u);
  bus.publish(Alarm{});
  EXPECT_EQ(count, 1);
}

TEST(SignalBus, ResetAndMove) {
  SignalBus bus;
  int count = 0;
  auto s = bus.subscribe<Log>([&](const Log&) { ++count; });
  SignalBus::Subscription moved = std::move(s);
  EXPECT_FALSE(s.active());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(moved.active());
  bus.publish(Log{});
  moved.reset();
  bus.publish(Log{});
  EXPECT_EQ(count, 1);
}

TEST(SignalBus, SubscriptionMayOutliveBus) {
  SignalBus::Subscription s;
  {
    SignalBus bus;
    s = bus.subscribe<Snapshot>([](const Snapshot&) {});
  }
  EXPECT_FALSE(s.active());
  s.reset();  // no crash
}

TEST(SignalBus, HandlerMayUnsubscribeItselfDuringPublish) {
  SignalBus bus;
  int count = 0;
  SignalBus::Subscription self;
  self = bus.subscribe<TransportHealth>([&](const TransportHealth&) {
    ++count;
    self.reset();
  });
  bus.publish(TransportHealth{});
  bus.publish(TransportHealth{});
  EXPECT_EQ(count, 1);
}

TEST(SignalBus, ThrowingHandlerDoesNotStopOthers) {
  SignalBus bus;
  int count = 0;
  auto a = bus.subscribe<ActuationFailed>([](const ActuationFailed&) { throw std::runtime_error("bad"); });
  auto b = bus.subscribe<ActuationFailed>([&](const ActuationFailed&) { ++count; });
  bus.publish(ActuationFailed{"A", Error{ErrorKind::Interlock, "B open", "A"}, {}});
  EXPECT_EQ(count, 1);
}

TEST(SignalBus, AThrowingHandlerIsReported) {
  SignalBus bus;
  std::vector<HandlerFailed> seen;
  auto watch = bus.subscribe<HandlerFailed>([&](const HandlerFailed& e) { seen.push_back(e); });
  auto a = bus.subscribe<ValveChanged>([](const ValveChanged&) { throw std::runtime_error("bad"); });
  // NOLINTNEXTLINE(bugprone-std-exception-baseclass): what is not an exception is caught too
  auto b = bus.subscribe<ValveChanged>([](const ValveChanged&) { throw 7; });
  EXPECT_EQ(bus.handler_failures(), 0u);
  bus.publish(ValveChanged{});
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_NE(seen[0].event.find("ValveChanged"), std::string::npos) << seen[0].event;
  EXPECT_EQ(seen[0].event.find("struct "), std::string::npos) << seen[0].event;
  EXPECT_EQ(seen[0].what, "bad");
  EXPECT_EQ(seen[0].count, 1u);
  EXPECT_EQ(seen[1].what, "unknown exception");
  EXPECT_EQ(seen[1].count, 2u);
  EXPECT_EQ(bus.handler_failures(), 2u);
}

// A failure is counted for its own event: one noisy handler does not hide
// the first failure of another.
TEST(SignalBus, FailuresAreCountedByEvent) {
  SignalBus bus;
  std::vector<HandlerFailed> seen;
  auto watch = bus.subscribe<HandlerFailed>([&](const HandlerFailed& e) { seen.push_back(e); });
  auto a = bus.subscribe<ValveChanged>([](const ValveChanged&) { throw std::runtime_error("v"); });
  auto b = bus.subscribe<Alarm>([](const Alarm&) { throw std::runtime_error("a"); });
  bus.publish(ValveChanged{});
  bus.publish(ValveChanged{});
  bus.publish(Alarm{});
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[1].count, 2u);
  EXPECT_EQ(seen[2].count, 1u);
}

// The report of a failure is not itself reported: a watcher that throws is
// counted, and that is the end of it.
TEST(SignalBus, AThrowingWatcherOfFailuresDoesNotRecurse) {
  SignalBus bus;
  int reports = 0;
  auto watch = bus.subscribe<HandlerFailed>([&](const HandlerFailed&) {
    ++reports;
    throw std::runtime_error("watcher");
  });
  auto a = bus.subscribe<ValveChanged>([](const ValveChanged&) { throw std::runtime_error("bad"); });
  bus.publish(ValveChanged{});
  EXPECT_EQ(reports, 1);
  EXPECT_EQ(bus.handler_failures(), 2u);
}

TEST(SignalBus, ConcurrentPublishAndSubscribe) {
  SignalBus bus;
  std::atomic<int> received{0};
  auto keep = bus.subscribe<Sample>([&](const Sample&) { received.fetch_add(1); });

  constexpr int kPublishers = 4;
  constexpr int kPerThread = 2000;
  std::vector<std::thread> threads;
  for (int t = 0; t < kPublishers; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < kPerThread; ++i) bus.publish(Sample{"d", {}, 1.0});
    });
  }
  threads.emplace_back([&] {
    for (int i = 0; i < 500; ++i) {
      auto s = bus.subscribe<Sample>([](const Sample&) {});
    }
  });
  for (auto& t : threads) t.join();
  EXPECT_EQ(received.load(), kPublishers * kPerThread);
  EXPECT_EQ(bus.subscriber_count<Sample>(), 1u);
}
