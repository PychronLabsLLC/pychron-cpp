#include "pychron/metrics/core_exporter.hpp"

#include <optional>
#include <utility>

namespace pychron::metrics {

namespace {

constexpr const char* kPressure = "pychron_pressure";
constexpr const char* kPressureHelp = "The last reading of a pressure gauge, in the unit the gauge is configured with.";
constexpr const char* kTemperature = "pychron_temperature_kelvin";
constexpr const char* kTemperatureHelp = "The last reading of a temperature controller's input.";
constexpr const char* kHeaterReadback = "pychron_heater_readback";
constexpr const char* kHeaterReadbackHelp = "A heater's last readback, in its configured unit.";
constexpr const char* kHeaterSetpoint = "pychron_heater_setpoint";
constexpr const char* kHeaterSetpointHelp = "A heater's setpoint, in its configured unit.";
constexpr const char* kHeaterEnabled = "pychron_heater_enabled";
constexpr const char* kHeaterEnabledHelp = "1 while a heater's output is enabled.";
constexpr const char* kValveState = "pychron_valve_state";
constexpr const char* kValveStateHelp = "1 for the state a valve is in, 0 for the others.";
constexpr const char* kValveTransitions = "pychron_valve_transitions_total";
constexpr const char* kValveTransitionsHelp = "Times a valve changed state.";
constexpr const char* kActuationFailures = "pychron_actuation_failures_total";
constexpr const char* kActuationFailuresHelp = "Valve actuations that failed.";
constexpr const char* kLastSample = "pychron_last_sample_timestamp_seconds";
constexpr const char* kLastSampleHelp = "When a source was last read, in real time. Its age says whether the reading is stale.";
constexpr const char* kAlarms = "pychron_alarms_total";
constexpr const char* kAlarmsHelp = "Alarms raised.";
constexpr const char* kLogRecords = "pychron_log_records_total";
constexpr const char* kLogRecordsHelp = "Log records, by level and by the first part of the logger's name.";
constexpr const char* kTransportConnected = "pychron_transport_connected";
constexpr const char* kTransportConnectedHelp = "1 while a transport is connected.";
constexpr const char* kTransportErrors = "pychron_transport_errors_total";
constexpr const char* kTransportErrorsHelp = "Errors a transport has reported.";

const char* state_name(ValveState s) {
  switch (s) {
    case ValveState::Open: return "open";
    case ValveState::Closed: return "closed";
    case ValveState::Unknown: return "unknown";
  }
  return "unknown";
}

const char* severity_name(AlarmSeverity s) {
  switch (s) {
    case AlarmSeverity::Info: return "info";
    case AlarmSeverity::Warning: return "warning";
    case AlarmSeverity::Critical: return "critical";
  }
  return "warning";
}

const char* level_name(LogLevel l) {
  switch (l) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
  }
  return "info";
}

// "transport.serial.ig1" -> "transport": the rest names one device or one
// run, and the set of components must stay small.
std::string component_of(const std::string& logger) {
  const std::string head = logger.substr(0, logger.find('.'));
  return head.empty() ? "unknown" : head;
}

// A field a device may not report: shown while it has a value, hidden when not.
void set_or_remove(Registry& r, const char* name, const char* help, const Labels& labels, std::optional<double> v) {
  if (v) {
    r.gauge(name, help, labels).set(*v);
  } else {
    r.remove(name, labels);
  }
}

}  // namespace

BuildInfo build_info(std::string version) {
  BuildInfo b;
  b.version = std::move(version);
#if defined(_WIN32)
  b.os = "windows";
#elif defined(__APPLE__)
  b.os = "macos";
#else
  b.os = "linux";
#endif
#if defined(__clang__)
  b.compiler = "clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
  b.compiler = "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#elif defined(_MSC_VER)
  b.compiler = "msvc " + std::to_string(_MSC_VER);
#else
  b.compiler = "unknown 0";
#endif
  return b;
}

