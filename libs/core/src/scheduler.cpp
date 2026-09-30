#include "pychron/core/scheduler.hpp"

#include <algorithm>

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

Scheduler::Scheduler(const Clock& clock, SignalBus* bus, Options options)
    : clock_(clock), bus_(bus), options_(options) {
  workers_.reserve(options_.threads);
  for (std::size_t i = 0; i < options_.threads; ++i) workers_.emplace_back([this] { worker_loop(); });
}

Scheduler::~Scheduler() {
  stop();
  {
    std::lock_guard lock(mutex_);
    shutting_down_ = true;
  }
  work_ready_.notify_all();
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
  wake_.notify_all();
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
  wake_.notify_all();
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
    work_ready_.notify_all();
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
  idle_.notify_all();
  wake_.notify_all();
}

void Scheduler::publish_log(LogLevel level, std::string message) const {
  if (bus_ == nullptr) return;
  bus_->publish(Log{level, "scheduler", std::move(message), clock_.now()});
}

void Scheduler::worker_loop() {
  for (;;) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock lock(mutex_);
      work_ready_.wait(lock, [this] { return shutting_down_ || !queue_.empty(); });
      if (queue_.empty()) return;  // shutting down with nothing left to drain
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    execute(job);
  }
}

void Scheduler::start() {
  std::lock_guard lock(mutex_);
  if (dispatching_) return;
  dispatching_ = true;
  dispatcher_ = std::thread([this] { dispatcher_loop(); });
}

void Scheduler::stop() {
  {
    std::lock_guard lock(mutex_);
    if (!dispatching_) return;
    dispatching_ = false;
  }
  wake_.notify_all();
  if (dispatcher_.joinable()) dispatcher_.join();
}

bool Scheduler::started() const {
  std::lock_guard lock(mutex_);
  return dispatching_;
}

void Scheduler::dispatcher_loop() {
  std::unique_lock lock(mutex_);
  while (dispatching_) {
    std::optional<TimePoint> next;
    for (const auto& [id, job] : jobs_) {
      if (job->running) continue;  // re-evaluated when it finishes
      if (!next || job->due < *next) next = job->due;
    }
    if (!next) {
      wake_.wait(lock);
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
}

void Scheduler::wait_idle() {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [this] { return in_flight_ == 0; });
}

std::optional<JobStats> Scheduler::stats(JobId id) const {
  std::lock_guard lock(mutex_);
  auto it = jobs_.find(id);
  if (it == jobs_.end()) return std::nullopt;
  return it->second->stats;
}

std::size_t Scheduler::job_count() const {
  std::lock_guard lock(mutex_);
  return jobs_.size();
}

}  // namespace pychron
