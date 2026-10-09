#include "pychron/systems/jobs/job_runner.hpp"

#include <ranges>
#include <string>

namespace pychron::jobs {

struct JobRunner::Slot {
  Job job;
  CancelToken token;
  std::unique_ptr<Progress> progress;
  pychron::JobId scheduled = 0;  // Scheduler job; 0 for run()
};

JobRunner::JobRunner(Spectrometer& spectrometer, Scheduler& scheduler, SignalBus& bus, const Clock& clock)
    : JobRunner(spectrometer, scheduler, bus, clock, Options{}) {}

JobRunner::JobRunner(Spectrometer& spectrometer, Scheduler& scheduler, SignalBus& bus, const Clock& clock,
                     Options options)
    : spectrometer_(spectrometer), scheduler_(scheduler), bus_(bus), clock_(clock), options_(options) {}

JobRunner::~JobRunner() {
  std::shared_ptr<Slot> slot;
  {
    std::lock_guard lock(mutex_);
    slot = current_;
  }
  if (!slot) return;
  slot->token.cancel();
  bool dropped = false;
  {
    std::lock_guard lock(mutex_);
    if (current_ == slot && slot->job.state == JobState::Queued && slot->scheduled != 0 &&
        scheduler_.cancel(slot->scheduled)) {
      current_.reset();
      dropped = true;
    }
  }
  if (!dropped) wait_idle();
}

Result<std::shared_ptr<JobRunner::Slot>> JobRunner::reserve(JobSpec spec) {
  if (!spec.body) return fail(ErrorKind::Config, "job '" + spec.kind + "' has no body", "jobs");
  if (current_) return fail(ErrorKind::Interlock, "spectrometer busy", spectrometer_.name());
  auto slot = std::make_shared<Slot>();
  slot->job.id = next_id_++;
  slot->job.kind = spec.kind;
  slot->job.spec = std::move(spec);
  slot->job.submitted = clock_.now();
  Slot* raw = slot.get();
  slot->progress = std::make_unique<Progress>([this, raw](const ProgressUpdate& update) {
    JobProgress event{raw->job.id, raw->job.kind, update};
    {
      std::lock_guard lock(mutex_);
      raw->job.progress = update;
    }
    bus_.publish(event);
  });
  current_ = slot;
  return slot;
}

Result<JobId> JobRunner::submit(JobSpec spec) {
  std::lock_guard lock(mutex_);
  auto slot = reserve(std::move(spec));
  if (!slot) return fail(slot.error());
  const JobId id = (*slot)->job.id;
  auto scheduled = scheduler_.after("job:" + std::to_string(id), Duration::zero(),
                                    [this, s = *slot] { execute(s); });
  if (!scheduled) {
    current_.reset();
    return fail(scheduled.error());
  }
  (*slot)->scheduled = *scheduled;
  return id;
}

Result<Job> JobRunner::run(JobSpec spec) {
  std::shared_ptr<Slot> slot;
  {
    std::lock_guard lock(mutex_);
    auto reserved = reserve(std::move(spec));
    if (!reserved) return fail(reserved.error());
    slot = *reserved;
  }
  execute(slot);
  std::lock_guard lock(mutex_);
  return slot->job;
}

void JobRunner::execute(const std::shared_ptr<Slot>& slot) {
  {
    std::lock_guard lock(mutex_);
    if (slot->job.state != JobState::Queued) return;  // cancelled while queued
    if (slot->token.cancelled()) {
      slot->job.state = JobState::Cancelled;
      slot->job.error = Error{ErrorKind::Cancelled, "cancelled before start", "jobs"};
    } else {
      slot->job.state = JobState::Running;
      slot->job.started = clock_.now();
    }
  }
  if (slot->job.state == JobState::Cancelled) {
    finish(slot);
    return;
  }

  auto before = spectrometer_.snapshot();
  {
    std::lock_guard lock(mutex_);
    slot->job.before = before;
  }
  bus_.publish(JobStarted{slot->job.id, slot->job.kind, std::move(before)});

  JobContext ctx{slot->job.id, spectrometer_, *slot->progress, slot->token};
  auto result = slot->job.spec.body(ctx);
  auto after = spectrometer_.snapshot();

  {
    std::lock_guard lock(mutex_);
    slot->job.after = std::move(after);
    if (result) {
      slot->job.state = JobState::Succeeded;
      slot->job.result = std::move(*result);
    } else {
      const bool cancelled = result.error().kind == ErrorKind::Cancelled || slot->token.cancelled();
      slot->job.state = cancelled ? JobState::Cancelled : JobState::Failed;
      slot->job.error = result.error();
    }
  }
  finish(slot);
}

void JobRunner::finish(const std::shared_ptr<Slot>& slot) {
  Job record;
  {
    std::lock_guard lock(mutex_);
    slot->job.finished = clock_.now();
    record = slot->job;
    if (options_.history > 0) {
      history_.push_back(record);
      while (history_.size() > options_.history) history_.pop_front();
    }
    if (current_ == slot) current_.reset();
    clock_.notify_all(idle_);
  }
  bus_.publish(JobFinished{std::move(record)});
}

Result<void> JobRunner::cancel(JobId id) {
  std::shared_ptr<Slot> slot;
  bool dropped = false;
  {
    std::lock_guard lock(mutex_);
    if (!current_ || current_->job.id != id) {
      return fail(ErrorKind::Config, "no queued or running job " + std::to_string(id), "jobs");
    }
    slot = current_;
    if (slot->job.state == JobState::Queued && slot->scheduled != 0 && scheduler_.cancel(slot->scheduled)) {
      slot->job.state = JobState::Cancelled;
      slot->job.error = Error{ErrorKind::Cancelled, "cancelled before start", "jobs"};
      dropped = true;
    }
  }
  // Outside the lock: the token hook may call into the acquisition engine.
  slot->token.cancel();
  if (dropped) finish(slot);
  return {};
}

bool JobRunner::busy() const {
  std::lock_guard lock(mutex_);
  return current_ != nullptr;
}

std::optional<JobId> JobRunner::current() const {
  std::lock_guard lock(mutex_);
  if (!current_) return std::nullopt;
  return current_->job.id;
}

std::optional<Job> JobRunner::job(JobId id) const {
  std::lock_guard lock(mutex_);
  if (current_ && current_->job.id == id) return current_->job;
  for (const auto& it : std::views::reverse(history_)) {
    if (it.id == id) return it;
  }
  return std::nullopt;
}

void JobRunner::wait_idle() const {
  std::unique_lock lock(mutex_);
  while (current_ != nullptr) clock_.wait(idle_, lock);
}

}  // namespace pychron::jobs
