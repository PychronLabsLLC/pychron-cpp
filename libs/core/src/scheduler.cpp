#include "pychron/core/scheduler.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

#include "pychron/core/signal_bus.hpp"

namespace pychron {

struct Scheduler::Job {
  JobId id = 0;
  std::string name;
  Kind kind = Kind::Periodic;
  Duration period{};
  TimePoint due{};
  std::function<bool()> body;  // returns false on a failed run
  bool running = false;
  JobStats stats;
};

Scheduler::Scheduler(const Clock& clock, SignalBus* bus) : Scheduler(clock, bus, Options{}) {}

Scheduler::Scheduler(const Clock& clock, SignalBus* bus, Options options,
                     std::shared_ptr<LogHub> log_hub)
    : clock_(clock), bus_(bus), options_(options), log_hub_(std::move(log_hub)) {
  if (log_hub_) logger_.emplace(log_hub_->logger("scheduler"));
  if (options_.threads == 0) return;
  workers_.reserve(options_.threads);
  // Time does not jump until every worker has entered the clock.
  auto hold = std::make_shared<Clock::Hold>(clock_);
  // A worker takes the mutex before anything else it does with the scheduler,
  // so it sees the count of the threads that exist, and that count is of
  // threads really started: the wait for them below, or in the destructor,
  // is then for threads that will answer.
  std::unique_lock lock(mutex_);
  try {
    for (std::size_t i = 0; i < options_.threads; ++i) {
      workers_.emplace_back([this, hold]() mutable { worker_loop(std::move(hold)); });
      ++live_workers_;
    }
  } catch (...) {
    // A thread could not be started. The destructor does not run for an
    // object whose constructor threw, so the workers there are go here.
    shutting_down_ = true;
    clock_.notify_all(work_ready_);
    while (live_workers_ != 0) clock_.wait(exited_, lock);
    lock.unlock();
    for (auto& w : workers_) w.join();
    throw;
  }
}

Scheduler::~Scheduler() {
  stop();
  {
    std::unique_lock lock(mutex_);
    shutting_down_ = true;
    clock_.notify_all(work_ready_);
    // A worker finishes what is queued first, which may take clock time, so
    // the wait for them is in the clock: a join is not, and would stop time.
    while (live_workers_ != 0) clock_.wait(exited_, lock);
  }
  for (auto& w : workers_) w.join();
}

Result<JobId> Scheduler::every(std::string name, Duration interval, Task task) {
  if (interval <= Duration::zero()) return fail(ErrorKind::Config, "interval must be > 0", name);
  return add(std::move(name), Kind::Periodic, interval, [t = std::move(task)] {
    t();
    return true;
  });
}

Result<JobId> Scheduler::scan(std::string device, Duration interval, Sampler sampler) {
  if (interval <= Duration::zero()) return fail(ErrorKind::Config, "interval must be > 0", device);
  auto body = [this, device, s = std::move(sampler)] {
    auto sample = s();
    if (!sample) {
      publish_log(LogLevel::Warn, "scan " + device + " failed: " + to_string(sample.error()));
      return false;
    }
    if (sample->device.empty()) sample->device = device;
    if (sample->ts == TimePoint{}) sample->ts = clock_.now();
    if (bus_ != nullptr) bus_->publish(*sample);
    return true;
  };
  return add(std::move(device), Kind::Periodic, interval, std::move(body));
}

Result<JobId> Scheduler::after(std::string name, Duration delay, Task task) {
  return add(std::move(name), Kind::OneShot, std::max(delay, Duration::zero()), [t = std::move(task)] {
    t();
    return true;
  });
}

Result<JobId> Scheduler::watchdog(std::string name, Duration timeout, Task on_missed) {
  if (timeout <= Duration::zero()) return fail(ErrorKind::Config, "timeout must be > 0", name);
  return add(std::move(name), Kind::Watchdog, timeout, [t = std::move(on_missed)] {
    t();
    return true;
  });
}

Result<JobId> Scheduler::add(std::string name, Kind kind, Duration period, std::function<bool()> body) {
  auto job = std::make_shared<Job>();
  job->name = std::move(name);
  job->kind = kind;
  job->period = period;
  job->body = std::move(body);
  JobId id = 0;
  {
    std::lock_guard lock(mutex_);
    id = next_id_++;
    job->id = id;
    job->due = clock_.now() + period;
    jobs_.emplace(id, std::move(job));
  }
  clock_.notify_all(wake_);
  return id;
}

Result<void> Scheduler::heartbeat(JobId id) {
  std::lock_guard lock(mutex_);
  auto it = jobs_.find(id);
  if (it == jobs_.end()) return fail(ErrorKind::Cancelled, "watchdog cancelled or unknown");
  auto& job = *it->second;
  if (job.kind != Kind::Watchdog) return fail(ErrorKind::Config, "heartbeat on a non-watchdog job", job.name);
  job.due = clock_.now() + job.period;
  return {};
}

bool Scheduler::cancel(JobId id) {
  bool removed = false;
  {
    std::lock_guard lock(mutex_);
    removed = jobs_.erase(id) > 0;
  }
  clock_.notify_all(wake_);
  return removed;
}

std::vector<std::shared_ptr<Scheduler::Job>> Scheduler::collect_due_locked(TimePoint now) {
  std::vector<std::shared_ptr<Job>> due;
  for (auto it = jobs_.begin(); it != jobs_.end();) {
    auto& job = it->second;
    if (job->due > now) {
      ++it;
      continue;
    }
    const bool overlapping = job->running;
    if (overlapping) ++job->stats.skipped_overlaps;
    switch (job->kind) {
      case Kind::Periodic: {
        // Fixed rate; ticks already in the past are dropped rather than stacked.
        const auto behind = (now - job->due) / job->period;
        job->due += (behind + 1) * job->period;
        break;
      }
      case Kind::Watchdog:
        job->due = now + job->period;
        break;
      case Kind::OneShot:
        break;
    }
    if (!overlapping) {
      job->running = true;
      ++in_flight_;
      due.push_back(job);
    }
    if (job->kind == Kind::OneShot && !overlapping) {
      it = jobs_.erase(it);
    } else {
      ++it;
    }
  }
  return due;
}

std::size_t Scheduler::run_pending() {
  std::vector<std::shared_ptr<Job>> due;
  {
    std::lock_guard lock(mutex_);
    due = collect_due_locked(clock_.now());
    if (!workers_.empty()) queue_.insert(queue_.end(), due.begin(), due.end());
  }
  if (workers_.empty()) {
    for (const auto& job : due) execute(job);
  } else if (!due.empty()) {
    clock_.notify_all(work_ready_);
  }
  return due.size();
}

void Scheduler::execute(const std::shared_ptr<Job>& job) {
  bool ok = false;
  try {
    ok = job->body();
  } catch (...) {
    publish_log(LogLevel::Error, "job " + job->name + " threw an exception");
  }
  {
    std::lock_guard lock(mutex_);
    job->running = false;
    ++job->stats.runs;
    if (!ok) ++job->stats.failures;
    --in_flight_;
  }
  clock_.notify_all(idle_);
  clock_.notify_all(wake_);
}

void Scheduler::publish_log(LogLevel level, std::string message) const {
  if (logger_) {
    if (!logger_->enabled(level)) return;
    logger_->log(level, message);  // publishes on the hub's bus
    if (bus_ == nullptr || bus_ == log_hub_->bus()) return;
  }
  if (bus_ == nullptr) return;
  bus_->publish(Log{level, "scheduler", std::move(message), clock_.now()});
}

void Scheduler::worker_loop(std::shared_ptr<Clock::Hold> started) {
  Clock::Participant participant(clock_, "scheduler.worker");
  started.reset();
  for (;;) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock lock(mutex_);
      while (!shutting_down_ && queue_.empty()) clock_.wait(work_ready_, lock);
      if (queue_.empty()) {  // shutting down with nothing left to drain
        // Said while this thread is still a participant: the destructor is
        // runnable again before the clock stops counting the worker.
        --live_workers_;
        lock.unlock();
        clock_.notify_all(exited_);
        return;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    execute(job);
  }
}

void Scheduler::start() {
  std::lock_guard lock(mutex_);
  if (dispatching_) return;
  // Time does not jump until the dispatcher has entered the clock.
  auto hold = std::make_shared<Clock::Hold>(clock_);
  dispatcher_ = std::thread([this, hold]() mutable { dispatcher_loop(std::move(hold)); });
  // Only once the thread exists: if it could not be started, stop() has
  // nothing to wait for. The dispatcher needs the mutex held here before it
  // reads either flag.
  dispatching_ = true;
  dispatcher_done_ = false;
}

void Scheduler::stop() {
  {
    std::unique_lock lock(mutex_);
    if (!dispatching_) return;
    // A job the dispatcher runs inline would wait here for itself. Refused
    // before anything is changed, so the scheduler goes on dispatching.
    if (dispatcher_.get_id() == std::this_thread::get_id()) {
      throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur),
                              "Scheduler::stop() called from a job on its own dispatcher thread");
    }
    dispatching_ = false;
    clock_.notify_all(wake_);
    // Waited for in the clock, where the dispatcher says it has finished; the
    // join below is then of a thread that is on its way out. A join alone
    // would stop time under a job the dispatcher is running inline.
    while (!dispatcher_done_) clock_.wait(exited_, lock);
  }
  if (dispatcher_.joinable()) dispatcher_.join();
}

