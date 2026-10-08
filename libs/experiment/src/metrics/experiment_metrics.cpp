#include "pychron/experiment/metrics/experiment_metrics.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string_view>
#include <utility>

#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/lab/notifier.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/measurement/engine.hpp"

namespace pychron::experiment::metrics {

namespace {

using executor::ExecutorState;
using executor::QueueEnd;
using pychron::metrics::Labels;
using pychron::metrics::MetricType;
using run::RunState;

constexpr const char* kExecutorState = "pychron_executor_state";
constexpr const char* kExecutorStateHelp = "1 for the state the executor is in, 0 for the others.";
constexpr const char* kQueueActive = "pychron_queue_active";
constexpr const char* kQueueActiveHelp = "1 while a queue is running.";
constexpr const char* kQueueRuns = "pychron_queue_runs";
constexpr const char* kQueueRunsHelp = "The running queue: the runs it has to do (total) and those finished (done).";
constexpr const char* kQueuesEnded = "pychron_queues_ended_total";
constexpr const char* kQueuesEndedHelp = "Queues that ended, by how.";
constexpr const char* kRunsStarted = "pychron_runs_started_total";
constexpr const char* kRunsStartedHelp = "Runs started.";
constexpr const char* kRunsFinished = "pychron_runs_finished_total";
constexpr const char* kRunsFinishedHelp = "Runs that ended, by their last state and whether they were truncated.";
constexpr const char* kSaveErrors = "pychron_run_save_errors_total";
constexpr const char* kSaveErrorsHelp = "Runs whose record could not be saved.";
constexpr const char* kStateDuration = "pychron_run_state_duration_seconds";
constexpr const char* kStateDurationHelp =
    "How long runs spend in each state, on the line's clock. The saving state is the cost of writing the record.";
constexpr const char* kBlocks = "pychron_measurement_blocks_total";
constexpr const char* kBlocksHelp = "Measurement blocks finished, by block and whether it succeeded.";
constexpr const char* kTrips = "pychron_conditional_trips_total";
constexpr const char* kTripsHelp = "Conditionals that tripped, by kind and by where they were defined.";
constexpr const char* kWaits = "pychron_executor_waits_total";
constexpr const char* kWaitsHelp = "Times the executor waited, by what for.";
constexpr const char* kLastFinished = "pychron_last_run_finished_age_seconds";
constexpr const char* kLastFinishedHelp = "Real seconds since a run last finished. Absent until one has.";
constexpr const char* kNotifications = "pychron_notifications_total";
constexpr const char* kNotificationsHelp = "Notifications handed to a channel, by event and whether it took them.";

// From one second to two hours: an extraction is tens of seconds, a
// measurement tens of minutes, a save a second or two.
std::vector<double> duration_buckets() { return {1, 2.5, 5, 10, 30, 60, 150, 300, 600, 1200, 2400, 3600, 7200}; }

// An enumeration's name as a label value: "baseline.after" -> "baseline_after".
std::string label_of(std::string_view name) {
  std::string out(name);
  for (char& c : out) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) c = '_';
  }
  return out;
}

// The executor's reason is a sentence, often with the sample in it
// ("delay before 12345-01A"); the label is which kind of wait it was.
const char* wait_reason(std::string_view reason) {
  if (reason.rfind("scheduled start", 0) == 0) return "scheduled_start";
  if (reason.rfind("delay", 0) == 0) return "delay";
  if (reason == "extraction device") return "extraction_device";
  if (reason.find("pump") != std::string_view::npos) return "pump_time";
  return "other";
}

const char* bool_name(bool b) { return b ? "true" : "false"; }

constexpr measurement::Block kBlocks_[] = {
    measurement::Block::PeakCenterBefore, measurement::Block::BaselineBefore, measurement::Block::PositionFirstHop,
    measurement::Block::Equilibrate,      measurement::Block::Main,           measurement::Block::BaselineAfter,
    measurement::Block::PeakCenterAfter};
constexpr ConditionalKind kKinds[] = {
    ConditionalKind::Truncation,   ConditionalKind::Termination,   ConditionalKind::Cancelation, ConditionalKind::Action,
    ConditionalKind::Modification, ConditionalKind::Equilibration, ConditionalKind::PreRun,      ConditionalKind::PostRun};
constexpr ConditionalLevel kLevels[] = {ConditionalLevel::System, ConditionalLevel::Queue, ConditionalLevel::Plan,
                                        ConditionalLevel::Run, ConditionalLevel::Hook};
constexpr lab::NotifyEvent kNotifyEvents[] = {lab::NotifyEvent::RunFailed, lab::NotifyEvent::QueueEnded,
                                              lab::NotifyEvent::Test};

