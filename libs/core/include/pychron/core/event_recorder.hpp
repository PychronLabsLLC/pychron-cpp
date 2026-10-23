#pragma once

// Bus tap (global logging spec 2.1): events published on a SignalBus written
// to the process log, one record each, on logger "trace.<EventName>". The
// message is the event's fields as `key=value` joined by ", ", in declaration
// order; time points are left out (the record carries its own stamp) and text
// goes through truncate_for_log.
//
// Every event type a library publishes is classified in its
// *_event_classes(): logged at a level, or never logged (high rate, or Log,
// which would loop). Its tests keep the list of event types by hand, so a new
// event fails them until it is classified here.
//
// A handler runs on whatever thread published the event. It never blocks,
// never waits on a Clock and never publishes on the bus itself; a record below
// the logger's level costs one atomic load (and, for TransportHealth, the
// check of whether its state changed). Loggers are taken when a recorder is
// made: an app makes its recorders after it has set the process hub.

#include <chrono>
#include <concepts>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "pychron/core/events.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/core/process_log.hpp"
#include "pychron/core/signal_bus.hpp"

namespace pychron {

// One event type: its name (the struct's, unqualified) and the level it is
// logged at; nullopt: never logged. An event whose level depends on what it
// says (a failed run) has the lowest of its levels here.
struct EventClass {
  std::string_view name;
  std::optional<LogLevel> level;
};

// The message of one record: `key=value, key=value`.
class EventLine {
 public:
  // Text, through truncate_for_log.
  EventLine& text(std::string_view key, std::string_view value);
  // A word that is short by construction (an enum's to_string), as is.
  EventLine& word(std::string_view key, std::string_view value);
  EventLine& flag(std::string_view key, bool value);
  // Six significant digits, in the C locale ("0.25", "1e-09").
  EventLine& number(std::string_view key, double value);
  // Left out when there is none.
  EventLine& number(std::string_view key, std::optional<double> value);
  template <std::integral T>
    requires(!std::same_as<T, bool>)
  EventLine& count(std::string_view key, T value) {
    return word(key, std::to_string(value));
  }
  // Seconds, as number() writes them, with "s": "2.5s". Any std::chrono
  // duration converts to this one.
  EventLine& seconds(std::string_view key, std::chrono::duration<double> value);
  // "[a,b,c]", the whole through truncate_for_log.
  EventLine& list(std::string_view key, std::span<const std::string> values);

  [[nodiscard]] const std::string& str() const noexcept { return out_; }

 private:
  void key(std::string_view key);
  std::string out_;
};

// Subscribes to E on `bus`: each event is one record on logger
// "trace.<name>". `level` is a LogLevel, or a callable (const E&) ->
// std::optional<LogLevel> (nullopt: this one is not logged); `fill` is
// (const E&, EventLine&) -> void, called only when the record is enabled.
template <class E, class Level, class Fill>
[[nodiscard]] SignalBus::Subscription record_event(SignalBus& bus, std::string_view name, Level level, Fill fill) {
  return bus.subscribe<E>([logger = process_logger("trace." + std::string(name)), level_of = std::move(level),
                           fill_in = std::move(fill)](const E& event) {
    std::optional<LogLevel> at;
    if constexpr (std::is_same_v<Level, LogLevel>) {
      at = level_of;
    } else {
      at = level_of(event);
    }
    if (!at || !logger.enabled(*at)) return;
    EventLine line;
    fill_in(event, line);
    logger.log(*at, line.str());
  });
}

// Core events (events.hpp, and the bus's own HandlerFailed).
class EventRecorder {
 public:
  explicit EventRecorder(SignalBus& bus);

 private:
  std::vector<SignalBus::Subscription> subscriptions_;
};

[[nodiscard]] std::span<const EventClass> core_event_classes() noexcept;

}  // namespace pychron
