#include "processing_bridge.hpp"

#include <algorithm>
#include <chrono>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

namespace pychron::ui {

namespace pp = pychron::processing;

ProcessingBridge::ProcessingBridge(pp::IAnalysisSource& source, QObject* parent)
    : QObject(parent), source_(source), worker_([this] { loop(); }) {}

ProcessingBridge::~ProcessingBridge() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    if (running_cancel_) running_cancel_->store(true);
    queue_.clear();
  }
  wake_.notify_all();
  worker_.join();
}

void ProcessingBridge::submit(int channel, pp::Pipeline pipeline, std::vector<std::string> targets, Callback done) {
  {
    std::lock_guard lock(mutex_);
    const std::uint64_t serial = ++serial_;
    latest_[channel] = serial;
    // Replace a pending job of the channel; cancel a running one.
    for (auto it = queue_.begin(); it != queue_.end();) {
      if (it->channel == channel) {
        it = queue_.erase(it);
        --in_flight_;
      } else {
        ++it;
      }
    }
    if (running_channel_ == channel && running_cancel_) running_cancel_->store(true);
    queue_.push_back(Job{channel, serial, std::move(pipeline), std::move(targets), std::move(done)});
    ++in_flight_;
  }
  wake_.notify_one();
}

void ProcessingBridge::release(int channel) {
  std::lock_guard lock(mutex_);
  latest_.erase(channel);
  for (auto it = queue_.begin(); it != queue_.end();) {
    if (it->channel == channel) {
      it = queue_.erase(it);
      --in_flight_;
    } else {
      ++it;
    }
  }
  if (running_channel_ == channel && running_cancel_) running_cancel_->store(true);
  // The runner is destroyed by the worker when it next runs this channel or
  // here when idle.
  if (running_channel_ != channel) runners_.erase(channel);
}

bool ProcessingBridge::idle() const { return in_flight_.load() == 0; }

bool ProcessingBridge::wait_idle(int timeout_ms) {
  QElapsedTimer t;
  t.start();
  while (!idle()) {
    if (t.elapsed() > timeout_ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(2);
  }
  QCoreApplication::processEvents();
  return true;
}

void ProcessingBridge::loop() {
  while (true) {
    Job job;
    std::shared_ptr<std::atomic<bool>> cancel;
    pp::Runner* runner = nullptr;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      job = std::move(queue_.front());
      queue_.pop_front();
      cancel = std::make_shared<std::atomic<bool>>(false);
      running_cancel_ = cancel;
      running_channel_ = job.channel;
      auto& r = runners_[job.channel];
      if (!r) r = std::make_unique<pp::Runner>(pp::UnitRegistry::builtin(), &source_);
      runner = r.get();
    }
    const auto start = std::chrono::steady_clock::now();
    PipelineResult result;
    for (const auto& target : job.targets) {
      result.outputs.push_back(runner->run(job.pipeline, target, cancel.get()));
      for (const auto& d : runner->diagnostics())
        if (std::find(result.diagnostics.begin(), result.diagnostics.end(), d) == result.diagnostics.end())
          result.diagnostics.push_back(d);
    }
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    bool deliver = false;
    {
      std::lock_guard lock(mutex_);
      running_cancel_.reset();
      running_channel_ = -1;
      auto it = latest_.find(job.channel);
      deliver = it != latest_.end() && it->second == job.serial && !cancel->load();
      if (it == latest_.end()) runners_.erase(job.channel);
    }
    if (!deliver) {
      --in_flight_;
      continue;
    }
    QPointer<ProcessingBridge> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, job = std::move(job), result = std::move(result)]() mutable {
          if (!self) return;
          bool current = false;
          {
            std::lock_guard lock(self->mutex_);
            auto it = self->latest_.find(job.channel);
            current = it != self->latest_.end() && it->second == job.serial;
          }
          if (current && job.done) job.done(result);
          --self->in_flight_;
        },
        Qt::QueuedConnection);
  }
}

}  // namespace pychron::ui