constexpr ExecutorState kExecutorStates[] = {
    ExecutorState::Idle,       ExecutorState::Preparing, ExecutorState::Running,   ExecutorState::StoppingAtBoundary,
    ExecutorState::Cancelling, ExecutorState::Aborting,  ExecutorState::Finalizing};

}  // namespace

ExperimentMetrics::ExperimentMetrics(pychron::metrics::Registry& registry, SignalBus& bus,
                                     pychron::metrics::RealClock now)
    : registry_(registry), now_(std::move(now)) {
  registry.declare(MetricType::Histogram, kStateDuration, kStateDurationHelp, duration_buckets());
  registry.declare(MetricType::Gauge, kLastFinished, kLastFinishedHelp);
  registry.declare(MetricType::Counter, kNotifications, kNotificationsHelp);

  // What can be named in advance reads zero from the start. A counter that
  // first appears already at 1 shows no increase: the box has nothing
  // earlier to compare it with, and the first trip or the first failed
  // block, often the only one, would not be drawn.
  for (const measurement::Block b : kBlocks_) {
    for (const bool ok : {false, true}) {
      registry.counter(kBlocks, kBlocksHelp, {{"block", label_of(to_string(b))}, {"ok", bool_name(ok)}});
    }
  }
  for (const ConditionalKind k : kKinds) {
    for (const ConditionalLevel l : kLevels) {
      registry.counter(kTrips, kTripsHelp, {{"kind", label_of(to_string(k))}, {"level", label_of(to_string(l))}});
    }
  }
  for (const char* reason : {"scheduled_start", "delay", "extraction_device", "pump_time", "other"}) {
    registry.counter(kWaits, kWaitsHelp, {{"reason", reason}});
  }
  for (const ExecutorState s : kExecutorStates) {
    registry.gauge(kExecutorState, kExecutorStateHelp, {{"state", label_of(to_string(s))}})
        .set(s == ExecutorState::Idle ? 1.0 : 0.0);
  }
  registry.gauge(kQueueActive, kQueueActiveHelp).set(0);
  registry.gauge(kQueueRuns, kQueueRunsHelp, {{"status", "total"}}).set(0);
  registry.gauge(kQueueRuns, kQueueRunsHelp, {{"status", "done"}}).set(0);
  for (const QueueEnd e :
       {QueueEnd::Completed, QueueEnd::Stopped, QueueEnd::Cancelled, QueueEnd::Aborted, QueueEnd::Failed}) {
    registry.counter(kQueuesEnded, kQueuesEndedHelp, {{"end", label_of(to_string(e))}});
  }
  registry.counter(kRunsStarted, kRunsStartedHelp);
  registry.counter(kSaveErrors, kSaveErrorsHelp);
  for (const RunState s : {RunState::Success, RunState::Failed, RunState::Cancelled, RunState::Aborted}) {
    for (const bool truncated : {false, true}) {
      registry.counter(kRunsFinished, kRunsFinishedHelp,
                       {{"state", label_of(to_string(s))}, {"truncated", bool_name(truncated)}});
    }
  }

  subscriptions_.push_back(bus.subscribe<executor::ExecutorStateChanged>([this](const executor::ExecutorStateChanged& e) {
    for (const ExecutorState s : kExecutorStates) {
      registry_.gauge(kExecutorState, kExecutorStateHelp, {{"state", label_of(to_string(s))}}).set(s == e.to ? 1.0 : 0.0);
    }
    // An executor driven without a session says nothing else about its queue.
    set_active(e.to != ExecutorState::Idle);
  }));
  subscriptions_.push_back(bus.subscribe<lab::QueueStarted>([this](const lab::QueueStarted& e) {
    {
      const std::lock_guard lock(mutex_);
      from_row_ = e.from_row;
      done_ = 0;
      runs_.clear();
    }
    set_queue(e.rows);
    registry_.gauge(kQueueRuns, kQueueRunsHelp, {{"status", "done"}}).set(0);
    set_active(true);
  }));
  subscriptions_.push_back(
      bus.subscribe<executor::QueueEdited>([this](const executor::QueueEdited& e) { set_queue(e.queue.runs.size()); }));
  subscriptions_.push_back(bus.subscribe<lab::QueueEnded>([this](const lab::QueueEnded& e) {
    {
      // A run cut off by an abort never says it ended.
      const std::lock_guard lock(mutex_);
      runs_.clear();
    }
    registry_.counter(kQueuesEnded, kQueuesEndedHelp, {{"end", label_of(to_string(e.result.end))}}).inc();
    set_active(false);
  }));
  subscriptions_.push_back(bus.subscribe<executor::RunStarted>(
      [this](const executor::RunStarted&) { registry_.counter(kRunsStarted, kRunsStartedHelp).inc(); }));
  subscriptions_.push_back(bus.subscribe<executor::RunFinished>([this](const executor::RunFinished& e) {
    const executor::RunSummary& s = e.summary;
    registry_
        .counter(kRunsFinished, kRunsFinishedHelp,
                 {{"state", label_of(to_string(s.state))}, {"truncated", bool_name(s.truncated)}})
        .inc();
    if (s.save_error) registry_.counter(kSaveErrors, kSaveErrorsHelp).inc();
    const double now = now_();
    std::size_t done = 0;
    {
      const std::lock_guard lock(mutex_);
      last_finished_ = now;
      done = ++done_;
    }
    registry_.gauge(kQueueRuns, kQueueRunsHelp, {{"status", "done"}}).set(static_cast<double>(done));
  }));
  subscriptions_.push_back(bus.subscribe<run::RunStateChanged>([this](const run::RunStateChanged& e) {
    std::optional<Tracked> left;
    {
      const std::lock_guard lock(mutex_);
      if (const auto it = runs_.find(e.run_id); it != runs_.end()) left = it->second;
      if (run::is_terminal(e.to)) {
        runs_.erase(e.run_id);
      } else {
        runs_[e.run_id] = Tracked{e.to, e.ts};
      }
    }
    // Pending is the wait in the queue, not a phase of the run.
    if (!left || left->state == RunState::Pending) return;
    const double seconds = std::max(0.0, std::chrono::duration<double>(e.ts - left->since).count());
    registry_
        .histogram(kStateDuration, kStateDurationHelp, duration_buckets(), {{"state", label_of(to_string(left->state))}})
        .observe(seconds);
  }));
  subscriptions_.push_back(bus.subscribe<measurement::BlockFinished>([this](const measurement::BlockFinished& e) {
    registry_.counter(kBlocks, kBlocksHelp, {{"block", label_of(to_string(e.block))}, {"ok", bool_name(e.ok)}}).inc();
  }));
  subscriptions_.push_back(bus.subscribe<measurement::ConditionalTripped>([this](const measurement::ConditionalTripped& e) {
    registry_
        .counter(kTrips, kTripsHelp,
                 {{"kind", label_of(to_string(e.trip.kind))}, {"level", label_of(to_string(e.trip.level))}})
        .inc();
  }));
  subscriptions_.push_back(bus.subscribe<executor::ExecutorWaiting>([this](const executor::ExecutorWaiting& e) {
    registry_.counter(kWaits, kWaitsHelp, {{"reason", wait_reason(e.reason)}}).inc();
  }));
  subscriptions_.push_back(bus.subscribe<lab::NotificationSent>([this](const lab::NotificationSent& e) {
    const std::string channel = e.channel.empty() ? std::string("none") : e.channel;
    bool first = false;
    {
      const std::lock_guard lock(mutex_);
      first = channels_.insert(channel).second;
    }
    if (first) {
      // A channel is known once it has been used; its first failure must
      // then be an increase.
      for (const lab::NotifyEvent event : kNotifyEvents) {
        for (const bool ok : {false, true}) {
          registry_.counter(kNotifications, kNotificationsHelp,
                            {{"channel", channel}, {"event", label_of(to_string(event))}, {"ok", bool_name(ok)}});
        }
      }
    }
    registry_
        .counter(kNotifications, kNotificationsHelp,
                 {{"channel", channel}, {"event", label_of(to_string(e.event))}, {"ok", bool_name(e.ok)}})
        .inc();
  }));

  // At each scrape: how long ago, by this computer's clock.
  collector_ = registry.add_collector([this](pychron::metrics::Registry& r) {
    std::optional<double> at;
    {
      const std::lock_guard lock(mutex_);
      at = last_finished_;
    }
    if (at) r.gauge(kLastFinished, kLastFinishedHelp).set(now_() - *at);
  });
}

ExperimentMetrics::~ExperimentMetrics() {
  collector_.reset();
  for (SignalBus::Subscription& s : subscriptions_) s.reset();
}

std::size_t ExperimentMetrics::tracked_runs() const {
  const std::lock_guard lock(mutex_);
  return runs_.size();
}

// `rows` is the whole queue; what is left to do starts at the row the queue
// was started from.
void ExperimentMetrics::set_queue(std::size_t rows) {
  std::size_t from = 0;
  {
    const std::lock_guard lock(mutex_);
    from = from_row_;
  }
  registry_.gauge(kQueueRuns, kQueueRunsHelp, {{"status", "total"}}).set(static_cast<double>(rows > from ? rows - from : 0));
}

void ExperimentMetrics::set_active(bool active) {
  registry_.gauge(kQueueActive, kQueueActiveHelp).set(active ? 1.0 : 0.0);
}

}  // namespace pychron::experiment::metrics
