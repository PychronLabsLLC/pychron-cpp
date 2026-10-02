#pragma once

// ExperimentBridge (experiment-window design 5.1): the QObject between a
// LabSession and the experiment window. Same rules as CoreBridge and
// SpectrometerBridge.
//
// State out: executor, run, measurement and peak-center events, whatever
// thread published them, are marshalled to the bridge's (main) thread with a
// queued call and re-emitted as signals of the same name. SeriesUpdated and
// FitsUpdated arrive once per reading, so they are queued under a mutex and
// delivered as in-order batches per queued call.
//
// Commands in: start/stop/cancel/abort/truncate forward to the session, which
// never blocks (the executor has its own thread). check() validates a queue
// against the lab on the calling thread; it touches no hardware.
//
// No core object ever holds a QObject*: the bus handlers reach the bridge only
// through a shared Gate the destructor closes before the bridge goes away.

#include <cstdint>
#include <memory>
#include <vector>

#include <QObject>

#include "pychron/core/error.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/experiment/collect/collector.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/measurement/engine.hpp"
#include "pychron/experiment/run/state.hpp"
#include "pychron/systems/jobs/peak_center.hpp"

namespace pychron::ui {

class ExperimentBridge : public QObject {
  Q_OBJECT

 public:
  // `session` and `bus` (the session's line bus) must outlive the bridge.
  ExperimentBridge(experiment::lab::LabSession& session, SignalBus& bus, QObject* parent = nullptr);
  ~ExperimentBridge() override;
  ExperimentBridge(const ExperimentBridge&) = delete;
  ExperimentBridge& operator=(const ExperimentBridge&) = delete;

  const experiment::lab::Lab& lab() const noexcept { return session_.lab(); }
  experiment::lab::LabSession& session() noexcept { return session_; }

  experiment::lab::LabCheck check(const experiment::QueueSpec& queue) const;

  // Non-blocking. start() fails at once (Config) when a queue is running or
  // `queue` does not check.
  Result<void> start(const experiment::QueueSpec& queue, std::size_t from_row);
  // LabSession::edit: the running queue's rows after those the executor has
  // reached; returns the new queue version.
  Result<std::uint64_t> edit(std::uint64_t base, const experiment::QueueSpec& queue);
  void stop();
  void cancel();
  void abort();
  void truncate(bool quick = false);
  bool running() const;

 signals:
  void executorStateChanged(const pychron::experiment::executor::ExecutorStateChanged& event);
  void runStarted(const pychron::experiment::executor::RunStarted& event);
  void runFinished(const pychron::experiment::executor::RunFinished& event);
  void executorWaiting(const pychron::experiment::executor::ExecutorWaiting& event);
  void queueEdited(const pychron::experiment::executor::QueueEdited& event);
  void queueFrontier(const pychron::experiment::executor::QueueFrontier& event);
  void notificationSent(const pychron::experiment::lab::NotificationSent& event);
  void queueEnded(const pychron::experiment::lab::QueueEnded& event);
  void runStateChanged(const pychron::experiment::run::RunStateChanged& event);
  void blockStarted(const pychron::experiment::measurement::BlockStarted& event);
  void blockFinished(const pychron::experiment::measurement::BlockFinished& event);
  void countsProgress(const pychron::experiment::measurement::CountsProgress& event);
  void conditionalTripped(const pychron::experiment::measurement::ConditionalTripped& event);
  void peakCenterDone(const pychron::jobs::PeakCenterDone& event);
  void seriesUpdated(const std::vector<pychron::experiment::collect::SeriesUpdated>& batch);
  // Live fits, coalesced like seriesUpdated and delivered right after it.
  void fitsUpdated(const std::vector<pychron::experiment::collect::FitsUpdated>& batch);

 private:
  struct Gate;

  template <class E>
  void relay(void (ExperimentBridge::*signal)(const E&));
  void flush_series();

  experiment::lab::LabSession& session_;
  SignalBus& bus_;
  std::shared_ptr<Gate> gate_;
  std::vector<SignalBus::Subscription> subscriptions_;
};

}  // namespace pychron::ui
