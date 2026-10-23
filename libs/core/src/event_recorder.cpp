#include "pychron/core/event_recorder.hpp"

#include <array>
#include <chrono>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

#include "pychron/core/error.hpp"
#include "pychron/core/log_summary.hpp"

namespace pychron {
namespace {

std::string_view valve_state_name(ValveState s) noexcept {
  switch (s) {
    case ValveState::Open: return "open";
    case ValveState::Closed: return "closed";
    case ValveState::Unknown: break;
  }
  return "unknown";
}

std::string_view severity_name(AlarmSeverity s) noexcept {
  switch (s) {
    case AlarmSeverity::Info: return "info";
    case AlarmSeverity::Warning: return "warning";
    case AlarmSeverity::Critical: return "critical";
  }
  return "warning";
}

std::string format_number(double value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << value;
  return out.str();
}

constexpr std::array<EventClass, 13> kCoreClasses{{
    {"ValveChanged", LogLevel::Debug},
    {"SwitchLockChanged", LogLevel::Debug},
    {"LinkReconnected", LogLevel::Debug},
    {"TransportHealth", LogLevel::Debug},  // only when its connected state changes
    {"ActuationFailed", LogLevel::Warn},
    {"Alarm", LogLevel::Warn},
    {"Sample", std::nullopt},             // every scan
    {"PressureSample", std::nullopt},     // every scan
    {"TemperatureSample", std::nullopt},  // every scan
    {"HeaterSample", std::nullopt},       // every scan
    {"Snapshot", std::nullopt},           // every scan
    {"Log", std::nullopt},                // the hub's own records: would loop
    {"HandlerFailed", std::nullopt},
}};

}  // namespace

void EventLine::key(std::string_view key) {
  if (!out_.empty()) out_ += ", ";
  out_ += key;
  out_ += '=';
}

EventLine& EventLine::text(std::string_view key, std::string_view value) {
  this->key(key);
  out_ += truncate_for_log(value);
  return *this;
}

EventLine& EventLine::word(std::string_view key, std::string_view value) {
  this->key(key);
  out_ += value;
  return *this;
}

EventLine& EventLine::flag(std::string_view key, bool value) { return word(key, value ? "true" : "false"); }

EventLine& EventLine::number(std::string_view key, double value) { return word(key, format_number(value)); }

EventLine& EventLine::number(std::string_view key, std::optional<double> value) {
  if (value) number(key, *value);
  return *this;
}

EventLine& EventLine::seconds(std::string_view key, std::chrono::duration<double> value) {
  return word(key, format_number(value.count()) + "s");
}

EventLine& EventLine::list(std::string_view key, std::span<const std::string> values) {
  std::string joined = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) joined += ',';
    joined += values[i];
  }
  joined += ']';
  return text(key, joined);
}

EventRecorder::EventRecorder(SignalBus& bus) {
  subscriptions_.push_back(
      record_event<ValveChanged>(bus, "ValveChanged", LogLevel::Debug, [](const ValveChanged& e, EventLine& l) {
        l.text("valve", e.valve).word("state", valve_state_name(e.state));
      }));
  subscriptions_.push_back(record_event<SwitchLockChanged>(
      bus, "SwitchLockChanged", LogLevel::Debug,
      [](const SwitchLockChanged& e, EventLine& l) { l.text("name", e.name).flag("locked", e.locked); }));
  subscriptions_.push_back(record_event<LinkReconnected>(
      bus, "LinkReconnected", LogLevel::Debug, [](const LinkReconnected& e, EventLine& l) {
        l.text("transport", e.transport).count("attempts", e.attempts).seconds("down_for", e.down_for);
      }));

  // Logged only when a transport's connected state differs from the last one
  // seen for it (the first one seen included). Handlers run on the publishing
  // threads, several transports' at once: the map has its own mutex, held for
  // the lookup only.
  struct Seen {
    std::mutex mutex;
    std::map<std::string, bool, std::less<>> connected;
  };
  auto seen = std::make_shared<Seen>();
  subscriptions_.push_back(record_event<TransportHealth>(
      bus, "TransportHealth",
      [seen](const TransportHealth& e) -> std::optional<LogLevel> {
        const std::lock_guard lock(seen->mutex);
        auto [it, inserted] = seen->connected.try_emplace(e.transport, e.connected);
        if (!inserted && it->second == e.connected) return std::nullopt;
        it->second = e.connected;
        return LogLevel::Debug;
      },
      [](const TransportHealth& e, EventLine& l) {
        l.text("transport", e.transport)
            .flag("connected", e.connected)
            .count("error_count", e.error_count)
            .text("last_error", e.last_error);
      }));

  subscriptions_.push_back(
      record_event<ActuationFailed>(bus, "ActuationFailed", LogLevel::Warn, [](const ActuationFailed& e, EventLine& l) {
        l.text("valve", e.valve).text("error", to_string(e.error));
      }));
  subscriptions_.push_back(record_event<Alarm>(bus, "Alarm", LogLevel::Warn, [](const Alarm& e, EventLine& l) {
    l.text("source", e.source).word("severity", severity_name(e.severity)).text("message", e.message);
  }));
}

std::span<const EventClass> core_event_classes() noexcept { return kCoreClasses; }

}  // namespace pychron
