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
