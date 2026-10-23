#include "pychron/experiment/event_log.hpp"

#include <array>
#include <optional>

#include "pychron/experiment/collect/collector.hpp"
#include "pychron/experiment/conditionals/conditional.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/lab/notifier.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/measurement/engine.hpp"
#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/record/types.hpp"
#include "pychron/experiment/run/run.hpp"
#include "pychron/experiment/run/state.hpp"

namespace pychron::experiment {
namespace {

constexpr LogLevel kDebug = LogLevel::Debug;
constexpr LogLevel kInfo = LogLevel::Info;

constexpr std::array<EventClass, 22> kExperimentClasses{{
    {"ExecutorStateChanged", LogLevel::Debug},
    {"RunStarted", LogLevel::Debug},
    {"RunFinished", LogLevel::Debug},  // info when the run did not succeed, or was not saved
    {"QueueEdited", LogLevel::Debug},
    {"QueueFrontier", LogLevel::Debug},
    {"ExecutorWaiting", LogLevel::Debug},
    {"ExecutorHolding", LogLevel::Debug},
    {"RunStateChanged", LogLevel::Debug},
    {"BlockStarted", LogLevel::Debug},
    {"BlockFinished", LogLevel::Debug},
    {"NotificationSent", LogLevel::Debug},
    {"SpoolStatus", LogLevel::Debug},  // warn when a record could not be sent
    {"QueueStarted", LogLevel::Info},
    {"QueueEnded", LogLevel::Info},
    {"ConditionalTripped", LogLevel::Info},
    {"RunNote", LogLevel::Info},
    {"MeasurementGap", LogLevel::Info},     // warn when the readings did not resume
    {"CountsProgress", std::nullopt},      // every reading
    {"OverlapReady", std::nullopt},
    {"NotificationOutbox", std::nullopt},  // the outbox's size
    {"SeriesUpdated", std::nullopt},       // every reading
    {"FitsUpdated", std::nullopt},         // every reading
}};

}  // namespace

ExperimentEventRecorder::ExperimentEventRecorder(SignalBus& bus) {
  using executor::ExecutorHolding;
  using executor::ExecutorStateChanged;
  using executor::ExecutorWaiting;
  using executor::QueueEdited;
  using executor::QueueFrontier;
  using executor::RunFinished;
  using executor::RunStarted;
  using lab::NotificationSent;
  using lab::QueueEnded;
  using lab::QueueStarted;
  using measurement::BlockFinished;
  using measurement::BlockStarted;
  using measurement::ConditionalTripped;
  using measurement::MeasurementGap;
  using persist::SpoolStatus;
  using run::RunNote;
  using run::RunStateChanged;

  auto& subs = subscriptions_;
  subs.push_back(record_event<ExecutorStateChanged>(
      bus, "ExecutorStateChanged", kDebug, [](const ExecutorStateChanged& e, EventLine& l) {
        l.word("from", executor::to_string(e.from)).word("to", executor::to_string(e.to)).text("reason", e.reason);
      }));
  subs.push_back(record_event<RunStarted>(bus, "RunStarted", kDebug, [](const RunStarted& e, EventLine& l) {
    l.count("row", e.row).text("run", e.name).text("run_id", e.run_id).text("identifier", e.identifier);
  }));
  // The summary, flattened to how the run ended: its queue changes and
  // messages are left out (QueueEdited and RunNote say them).
  subs.push_back(record_event<RunFinished>(
      bus, "RunFinished",
      [](const RunFinished& e) -> std::optional<LogLevel> {
        const auto& s = e.summary;
        const bool failed =
            s.state != run::RunState::Success || s.save_error || s.status == record::RecordStatus::Failed;
        return failed ? kInfo : kDebug;
      },
      [](const RunFinished& e, EventLine& l) {
        const auto& s = e.summary;
        l.count("row", s.row)
            .text("identifier", s.identifier)
            .text("run_id", s.run_id)
            .count("aliquot", s.aliquot)
            .text("step", s.step)
            .word("state", run::to_string(s.state))
            .flag("truncated", s.truncated)
            .flag("save_error", s.save_error)
            .word("status", record::to_string(s.status));
        if (!s.failure.empty()) l.text("failure", s.failure);
        if (s.error) l.text("error", *s.error);
      }));
  // The queue itself is its row count.
  subs.push_back(record_event<QueueEdited>(bus, "QueueEdited", kDebug, [](const QueueEdited& e, EventLine& l) {
    l.count("rows", e.queue.runs.size()).list("changes", e.changes).count("version", e.version).count("frozen", e.frozen);
  }));
  subs.push_back(record_event<QueueFrontier>(bus, "QueueFrontier", kDebug, [](const QueueFrontier& e, EventLine& l) {
    l.count("frozen", e.frozen).count("version", e.version);
  }));
  subs.push_back(
      record_event<ExecutorWaiting>(bus, "ExecutorWaiting", kDebug, [](const ExecutorWaiting& e, EventLine& l) {
        l.text("reason", e.reason).seconds("duration", e.duration).text("run_id", e.run_id);
      }));
  subs.push_back(
      record_event<ExecutorHolding>(bus, "ExecutorHolding", kDebug, [](const ExecutorHolding& e, EventLine& l) {
        l.text("reason", e.reason).list("links", e.links);
        if (e.row) l.count("row", *e.row);
        l.word("kind", executor::to_string(e.kind));
      }));
  subs.push_back(
      record_event<RunStateChanged>(bus, "RunStateChanged", kDebug, [](const RunStateChanged& e, EventLine& l) {
        l.text("run_id", e.run_id)
            .word("from", run::to_string(e.from))
            .word("to", run::to_string(e.to))
            .text("reason", e.reason);
      }));
  subs.push_back(record_event<RunNote>(bus, "RunNote", kInfo, [](const RunNote& e, EventLine& l) {
    l.text("run_id", e.run_id).count("row", e.row).text("text", e.text);
  }));
  subs.push_back(record_event<QueueStarted>(bus, "QueueStarted", kInfo, [](const QueueStarted& e, EventLine& l) {
    l.count("rows", e.rows).count("from_row", e.from_row);
  }));
  // The result, flattened: its runs are their count (RunFinished said each).
  subs.push_back(record_event<QueueEnded>(bus, "QueueEnded", kInfo, [](const QueueEnded& e, EventLine& l) {
    const auto& r = e.result;
    l.word("end", executor::to_string(r.end)).text("reason", r.reason).count("runs", r.runs.size());
    if (!r.code.empty()) l.text("code", r.code);
  }));
  subs.push_back(record_event<BlockStarted>(bus, "BlockStarted", kDebug, [](const BlockStarted& e, EventLine& l) {
    l.text("run_id", e.run_id).word("block", measurement::to_string(e.block)).text("gas", e.gas).count("segment", e.segment);
  }));
  subs.push_back(record_event<BlockFinished>(bus, "BlockFinished", kDebug, [](const BlockFinished& e, EventLine& l) {
    l.text("run_id", e.run_id)
        .word("block", measurement::to_string(e.block))
        .flag("ok", e.ok)
        .text("gas", e.gas)
        .count("segment", e.segment);
  }));
  subs.push_back(record_event<MeasurementGap>(
      bus, "MeasurementGap",
      [](const MeasurementGap& e) -> std::optional<LogLevel> { return e.resumed ? kInfo : LogLevel::Warn; },
      [](const MeasurementGap& e, EventLine& l) {
        l.text("run_id", e.run_id)
            .word("block", measurement::to_string(e.block))
            .number("from", e.from)
            .number("to", e.to)
            .flag("resumed", e.resumed);
      }));
  // The trip, flattened to what fired and why; its action and metric
  // context are left out.
  subs.push_back(record_event<ConditionalTripped>(
      bus, "ConditionalTripped", kInfo, [](const ConditionalTripped& e, EventLine& l) {
        const auto& t = e.trip;
        l.text("run_id", e.run_id)
            .text("name", t.name)
            .word("kind", to_string(t.kind))
            .number("value", t.value)
            .count("count", t.count)
            .count("reading", t.reading)
            .text("check", t.check);
      }));
  subs.push_back(
      record_event<NotificationSent>(bus, "NotificationSent", kDebug, [](const NotificationSent& e, EventLine& l) {
        l.text("channel", e.channel)
            .word("event", lab::to_string(e.event))
            .text("subject", e.subject)
            .flag("ok", e.ok)
            .text("error", e.error);
      }));
  subs.push_back(record_event<SpoolStatus>(
      bus, "SpoolStatus",
      [](const SpoolStatus& e) -> std::optional<LogLevel> { return e.last_error ? LogLevel::Warn : kDebug; },
      [](const SpoolStatus& e, EventLine& l) {
        l.count("pending", e.pending);
        if (e.last_error) l.text("last_error", *e.last_error);
      }));
}

std::span<const EventClass> experiment_event_classes() noexcept { return kExperimentClasses; }

}  // namespace pychron::experiment
