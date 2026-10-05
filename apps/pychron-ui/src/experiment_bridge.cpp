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
  std::vector<experiment::collect::FitsUpdated> fits;
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
  relay<exec::QueueFrontier>(&ExperimentBridge::queueFrontier);
  relay<experiment::lab::NotificationSent>(&ExperimentBridge::notificationSent);
  relay<experiment::lab::QueueEnded>(&ExperimentBridge::queueEnded);
  relay<experiment::run::RunStateChanged>(&ExperimentBridge::runStateChanged);
  relay<experiment::run::RunNote>(&ExperimentBridge::runNote);
  relay<meas::BlockStarted>(&ExperimentBridge::blockStarted);
  relay<meas::BlockFinished>(&ExperimentBridge::blockFinished);
  relay<meas::CountsProgress>(&ExperimentBridge::countsProgress);
  relay<meas::ConditionalTripped>(&ExperimentBridge::conditionalTripped);
  relay<jobs::PeakCenterDone>(&ExperimentBridge::peakCenterDone);
  auto queue = [gate = gate_](auto&& add) {
    std::lock_guard lock(gate->mutex);
    ExperimentBridge* self = gate->target;
    if (self == nullptr) return;
    add(*gate);
    if (!gate->flush_posted) {
      gate->flush_posted = true;
      QMetaObject::invokeMethod(self, [self] { self->flush_series(); }, Qt::QueuedConnection);
    }
  };
  subscriptions_.push_back(bus_.subscribe<SeriesUpdated>(
      [queue](const SeriesUpdated& event) { queue([&](Gate& g) { g.series.push_back(event); }); }));
  subscriptions_.push_back(bus_.subscribe<experiment::collect::FitsUpdated>(
      [queue](const experiment::collect::FitsUpdated& event) { queue([&](Gate& g) { g.fits.push_back(event); }); }));
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

Result<std::uint64_t> ExperimentBridge::edit(std::uint64_t base, const experiment::QueueSpec& queue) {
  return session_.edit(base, queue);
}

void ExperimentBridge::stop() { session_.stop(); }
void ExperimentBridge::cancel() { session_.cancel(); }
void ExperimentBridge::abort() { session_.abort(); }
void ExperimentBridge::truncate(bool quick) { session_.truncate(quick); }
bool ExperimentBridge::running() const { return session_.running(); }

void ExperimentBridge::flush_series() {
  std::vector<SeriesUpdated> batch;
  std::vector<experiment::collect::FitsUpdated> fits;
  {
    std::lock_guard lock(gate_->mutex);
    batch.swap(gate_->series);
    fits.swap(gate_->fits);
    gate_->flush_posted = false;
  }
  if (!batch.empty()) emit seriesUpdated(batch);
  if (!fits.empty()) emit fitsUpdated(fits);
}

}  // namespace pychron::ui
