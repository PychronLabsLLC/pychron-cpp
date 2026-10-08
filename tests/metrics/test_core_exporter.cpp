#include "pychron/metrics/core_exporter.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <optional>
#include <string>

#include "metrics_text.hpp"
#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/metrics/registry.hpp"

using namespace pychron;
using namespace pychron::metrics;
using metrics_text::has;
using metrics_text::value;

namespace {

// The registry and the bus come first: the exporter points at both.
struct CoreExporterTest : ::testing::Test {
  Registry registry;
  SignalBus bus;
  double now = 1700000000.0;
  std::unique_ptr<CoreExporter> exporter =
      std::make_unique<CoreExporter>(registry, bus, BuildInfo{"0.3.0", "testos", "testcc 1.0"}, [this] { return now; });

  std::string text() { return registry.render(); }
};

}  // namespace

TEST_F(CoreExporterTest, Pressure) {
  bus.publish(PressureSample{"IG1", 2.5e-9, "torr", {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_pressure{gauge=\"IG1\",unit=\"torr\"}"), 2.5e-9);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_last_sample_timestamp_seconds{kind=\"pressure\",source=\"IG1\"}"), 1700000000.0);
}

TEST_F(CoreExporterTest, TheSampleTimestampIsWhenItWasHandledInRealTime) {
  bus.publish(PressureSample{"IG1", 1e-9, "torr", TimePoint(std::chrono::hours(5))});  // the line's clock: ignored
  now = 1700000015.0;
  bus.publish(PressureSample{"IG1", 1e-9, "torr", TimePoint(std::chrono::hours(9))});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_last_sample_timestamp_seconds{kind=\"pressure\",source=\"IG1\"}"), 1700000015.0);
}

TEST_F(CoreExporterTest, ANonFinitePressureIsRendered) {
  bus.publish(PressureSample{"IG1", std::nan(""), "torr", {}});
  EXPECT_EQ(metrics_text::raw(text(), "pychron_pressure{gauge=\"IG1\",unit=\"torr\"}"), "NaN");
}