CoreExporter::CoreExporter(Registry& registry, SignalBus& bus, BuildInfo build, UnixClock now)
    : registry_(registry), now_(std::move(now)) {
  // Families whose series wait for a first event are named now, so the list
  // of what this build exports does not depend on what has happened yet.
  registry.declare(MetricType::Gauge, kPressure, kPressureHelp);
  registry.declare(MetricType::Gauge, kTemperature, kTemperatureHelp);
  registry.declare(MetricType::Gauge, kHeaterReadback, kHeaterReadbackHelp);
  registry.declare(MetricType::Gauge, kHeaterSetpoint, kHeaterSetpointHelp);
  registry.declare(MetricType::Gauge, kHeaterEnabled, kHeaterEnabledHelp);
  registry.declare(MetricType::Gauge, kValveState, kValveStateHelp);
  registry.declare(MetricType::Counter, kValveTransitions, kValveTransitionsHelp);
  registry.declare(MetricType::Counter, kActuationFailures, kActuationFailuresHelp);
  registry.declare(MetricType::Gauge, kLastSample, kLastSampleHelp);
  registry.declare(MetricType::Counter, kAlarms, kAlarmsHelp);
  registry.declare(MetricType::Counter, kLogRecords, kLogRecordsHelp);
  registry.declare(MetricType::Gauge, kTransportConnected, kTransportConnectedHelp);
  registry.declare(MetricType::Counter, kTransportErrors, kTransportErrorsHelp);

  registry
      .gauge("pychron_build_info", "Always 1; the labels say which build is running.",
             {{"version", build.version}, {"os", build.os}, {"compiler", build.compiler}})
      .set(1);
  registry.gauge("pychron_process_start_time_seconds", "When the application started, in real time.").set(now_());

  subscriptions_.push_back(bus.subscribe<PressureSample>([this](const PressureSample& e) {
    registry_.gauge(kPressure, kPressureHelp, {{"gauge", e.gauge}, {"unit", e.units}}).set(e.value);
    stamp("pressure", e.gauge);
  }));
  subscriptions_.push_back(bus.subscribe<TemperatureSample>([this](const TemperatureSample& e) {
    registry_.gauge(kTemperature, kTemperatureHelp, {{"source", e.source}, {"input", e.input}}).set(e.kelvin);
    stamp("temperature", e.source + "/" + e.input);
  }));
  subscriptions_.push_back(bus.subscribe<HeaterSample>([this](const HeaterSample& e) {
    const Labels labels{{"heater", e.heater}};
    set_or_remove(registry_, kHeaterReadback, kHeaterReadbackHelp, labels, e.readback);
    set_or_remove(registry_, kHeaterSetpoint, kHeaterSetpointHelp, labels, e.setpoint);
    set_or_remove(registry_, kHeaterEnabled, kHeaterEnabledHelp, labels,
                  e.enabled ? std::optional<double>(*e.enabled ? 1.0 : 0.0) : std::nullopt);
    stamp("heater", e.heater);
  }));
  subscriptions_.push_back(
      bus.subscribe<ValveChanged>([this](const ValveChanged& e) { on_valve(e.valve, e.state, true); }));
  subscriptions_.push_back(bus.subscribe<Snapshot>([this](const Snapshot& e) {
    // What the line says its valves are: a starting point, not a change.
    for (const auto& [valve, state] : e.valves) on_valve(valve, state, false);
  }));
  subscriptions_.push_back(bus.subscribe<ActuationFailed>([this](const ActuationFailed& e) {
    registry_.counter(kActuationFailures, kActuationFailuresHelp, {{"valve", e.valve}}).inc();
  }));
  subscriptions_.push_back(bus.subscribe<Alarm>([this](const Alarm& e) {
    registry_.counter(kAlarms, kAlarmsHelp, {{"source", e.source}, {"severity", severity_name(e.severity)}}).inc();
  }));
  subscriptions_.push_back(bus.subscribe<Log>([this](const Log& e) {
    registry_
        .counter(kLogRecords, kLogRecordsHelp, {{"level", level_name(e.level)}, {"component", component_of(e.logger)}})
        .inc();
  }));
  subscriptions_.push_back(bus.subscribe<TransportHealth>([this](const TransportHealth& e) {
    const Labels labels{{"transport", e.transport}};
    registry_.gauge(kTransportConnected, kTransportConnectedHelp, labels).set(e.connected ? 1.0 : 0.0);
    registry_.counter(kTransportErrors, kTransportErrorsHelp, labels).set_total(static_cast<double>(e.error_count));
  }));
}

CoreExporter::~CoreExporter() {
  for (SignalBus::Subscription& s : subscriptions_) s.reset();
}

void CoreExporter::on_valve(const std::string& valve, ValveState state, bool count_transition) {
  bool changed = false;
  {
    const std::lock_guard lock(valves_mutex_);
    const auto [it, inserted] = valves_.try_emplace(valve, state);
    changed = inserted || it->second != state;
    it->second = state;
  }
  for (const ValveState s : {ValveState::Open, ValveState::Closed, ValveState::Unknown}) {
    registry_.gauge(kValveState, kValveStateHelp, {{"valve", valve}, {"state", state_name(s)}}).set(s == state ? 1.0 : 0.0);
  }
  Counter& transitions = registry_.counter(kValveTransitions, kValveTransitionsHelp, {{"valve", valve}});
  if (changed && count_transition) transitions.inc();
}

void CoreExporter::stamp(const char* kind, const std::string& source) {
  registry_.gauge(kLastSample, kLastSampleHelp, {{"kind", kind}, {"source", source}}).set(now_());
}

}  // namespace pychron::metrics
