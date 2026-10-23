#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "log_capture.hpp"
#include "pychron/core/event_recorder.hpp"
#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"

namespace pychron {
namespace {

using testing::LogCapture;
using Lines = std::vector<std::string>;

// Every struct published on a SignalBus that libs/core declares: events.hpp
// and the bus's own HandlerFailed. Kept by hand: a new event fails
// EveryEventClassified until core_event_classes() has it.
const std::set<std::string_view> kCoreEvents{
    "Sample",         "ValveChanged", "PressureSample",    "TemperatureSample", "HeaterSample",
    "Alarm",          "TransportHealth", "LinkReconnected", "Log",               "SwitchLockChanged",
    "Snapshot",       "ActuationFailed", "HandlerFailed",
};

TEST(CoreEventLog, EveryEventClassified) {
  std::set<std::string_view> classified;
  for (const auto& c : core_event_classes()) EXPECT_TRUE(classified.insert(c.name).second) << c.name << " twice";
  EXPECT_EQ(classified, kCoreEvents);
}

TEST(CoreEventLog, ValveChanged) {
  LogCapture log;
  ASSERT_TRUE(log.ok());
  SignalBus bus;
  const EventRecorder recorder(bus);
  bus.publish(ValveChanged{"V1", ValveState::Open, {}});
  bus.publish(ValveChanged{"V2", ValveState::Closed, {}});
  EXPECT_EQ(log.trace_lines(), (Lines{"[debug] trace.ValveChanged: valve=V1, state=open",
                                      "[debug] trace.ValveChanged: valve=V2, state=closed"}));
}

TEST(CoreEventLog, SwitchLockChanged) {
  LogCapture log;
  SignalBus bus;
  const EventRecorder recorder(bus);
  bus.publish(SwitchLockChanged{"A", true, {}});
  EXPECT_EQ(log.trace_lines(), (Lines{"[debug] trace.SwitchLockChanged: name=A, locked=true"}));
}

TEST(CoreEventLog, LinkReconnected) {
  LogCapture log;
  SignalBus bus;
  const EventRecorder recorder(bus);
  bus.publish(LinkReconnected{"ngx", 3, std::chrono::milliseconds(2500), {}});
  EXPECT_EQ(log.trace_lines(), (Lines{"[debug] trace.LinkReconnected: transport=ngx, attempts=3, down_for=2.5s"}));
}

TEST(CoreEventLog, ActuationFailed) {
  LogCapture log;
  SignalBus bus;
  const EventRecorder recorder(bus);
  bus.publish(ActuationFailed{"V1", Error{ErrorKind::Timeout, "no reply", "proxr", ""}, {}});
  EXPECT_EQ(log.trace_lines(), (Lines{"[warn] trace.ActuationFailed: valve=V1, error=proxr: timeout: no reply"}));
}

TEST(CoreEventLog, Alarm) {
  LogCapture log;
  SignalBus bus;
  const EventRecorder recorder(bus);
  bus.publish(Alarm{"gauge", AlarmSeverity::Critical, "pressure high", {}});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[warn] trace.Alarm: source=gauge, severity=critical, message=pressure high"}));
}

TEST(CoreEventLog, TransportHealthOnlyOnChange) {
  LogCapture log;
  SignalBus bus;
  const EventRecorder recorder(bus);
  bus.publish(TransportHealth{"a", false, 1, "timeout", {}});
  bus.publish(TransportHealth{"a", false, 2, "timeout", {}});  // same state: nothing
  bus.publish(TransportHealth{"b", false, 1, "refused", {}});  // another transport
  bus.publish(TransportHealth{"a", true, 0, "", {}});
  bus.publish(TransportHealth{"a", true, 0, "", {}});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.TransportHealth: transport=a, connected=false, error_count=1, last_error=timeout",
                   "[debug] trace.TransportHealth: transport=b, connected=false, error_count=1, last_error=refused",
                   "[debug] trace.TransportHealth: transport=a, connected=true, error_count=0, last_error="}));
}

TEST(CoreEventLog, LongTextIsTruncated) {
  LogCapture log;
  SignalBus bus;
  const EventRecorder recorder(bus);
  bus.publish(Alarm{"s", AlarmSeverity::Warning, std::string(300, 'x'), {}});
  const auto lines = log.trace_lines();
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_EQ(lines[0], "[warn] trace.Alarm: source=s, severity=warning, message=" + std::string(200, 'x') +
                          "\xE2\x80\xA6");
}

TEST(CoreEventLog, NeverLoggedEventsWriteNothing) {
  LogCapture log;
  SignalBus bus;
  const EventRecorder recorder(bus);
  const auto before = log.lines();
  bus.publish(Sample{"g", {}, 1.0});
  bus.publish(PressureSample{"g", 1e-9, "torr", {}});
  bus.publish(TemperatureSample{"tc", "A", 20.0, {}});
  bus.publish(HeaterSample{"h", 1.0, 2.0, true, false, {}});
  bus.publish(Snapshot{});
  bus.publish(Log{LogLevel::Error, "x", "m", {}, {}, {}});
  bus.publish(HandlerFailed{"pychron::ValveChanged", "boom", 1});
  EXPECT_EQ(log.lines(), before);
}

TEST(CoreEventLog, EveryClassifiedLoggedEventIsSubscribed) {
  SignalBus bus;
  const EventRecorder recorder(bus);
  EXPECT_EQ(bus.subscriber_count<ValveChanged>(), 1U);
  EXPECT_EQ(bus.subscriber_count<SwitchLockChanged>(), 1U);
  EXPECT_EQ(bus.subscriber_count<LinkReconnected>(), 1U);
  EXPECT_EQ(bus.subscriber_count<TransportHealth>(), 1U);
  EXPECT_EQ(bus.subscriber_count<ActuationFailed>(), 1U);
  EXPECT_EQ(bus.subscriber_count<Alarm>(), 1U);
  EXPECT_EQ(bus.subscriber_count<Sample>(), 0U);
  EXPECT_EQ(bus.subscriber_count<Log>(), 0U);
}

TEST(CoreEventLog, WithoutHubNothingIsWritten) {
  const ProcessLogScope none(nullptr);
  SignalBus bus;
  const EventRecorder recorder(bus);
  bus.publish(ValveChanged{"V1", ValveState::Open, {}});  // must not crash
}

TEST(EventLine, Formats) {
  EventLine line;
  const std::vector<std::string> list{"a", "b"};
  line.text("t", "x")
      .word("w", "open")
      .flag("f", false)
      .number("n", 0.25)
      .number("none", std::optional<double>{})
      .number("some", std::optional<double>{1e-9})
      .count("c", 3U)
      .seconds("s", std::chrono::seconds(2))
      .list("l", list);
  EXPECT_EQ(line.str(), "t=x, w=open, f=false, n=0.25, some=1e-09, c=3, s=2s, l=[a,b]");
}

}  // namespace
}  // namespace pychron