TEST_F(CoreExporterTest, Temperature) {
  bus.publish(TemperatureSample{"ls336", "A", 4.2, {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_temperature_kelvin{input=\"A\",source=\"ls336\"}"), 4.2);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_last_sample_timestamp_seconds{kind=\"temperature\",source=\"ls336/A\"}"),
                   1700000000.0);
}

TEST_F(CoreExporterTest, HeaterFieldsComeAndGo) {
  bus.publish(HeaterSample{"h1", 80.0, 100.0, true, false, {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_heater_readback{heater=\"h1\"}"), 80.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_heater_setpoint{heater=\"h1\"}"), 100.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_heater_enabled{heater=\"h1\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_last_sample_timestamp_seconds{kind=\"heater\",source=\"h1\"}"), 1700000000.0);

  bus.publish(HeaterSample{"h1", std::nullopt, 100.0, false, false, {}});
  EXPECT_FALSE(has(text(), "pychron_heater_readback{heater=\"h1\"}"));
  EXPECT_DOUBLE_EQ(value(text(), "pychron_heater_setpoint{heater=\"h1\"}"), 100.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_heater_enabled{heater=\"h1\"}"), 0.0);

  bus.publish(HeaterSample{"h1", 81.0, std::nullopt, std::nullopt, false, {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_heater_readback{heater=\"h1\"}"), 81.0);
  EXPECT_FALSE(has(text(), "pychron_heater_setpoint{heater=\"h1\"}"));
  EXPECT_FALSE(has(text(), "pychron_heater_enabled{heater=\"h1\"}"));
}

TEST_F(CoreExporterTest, ValveStateIsOneOfThree) {
  bus.publish(ValveChanged{"A", ValveState::Open, {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_state{state=\"open\",valve=\"A\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_state{state=\"closed\",valve=\"A\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_state{state=\"unknown\",valve=\"A\"}"), 0.0);
  bus.publish(ValveChanged{"A", ValveState::Closed, {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_state{state=\"open\",valve=\"A\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_state{state=\"closed\",valve=\"A\"}"), 1.0);
  bus.publish(ValveChanged{"A", ValveState::Unknown, {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_state{state=\"closed\",valve=\"A\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_state{state=\"unknown\",valve=\"A\"}"), 1.0);
}

TEST_F(CoreExporterTest, ValveTransitionsCountChangesOnly) {
  bus.publish(ValveChanged{"A", ValveState::Open, {}});
  bus.publish(ValveChanged{"A", ValveState::Open, {}});
  bus.publish(ValveChanged{"A", ValveState::Closed, {}});
  bus.publish(ValveChanged{"B", ValveState::Open, {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_transitions_total{valve=\"A\"}"), 2.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_transitions_total{valve=\"B\"}"), 1.0);
}

TEST_F(CoreExporterTest, SnapshotSeedsValvesWithoutCountingTransitions) {
  Snapshot s;
  s.valves = {{"A", ValveState::Open}, {"B", ValveState::Closed}};
  bus.publish(s);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_state{state=\"open\",valve=\"A\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_state{state=\"closed\",valve=\"B\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_transitions_total{valve=\"A\"}"), 0.0);
  // The state the snapshot gave is what the next change is measured against.
  bus.publish(ValveChanged{"A", ValveState::Open, {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_transitions_total{valve=\"A\"}"), 0.0);
  bus.publish(ValveChanged{"A", ValveState::Closed, {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_valve_transitions_total{valve=\"A\"}"), 1.0);
}

TEST_F(CoreExporterTest, ActuationFailures) {
  bus.publish(ActuationFailed{"A", Error{ErrorKind::Timeout, "no reply from the controller", "vc1"}, {}});
  bus.publish(ActuationFailed{"A", Error{ErrorKind::Interlock, "B is open", ""}, {}});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_actuation_failures_total{valve=\"A\"}"), 2.0);
  EXPECT_EQ(t.find("no reply"), std::string::npos);
}

TEST_F(CoreExporterTest, AlarmsBySourceAndSeverity) {
  bus.publish(Alarm{"IG1", AlarmSeverity::Critical, "pressure above 1e-6", {}});
  bus.publish(Alarm{"IG1", AlarmSeverity::Warning, "read failed", {}});
  bus.publish(Alarm{"IG1", AlarmSeverity::Warning, "read failed again", {}});
  bus.publish(Alarm{"line", AlarmSeverity::Info, "note", {}});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_alarms_total{severity=\"critical\",source=\"IG1\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_alarms_total{severity=\"warning\",source=\"IG1\"}"), 2.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_alarms_total{severity=\"info\",source=\"line\"}"), 1.0);
}

TEST_F(CoreExporterTest, AlarmMessageIsNotALabel) {
  bus.publish(Alarm{"IG1", AlarmSeverity::Critical, "pressure above 1e-6 for sample 12345-01A", {}});
  EXPECT_EQ(text().find("12345-01A"), std::string::npos);
}

TEST_F(CoreExporterTest, LogRecordsUseTheFirstSegmentOfTheLogger) {
  bus.publish(Log{LogLevel::Warn, "transport.serial.ig1", "boom", {}});
  bus.publish(Log{LogLevel::Error, "transport.tcp.vc", "bang", {}});
  bus.publish(Log{LogLevel::Info, "scheduler", "tick", {}});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_log_records_total{component=\"transport\",level=\"warn\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_log_records_total{component=\"transport\",level=\"error\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_log_records_total{component=\"scheduler\",level=\"info\"}"), 1.0);
  EXPECT_EQ(t.find("boom"), std::string::npos);
  EXPECT_EQ(t.find("ig1"), std::string::npos);
}

TEST_F(CoreExporterTest, AnEmptyLoggerNameIsComponentUnknown) {
  bus.publish(Log{LogLevel::Debug, "", "x", {}});
  bus.publish(Log{LogLevel::Trace, ".wire", "x", {}});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_log_records_total{component=\"unknown\",level=\"debug\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_log_records_total{component=\"unknown\",level=\"trace\"}"), 1.0);
}

TEST_F(CoreExporterTest, TransportHealth) {
  bus.publish(TransportHealth{"valve_bus", true, 4, "", {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_transport_connected{transport=\"valve_bus\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_transport_errors_total{transport=\"valve_bus\"}"), 4.0);
  bus.publish(TransportHealth{"valve_bus", false, 6, "connection reset by peer", {}});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_transport_connected{transport=\"valve_bus\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_transport_errors_total{transport=\"valve_bus\"}"), 6.0);
  EXPECT_EQ(t.find("connection reset"), std::string::npos);
}

TEST_F(CoreExporterTest, TransportErrorsSurviveARestartOfTheCount) {
  bus.publish(TransportHealth{"valve_bus", true, 4, "", {}});
  bus.publish(TransportHealth{"valve_bus", true, 1, "", {}});  // reconnected: the transport counts from zero
  EXPECT_DOUBLE_EQ(value(text(), "pychron_transport_errors_total{transport=\"valve_bus\"}"), 5.0);
}

TEST_F(CoreExporterTest, OddNamesRenderValidly) {
  bus.publish(PressureSample{"IG \"bone\"", 1e-8, "torr", {}});
  bus.publish(HeaterSample{"F\xC3\xBCrnace", 20.0, std::nullopt, std::nullopt, std::nullopt, {}});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_pressure{gauge=\"IG \\\"bone\\\"\",unit=\"torr\"}"), 1e-8);
  EXPECT_DOUBLE_EQ(value(t, "pychron_heater_readback{heater=\"F\xC3\xBCrnace\"}"), 20.0);
}

TEST_F(CoreExporterTest, BuildInfoAndStartTime) {
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_build_info{compiler=\"testcc 1.0\",os=\"testos\",version=\"0.3.0\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_process_start_time_seconds"), 1700000000.0);
}

TEST_F(CoreExporterTest, EveryFamilyIsNamedBeforeItsFirstEvent) {
  const std::vector<std::string> names = registry.names();
  for (const char* expected :
       {"pychron_pressure", "pychron_temperature_kelvin", "pychron_heater_readback", "pychron_heater_setpoint",
        "pychron_heater_enabled", "pychron_valve_state", "pychron_valve_transitions_total",
        "pychron_actuation_failures_total", "pychron_last_sample_timestamp_seconds", "pychron_alarms_total",
        "pychron_build_info", "pychron_process_start_time_seconds", "pychron_log_records_total",
        "pychron_transport_connected", "pychron_transport_errors_total"}) {
    EXPECT_NE(std::find(names.begin(), names.end(), expected), names.end()) << expected;
  }
}

TEST_F(CoreExporterTest, StopsListeningWhenDestroyed) {
  bus.publish(PressureSample{"IG1", 1.0, "torr", {}});
  exporter.reset();
  bus.publish(PressureSample{"IG1", 2.0, "torr", {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_pressure{gauge=\"IG1\",unit=\"torr\"}"), 1.0);
}

TEST(BuildInfo, NamesThisPlatform) {
  const BuildInfo b = build_info("1.2.3");
  EXPECT_EQ(b.version, "1.2.3");
  EXPECT_TRUE(b.os == "macos" || b.os == "linux" || b.os == "windows") << b.os;
  EXPECT_FALSE(b.compiler.empty());
  EXPECT_NE(b.compiler.find(' '), std::string::npos) << b.compiler;  // "<name> <version>"
}
