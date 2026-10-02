#include "experiment_bridge.hpp"

#include <mutex>
#include <utility>

#include <QMetaObject>

namespace pychron::ui {

namespace exec = experiment::executor;
namespace meas = experiment::measurement;
using experiment::collect::SeriesUpdated;

// Bus handlers run on executor/scheduler threads and may still be mid-call
// while the bridge is destroyed. They reach it only through the Gate, which
// the destructor closes under the same lock.
struct ExperimentBridge::Gate {
  std::mutex mutex;
  ExperimentBridge* target = nullptr;
  std::vector<SeriesUpdated> series;  // waiting for the main thread, oldest first
  bool flush_posted = false;
};

template <class E>
void ExperimentBridge::relay(void (ExperimentBridge::*signal)(const E&)) {
  subscriptions_.push_back(bus_.subscribe<E>([gate = gate_, signal](const E& event) {
    std::lock_guard lock(gate->mutex);
    if (ExperimentBridge* self = gate->target) {
      QMetaObject::invokeMethod(self, [self, signal, event] { emit(self->*signal)(event); }, Qt::QueuedConnection);
    }
  }));
}

ExperimentBridge::ExperimentBridge(experiment::lab::LabSession& session, SignalBus& bus, QObject* parent)
    : QObject(parent), session_(session), bus_(bus), gate_(std::make_shared<Gate>()) {
  gate_->target = this;
  relay<exec::ExecutorStateChanged>(&ExperimentBridge::executorStateChanged);
  relay<exec::RunStarted>(&ExperimentBridge::runStarted);
  relay<exec::RunFinished>(&ExperimentBridge::runFinished);
  relay<exec::ExecutorWaiting>(&ExperimentBridge::executorWaiting);
  relay<exec::QueueEdited>(&ExperimentBridge::queueEdited);
  relay<experiment::lab::QueueEnded>(&ExperimentBridge::queueEnded);
  relay<experiment::run::RunStateChanged>(&ExperimentBridge::runStateChanged);
  relay<meas::BlockStarted>(&ExperimentBridge::blockStarted);
  relay<meas::BlockFinished>(&ExperimentBridge::blockFinished);
  relay<meas::CountsProgress>(&ExperimentBridge::countsProgress);
  relay<meas::ConditionalTripped>(&ExperimentBridge::conditionalTripped);
  relay<jobs::PeakCenterDone>(&ExperimentBridge::peakCenterDone);
  subscriptions_.push_back(bus_.subscribe<SeriesUpdated>([gate = gate_](const SeriesUpdated& event) {
    std::lock_guard lock(gate->mutex);
    ExperimentBridge* self = gate->target;
    if (self == nullptr) return;
    gate->series.push_back(event);
    if (!gate->flush_posted) {
      gate->flush_posted = true;
      QMetaObject::invokeMethod(self, [self] { self->flush_series(); }, Qt::QueuedConnection);
    }
  }));
}

ExperimentBridge::~ExperimentBridge() {
  {
    std::lock_guard lock(gate_->mutex);
    gate_->target = nullptr;
  }
  subscriptions_.clear();
}

experiment::lab::LabCheck ExperimentBridge::check(const experiment::QueueSpec& queue) const {
  return experiment::lab::check_lab_queue(session_.lab(), queue);
}

Result<void> ExperimentBridge::start(const experiment::QueueSpec& queue, std::size_t from_row) {
  return session_.start(queue, from_row);
}

void ExperimentBridge::stop() { session_.stop(); }
void ExperimentBridge::cancel() { session_.cancel(); }
void ExperimentBridge::abort() { session_.abort(); }
void ExperimentBridge::truncate(bool quick) { session_.truncate(quick); }
bool ExperimentBridge::running() const { return session_.running(); }

void ExperimentBridge::flush_series() {
  std::vector<SeriesUpdated> batch;
  {
    std::lock_guard lock(gate_->mutex);
    batch.swap(gate_->series);
    gate_->flush_posted = false;
  }
  if (!batch.empty()) emit seriesUpdated(batch);
}

}  // namespace pychron::ui