bool Scheduler::started() const {
  std::lock_guard lock(mutex_);
  return dispatching_;
}

void Scheduler::dispatcher_loop(std::shared_ptr<Clock::Hold> started) {
  Clock::Participant participant(clock_, "scheduler.dispatch");
  started.reset();
  std::unique_lock lock(mutex_);
  while (dispatching_) {
    std::optional<TimePoint> next;
    for (const auto& [id, job] : jobs_) {
      if (job->running) continue;  // re-evaluated when it finishes
      if (!next || job->due < *next) next = job->due;
    }
    if (!next) {
      clock_.wait(wake_, lock);
      continue;
    }
    if (clock_.now() < *next) {
      clock_.wait_until(wake_, lock, *next);
      continue;
    }
    lock.unlock();
    run_pending();
    lock.lock();
  }
  // Said while this thread is still a participant: stop() is runnable again
  // before the clock stops counting the dispatcher.
  dispatcher_done_ = true;
  lock.unlock();
  clock_.notify_all(exited_);
}

void Scheduler::wait_idle() {
  std::unique_lock lock(mutex_);
  while (in_flight_ != 0) clock_.wait(idle_, lock);
}

std::optional<JobStats> Scheduler::stats(JobId id) const {
  std::lock_guard lock(mutex_);
  auto it = jobs_.find(id);
  if (it == jobs_.end()) return std::nullopt;
  return it->second->stats;
}

std::vector<NamedJobStats> Scheduler::job_stats() const {
  std::vector<NamedJobStats> out;
  {
    std::lock_guard lock(mutex_);
    out.reserve(jobs_.size());
    for (const auto& [id, job] : jobs_) {
      if (job->kind != Kind::OneShot) out.push_back(NamedJobStats{job->name, job->stats});
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const NamedJobStats& a, const NamedJobStats& b) { return a.name < b.name; });
  return out;
}

std::size_t Scheduler::job_count() const {
  std::lock_guard lock(mutex_);
  return jobs_.size();
}

}  // namespace pychron
