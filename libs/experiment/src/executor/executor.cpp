#include "pychron/experiment/executor/executor.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <regex>
#include <sstream>
#include <string_view>
#include <system_error>

#include "pychron/core/events.hpp"
#include "pychron/experiment/conditionals/metrics.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::experiment::executor {

namespace {

pychron::Duration to_clock(Duration d) {
  return std::chrono::duration_cast<pychron::Duration>(d > Duration::zero() ? d : Duration::zero());
}

std::string json_string(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          out += b;
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

bool is_blank(AnalysisType t) {
  return t == AnalysisType::BlankUnknown || t == AnalysisType::BlankAir || t == AnalysisType::BlankCocktail ||
         t == AnalysisType::BlankExtractionLine;
}

}  // namespace

std::string_view to_string(ExecutorState s) noexcept {
  switch (s) {
    case ExecutorState::Idle: return "idle";
    case ExecutorState::Preparing: return "preparing";
    case ExecutorState::Running: return "running";
    case ExecutorState::Holding: return "holding";
    case ExecutorState::StoppingAtBoundary: return "stopping";
    case ExecutorState::Cancelling: return "cancelling";
    case ExecutorState::Aborting: return "aborting";
    case ExecutorState::Finalizing: return "finalizing";
  }
  return "?";
}

bool can_transition(ExecutorState from, ExecutorState to) noexcept {
  using S = ExecutorState;
  switch (from) {
    case S::Idle: return to == S::Preparing;
    case S::Preparing: return to != S::Preparing && to != S::Idle;
    case S::Running:
      return to == S::Holding || to == S::StoppingAtBoundary || to == S::Cancelling || to == S::Aborting ||
             to == S::Finalizing;
    case S::Holding:
      return to == S::Running || to == S::StoppingAtBoundary || to == S::Cancelling || to == S::Aborting ||
             to == S::Finalizing;
    case S::StoppingAtBoundary: return to == S::Cancelling || to == S::Aborting || to == S::Finalizing;
    case S::Cancelling: return to == S::Aborting || to == S::Finalizing;
    case S::Aborting: return to == S::Finalizing;
    case S::Finalizing: return to == S::Idle;
  }
  return false;
}

std::string_view to_string(QueueEnd e) noexcept {
  switch (e) {
    case QueueEnd::Completed: return "completed";
    case QueueEnd::Stopped: return "stopped";
    case QueueEnd::Cancelled: return "cancelled";
    case QueueEnd::Aborted: return "aborted";
    case QueueEnd::Failed: return "failed";
  }
  return "?";
}

// A mutex-like hardware resource a run waits for (cancellable). Held across a
// whole phase of a run, so a run that waits for it waits through the clock.
class Executor::Resource {
 public:
  // False when `token` was requested while the resource was held.
  bool acquire(const Clock& clock, scripting::CancelToken& token) {
    {
      std::lock_guard lock(mutex_);
      if (!held_) {
        held_ = true;
        return true;
      }
    }
    // A request does not change anything mutex_ guards, so it takes the
    // mutex before it notifies: the waiter is then either before its check
    // of the token or asleep, and cannot miss it.
    const auto id = token.add_on_cancel([this, &clock] {
      std::lock_guard lock(mutex_);
      clock.notify_all(cv_);
    });
    bool acquired = true;
    {
      std::unique_lock lock(mutex_);
      while (held_) {
        if (token.requested()) {
          acquired = false;
          break;
        }
        clock.wait(cv_, lock);
      }
      if (acquired) held_ = true;
    }
    token.remove_on_cancel(id);  // without mutex_: the callback takes it
    return acquired;
  }
  void release(const Clock& clock) {
    std::lock_guard lock(mutex_);
    held_ = false;
    clock.notify_all(cv_);
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;  // waited on and notified through the clock
  bool held_ = false;
};

struct Executor::Slot {
  std::size_t row = 0;
  RunSpec spec;
  QueueSpec header;  // queue fields without the runs: the queue may change while this runs
  run::RunControl control;
  std::unique_ptr<run::Run> run;
  run::RunResult result;
  std::thread thread;
  // Under Executor::mutex_, and notified on Executor::cv_ through the clock.
  bool done = false;           // the thread has nothing more to do: it can be joined
  bool overlap_ready = false;  // the run's inlet closed
};

Executor::Executor(ExecutorContext context, ExecutorOptions options)
    : ctx_(std::move(context)),
      options_(std::move(options)),
      clock_(ctx_.services.clock != nullptr ? *ctx_.services.clock : [] () -> const Clock& {
        static SteadyClock c;
        return c;
      }()),
      extraction_(std::make_unique<Resource>()),
      spectrometer_(std::make_unique<Resource>()) {
  if (ctx_.services.clock == nullptr) ctx_.services.clock = &clock_;
}

Executor::~Executor() = default;

ExecutorState Executor::state() const {
  std::lock_guard lock(mutex_);
  return state_;
}

void Executor::set_state(ExecutorState to, std::string reason) {
  ExecutorStateChanged ev;
  {
    std::lock_guard lock(mutex_);
    if (!can_transition(state_, to)) return;
    ev = {state_, to, std::move(reason)};
    state_ = to;
  }
  if (ctx_.services.bus != nullptr) ctx_.services.bus->publish(ev);
}

std::optional<ExecutorState> Executor::requested_locked() const {
  if (end_ == QueueEnd::Aborted) return ExecutorState::Aborting;
  if (end_ == QueueEnd::Cancelled) return ExecutorState::Cancelling;
  if (stop_) return ExecutorState::StoppingAtBoundary;
  return std::nullopt;
}

void Executor::stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    clock_.notify_all(cv_);
    // Under the lock, with what it goes with: execute() clears both together
    // when its queue has ended.
    queue_token_.wake();
  }
  set_state(ExecutorState::StoppingAtBoundary, "stop requested");
}

void Executor::cancel() {
  {
    std::lock_guard lock(mutex_);
    if (!end_) {
      end_ = QueueEnd::Cancelled;
      end_reason_ = "cancelled by the operator";
    }
    // Signalled under the lock: finish() erases a slot from active_ under it
    // and then destroys the slot, so a copied pointer could dangle.
    for (auto* s : active_) s->control.cancel();
    clock_.notify_all(cv_);
    queue_token_.cancel();  // under the lock, as in stop()
  }
  set_state(ExecutorState::Cancelling, "cancel requested");
}

void Executor::abort() {
  {
    std::lock_guard lock(mutex_);
    end_ = QueueEnd::Aborted;
    end_reason_ = "aborted by the operator";
    for (auto* s : active_) s->control.abort();  // under the lock, as in cancel()
    clock_.notify_all(cv_);
    queue_token_.abort();  // under the lock, as in stop()
  }
  set_state(ExecutorState::Aborting, "abort requested");
}

void Executor::resume() {
  std::lock_guard lock(mutex_);
  if (!holding_) return;
  resume_ = true;
  clock_.notify_all(cv_);
}

Result<void> Executor::skip() {
  std::lock_guard lock(mutex_);
  if (!holding_) return fail(ErrorKind::Config, "the queue is not holding");
  if (!holding_->row) return fail(ErrorKind::Config, "the run held for has already started");
  skip_ = true;
  clock_.notify_all(cv_);
  return {};
}

void Executor::truncate(bool quick) {
  std::lock_guard lock(mutex_);
  for (auto* s : active_) {
    const auto st = s->run ? s->run->state() : run::RunState::Pending;
    if (st == run::RunState::Equilibrating || st == run::RunState::Measuring) s->control.truncate(quick);
  }
}

bool Executor::ending() const {
  std::lock_guard lock(mutex_);
  return stop_ || end_.has_value();
}

bool Executor::wait(Duration d, const std::string& reason, const std::string& run_id) {
  if (d > Duration::zero()) {
    if (ctx_.services.bus != nullptr) ctx_.services.bus->publish(ExecutorWaiting{reason, d, clock_.now(), run_id});
    if (options_.sleep) {
      options_.sleep(d);
    } else {
      queue_token_.wait_until(clock_, clock_.now() + to_clock(d));
    }
  }
  return !queue_token_.requested();
}

bool Executor::overlaps(const ExperimentQueue& queue, std::size_t row) const {
  const auto& r = queue.runs()[row];
  if (!options_.allow_overlap || r.id.type != AnalysisType::Unknown || r.overlap.duration <= Duration::zero())
    return false;
  for (std::size_t i = row + 1; i < queue.size(); ++i)
    if (!queue.runs()[i].skip) return true;
  return false;  // the last runnable row never overlaps
}

std::unique_ptr<Executor::Slot> Executor::launch(std::size_t row, RunSpec spec, QueueSpec header, int index) {
  auto slot = std::make_unique<Slot>();
  Slot* s = slot.get();
  s->row = row;
  s->spec = std::move(spec);
  s->header = std::move(header);
  s->header.runs.clear();

  run::RunHooks hooks;
  hooks.acquire_extraction = [this, s] {
    if (ctx_.services.bus != nullptr)
      ctx_.services.bus->publish(ExecutorWaiting{"extraction device", {}, clock_.now(), s->run->id()});
    return extraction_->acquire(clock_, s->control.token());
  };
  hooks.release_extraction = [this] { extraction_->release(clock_); };
  hooks.acquire_spectrometer = [this, s] {
    if (!spectrometer_->acquire(clock_, s->control.token())) return false;
    std::optional<TimePoint> pump;
    {
      // The run that had the spectrometer says when it began to pump after it
      // has let it go: that is waited for, or the time read here is the one
      // of the run before it.
      std::unique_lock lock(mutex_);
      while (pump_owed_ != nullptr && !s->control.requested()) clock_.wait(cv_, lock);
      pump = pump_started_;
    }
    const auto min = to_clock(s->spec.overlap.min_delay);
    if (pump && min > pychron::Duration::zero()) {
      const auto ready = *pump + min;
      if (clock_.now() < ready) wait(std::chrono::duration<double>(ready - clock_.now()), "minimum pump time", s->run->id());
    }
    if (s->control.requested()) {
      spectrometer_->release(clock_);
      return false;
    }
    return true;
  };
  hooks.release_spectrometer = [this, s] {
    {
      std::lock_guard lock(mutex_);
      pump_owed_ = s;  // before the next run can have the spectrometer
    }
    spectrometer_->release(clock_);
  };
  hooks.on_overlap_ready = [this, s] {
    std::lock_guard lock(mutex_);
    s->overlap_ready = true;
    clock_.notify_all(cv_);
  };
  hooks.on_pump_time_started = [this, s] {
    std::lock_guard lock(mutex_);
    pump_started_ = clock_.now();
    if (pump_owed_ == s) pump_owed_ = nullptr;
    clock_.notify_all(cv_);
  };

  s->run = std::make_unique<run::Run>(s->spec, s->header, ctx_.services, std::move(hooks), index, row);
  {
    std::lock_guard lock(mutex_);
    active_.push_back(s);
    last_started_row_ = row;
    // A cancel/abort that raced the launch still reaches this run.
    if (end_ == QueueEnd::Cancelled) s->control.cancel();
    if (end_ == QueueEnd::Aborted) s->control.abort();
  }
  if (ctx_.services.bus != nullptr)
    ctx_.services.bus->publish(RunStarted{row, s->run->id(), s->spec.id.identifier, clock_.now()});
  // Time does not move on between the thread's start and its Participant.
  auto hold = std::make_shared<Clock::Hold>(clock_);
  s->thread = std::thread([this, s, hold]() mutable {
    Clock::Participant participant(clock_, "executor.slot." + std::to_string(s->row));
    hold.reset();
    s->result = s->run->execute(s->control);
    // Said while this thread is still a participant.
    std::lock_guard lock(mutex_);
    s->done = true;
    if (pump_owed_ == s) pump_owed_ = nullptr;  // a run that failed or was aborted never says
    clock_.notify_all(cv_);
  });
  return slot;
}

void Executor::finish(ExperimentQueue& queue, Slot& slot, QueueResult& out) {
  {
    // The thread may still have to wait in the clock: it says when it is
    // done, and only then is it joined.
    std::unique_lock lock(mutex_);
    while (!slot.done) clock_.wait(cv_, lock);
  }
  slot.thread.join();
  {
    std::lock_guard lock(mutex_);
    std::erase(active_, &slot);
  }
  const auto& r = slot.result;
  RunSummary sum;
  sum.row = slot.row;
  sum.identifier = slot.spec.id.identifier;
  sum.run_id = r.uuid;
  sum.aliquot = r.aliquot;
  sum.step = r.step;
  sum.state = r.state;
  sum.truncated = r.truncated;
  sum.save_error = r.save_error;
  if (r.error) sum.error = r.error->what;
  sum.messages = r.messages;
  if (r.record) {
    sum.status = r.record->provenance.status;
    sum.failure = r.record->provenance.failure;
  }

  auto end_with = [&](QueueEnd e, std::string why) {
    std::lock_guard lock(mutex_);
    if (!end_) {
      end_ = e;
      end_reason_ = std::move(why);
      clock_.notify_all(cv_);
    }
  };
  std::size_t at = 0;
  {
    std::lock_guard lock(mutex_);
    at = last_started_row_;
  }

  std::optional<QueueEdited> edited;
  if (r.state == run::RunState::Success) {
    std::lock_guard queue_lock(queue_mutex_);
    const QueueSpec before = queue.spec();
    for (const auto& trip : r.measurement.modifications) {
      if (!is_queue_action(trip.action.type)) continue;
      if (auto change = apply_queue_action(queue, at, trip.action, ctx_.blank); change) {
        sum.queue_changes.push_back(trip.name + ": " + change->description);
      } else {
        sum.queue_changes.push_back(trip.name + ": failed: " + change.error().what);
      }
    }
    if (run_checks_ && r.record) {
      RecordMetrics metrics(*r.record, ctx_.services.arar);
      auto post = run_checks_->post_run(slot.spec, metrics, queue, at, {}, ctx_.blank);
      if (!post) {
        sum.queue_changes.push_back("post_run failed: " + post.error().what);
      } else if (*post) {
        if ((*post)->cancel_queue) end_with(QueueEnd::Cancelled, "post_run conditional '" + (*post)->trip.name + "'");
        if ((*post)->change) sum.queue_changes.push_back((*post)->trip.name + ": " + (*post)->change->description);
      }
    }
    if (!(queue.spec() == before)) edited = QueueEdited{queue.spec(), sum.queue_changes, ++version_, frozen_};
  }
  if (r.measurement.cancel_queue) {
    std::string name = "cancelation";
    for (const auto& t : r.measurement.data.trips)
      if (t.kind == ConditionalKind::Cancelation || t.action.type == ActionSpec::Type::Cancel) name = t.name;
    end_with(QueueEnd::Cancelled, "conditional '" + name + "' cancelled the queue");
  }
  // A failure for a link is the line's, not the run's (spec 4.6): the queue
  // holds rather than ends. One before the run's point of no return spent
  // nothing: that row is started again, with the aliquot it was given.
  const bool for_a_link = r.state == run::RunState::Failed && r.error && is_transient(r.error->kind) && !r.save_error;
  bool consumed = true;
  if (r.state == run::RunState::Failed) {
    std::lock_guard lock(mutex_);
    last_failed_row_ = slot.row;
  }
  switch (r.state) {
    case run::RunState::Cancelled: end_with(QueueEnd::Cancelled, "run " + sum.identifier + " cancelled"); break;
    case run::RunState::Aborted: end_with(QueueEnd::Aborted, "run " + sum.identifier + " aborted"); break;
    case run::RunState::Failed:
      if (r.unsafe) {
        // The line may not be safe: nobody goes on, nobody holds (spec 4.7).
        std::string steps;
        for (const auto& s : r.unsafe_steps) steps += (steps.empty() ? "" : ", ") + s;
        end_with(QueueEnd::Failed, "run " + sum.identifier + " left the line unsafe: " + steps);
        std::lock_guard lock(mutex_);
        unsafe_ = true;
      } else if (for_a_link && !r.past_no_return) {
        // Never an overlapped run: one goes in flight only once its inlet has
        // closed, past its point of no return.
        consumed = false;
        std::lock_guard lock(mutex_);
        retry_ = Retry{slot.row, r.aliquot, r.step, "run " + sum.identifier + " not started: " + sum.error.value_or("")};
      } else if (for_a_link && options_.hold.end_after_link_failure) {
        end_with(QueueEnd::Failed, "run " + sum.identifier + " failed for a link: " + sum.error.value_or(""));
      } else if (for_a_link) {
        std::lock_guard lock(mutex_);
        hold_next_ = "run " + sum.identifier + " failed for a link: " + sum.error.value_or("");
      } else if (!r.save_error && !options_.continue_on_failure) {
        end_with(QueueEnd::Failed, "run " + sum.identifier + " failed: " + sum.error.value_or(""));
      }
      break;
    default: break;
  }
  if (slot.spec.end_after) {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  if (consumed) previous_spec_ = slot.spec;  // an attempt started again is not the run before
  out.runs.push_back(sum);
  if (ctx_.services.bus != nullptr) {
    if (edited) ctx_.services.bus->publish(*edited);
    ctx_.services.bus->publish(RunFinished{sum});
  }
  if (ctx_.flusher != nullptr) ctx_.flusher->flush_now();
  write_state(queue, consumed ? at + 1 : slot.row, out);
}

std::optional<CheckFailure> Executor::ready(const RunSpec& next) {
  std::optional<CheckFailure> waiting;
  const auto wait_for = [&](const std::string& why, const std::vector<std::string>& links) {
    if (!waiting) waiting = CheckFailure{CheckKind::Wait, Error{ErrorKind::Io, why, {}}, {}};
    else waiting->error.what += "; " + why;
    for (const auto& l : links)
      if (std::find(waiting->links.begin(), waiting->links.end(), l) == waiting->links.end()) waiting->links.push_back(l);
  };
  for (auto* check : ctx_.checks) {
    auto r = check->check(next);
    if (!r) continue;
    if (r->kind == CheckKind::Refuse) {
      std::lock_guard lock(mutex_);
      if (!end_) {
        end_ = QueueEnd::Cancelled;
        end_reason_ = "pre-run check '" + check->name() + "': " + r->error.what;
      }
      clock_.notify_all(cv_);
      return r;
    }
    wait_for("pre-run check '" + check->name() + "': " + r->error.what, r->links);
  }
  if (ctx_.links) {
    std::vector<std::string> down;
    for (Link* l : ctx_.links(next))
      if (l->transport().health().state == HealthState::Down) down.push_back(l->transport().name());
    if (!down.empty()) {
      std::string names;
      for (const auto& d : down) names += (names.empty() ? "" : ", ") + d;
      wait_for("link down: " + names, down);
    }
  }
  return waiting;
}

Executor::HoldEnd Executor::hold(const std::string& reason, const RunSpec& next, std::optional<std::size_t> row,
                                 const ExperimentQueue& queue, const QueueResult& out,
                                 std::optional<TimePoint> until_given) {
  const TimePoint since = clock_.now();
  const TimePoint until = until_given.value_or(since + options_.hold.limit);
  last_hold_until_ = until;
  std::vector<std::string> links;
  if (auto w = ready(next)) links = w->links;
  {
    std::lock_guard lock(mutex_);
    holding_ = Held{reason, since, clock_.wall_now(), row};
    resume_ = skip_ = false;
  }
  set_state(ExecutorState::Holding, reason);
  std::size_t next_row = 0;
  {
    std::lock_guard lock(mutex_);
    next_row = row.value_or(last_started_row_ + 1);
  }
  write_state(queue, next_row, out);  // before it is said: whoever hears it may read the file
  if (ctx_.services.bus != nullptr) ctx_.services.bus->publish(ExecutorHolding{reason, links, since, until, row});

  std::optional<TimePoint> up_since;
  TimePoint next_probe = since;
  HoldEnd result = HoldEnd::Ended;
  while (true) {
    {
      std::lock_guard lock(mutex_);
      if (stop_ || end_) break;
      if (std::exchange(resume_, false)) {
        result = HoldEnd::Resumed;
        break;
      }
      if (std::exchange(skip_, false)) {
        result = HoldEnd::Skipped;
        break;
      }
    }
    TimePoint now = clock_.now();
    if (now >= until) {
      std::lock_guard lock(mutex_);
      if (!end_) {
        end_ = QueueEnd::Failed;
        std::string names;
        for (const auto& l : links) names += (names.empty() ? "" : ", ") + l;
        end_reason_ = "held for " + std::to_string(std::chrono::duration_cast<std::chrono::minutes>(now - since).count()) +
                      " min, the hold limit: " + reason + (names.empty() ? "" : " (links: " + names + ")");
      }
      break;
    }
    // Each link down is asked again (its connect step, Safe) at the interval.
    if (now >= next_probe && ctx_.links) {
      for (Link* l : ctx_.links(next))
        if (l->transport().health().state == HealthState::Down) (void)l->probe();
      next_probe = clock_.now() + options_.hold.probe_interval;
      now = clock_.now();
    }
    if (auto w = ready(next)) {
      up_since.reset();
      if (!w->links.empty()) links = w->links;
      if (w->kind == CheckKind::Refuse) break;
    } else {
      if (!up_since) up_since = now;
      if (now - *up_since >= options_.hold.settle) {
        result = HoldEnd::Back;
        break;
      }
    }
    TimePoint wake = std::min(until, ctx_.links ? next_probe : until);
    if (up_since) wake = std::min(wake, *up_since + options_.hold.settle);
    if (!ctx_.links && !up_since) wake = std::min(wake, now + options_.hold.probe_interval);  // a check is asked again
    std::unique_lock lock(mutex_);
    if (!(stop_ || end_ || resume_ || skip_)) clock_.wait_until(cv_, lock, wake);
  }
  {
    std::lock_guard lock(mutex_);
    holding_.reset();
  }
  if (result != HoldEnd::Ended) set_state(ExecutorState::Running, "holding ended");
  return result;
}

Result<std::uint64_t> Executor::edit(std::uint64_t base, std::vector<RunSpec> runs, std::string description) {
  QueueEdited e;
  {
    std::lock_guard lock(queue_mutex_);
    if (queue_ == nullptr) return fail(ErrorKind::Config, "no queue is running");
    if (base != version_)
      return fail(ErrorKind::Config, "the queue changed while it was being edited (version " + std::to_string(base) +
                                         ", now " + std::to_string(version_) + ")");
    if (runs.size() < frozen_)
      return fail(ErrorKind::Config, "the edit removes rows the executor has already reached");
    for (std::size_t i = 0; i < frozen_; ++i)
      if (!(runs[i] == queue_->runs()[i]))
        return fail(ErrorKind::Config, "row " + std::to_string(i) + " has already been reached and cannot change");
    std::vector<RunSpec> tail(std::make_move_iterator(runs.begin() + static_cast<std::ptrdiff_t>(frozen_)),
                              std::make_move_iterator(runs.end()));
    if (auto r = queue_->replace_from(frozen_, std::move(tail)); !r) return fail(r.error());
    e = QueueEdited{queue_->spec(), {std::move(description)}, ++version_, frozen_};
  }
  if (ctx_.services.bus != nullptr) ctx_.services.bus->publish(e);
  return e.version;
}

std::uint64_t Executor::queue_version() const {
  std::lock_guard lock(queue_mutex_);
  return version_;
}

std::size_t Executor::frozen_rows() const {
  std::lock_guard lock(queue_mutex_);
  return frozen_;
}

void Executor::write_state(const ExperimentQueue& queue, std::size_t next_row, const QueueResult& out) {
  if (options_.state_file.empty()) return;
  std::ostringstream j;
  j << "{\n  \"queue\": " << json_string(queue.spec().name) << ",\n  \"state\": "
    << json_string(std::string(to_string(state()))) << ",\n  \"next_row\": " << next_row << ",\n  \"runs\": [";
  for (std::size_t i = 0; i < out.runs.size(); ++i) {
    const auto& r = out.runs[i];
    j << (i ? "," : "") << "\n    {\"row\": " << r.row << ", \"identifier\": " << json_string(r.identifier)
      << ", \"run_id\": " << json_string(r.run_id) << ", \"state\": " << json_string(std::string(run::to_string(r.state)))
      << "}";
  }
  j << "\n  ]";
  {
    // What a restart, or the operator, may want to know (spec 4.6).
    std::lock_guard lock(mutex_);
    if (last_failed_row_) j << ",\n  \"last_failed_row\": " << *last_failed_row_;
    if (holding_) {
      // Wall time, as every stamp written down (seconds since 1970).
      const auto since = std::chrono::duration<double>(holding_->since_wall.time_since_epoch()).count();
      j << ",\n  \"holding\": {\"reason\": " << json_string(holding_->reason) << ", \"since\": " << since;
      if (holding_->row) j << ", \"row\": " << *holding_->row;
      j << "}";
    }
  }
  j << "\n}\n";
  std::error_code ec;
  if (options_.state_file.has_parent_path()) std::filesystem::create_directories(options_.state_file.parent_path(), ec);
  const auto tmp = options_.state_file.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << j.str();
  }
  std::filesystem::rename(tmp, options_.state_file, ec);
}

Result<std::size_t> Executor::resume_row(const std::filesystem::path& state_file) {
  std::ifstream in(state_file);
  if (!in) return fail(ErrorKind::Io, "cannot read " + state_file.string());
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  std::smatch m;
  if (!std::regex_search(text, m, std::regex(R"("next_row"\s*:\s*([0-9]+))")))
    return fail(ErrorKind::Config, state_file.string() + ": no next_row");
  return static_cast<std::size_t>(std::stoull(m[1].str()));
}

void Executor::ended(QueueResult& out, bool read) {
  std::lock_guard lock(mutex_);
  if (read) {
    out.end = end_ ? *end_ : (stop_ ? QueueEnd::Stopped : QueueEnd::Completed);
    out.reason = end_ ? end_reason_ : (stop_ ? "stopped at a run boundary" : "");
    out.code = unsafe_ ? kUnsafe : "";
  }
  unsafe_ = false;
  stop_ = false;
  end_.reset();
  end_reason_.clear();
  queue_token_.reset();
}

QueueResult Executor::execute(ExperimentQueue& queue, std::size_t from_row) {
  Clock::Participant participant(clock_, "executor.run");
  QueueResult out;
  {
    // A stop, cancel or abort already asked for is left as it is: it was
    // meant for this queue (see ended()).
    std::lock_guard lock(mutex_);
    pump_started_.reset();
    pump_owed_ = nullptr;
    active_.clear();
    holding_.reset();
    resume_ = skip_ = false;
    last_failed_row_.reset();
    hold_next_.reset();
    retry_.reset();
  }
  previous_spec_.reset();
  set_state(ExecutorState::Preparing, "queue " + queue.spec().name);
  {
    // A request kept from before the queue began shows now that there is one.
    std::optional<ExecutorState> kept;
    {
      std::lock_guard lock(mutex_);
      kept = requested_locked();
    }
    if (kept) set_state(*kept, "requested before the queue began");
  }

  if (ctx_.flusher != nullptr) {
    ctx_.flusher->flush_now();  // what an earlier session left, off this thread
  } else if (ctx_.services.save != nullptr) {
    (void)ctx_.services.save->recover();
  }
  ConditionalSet queue_level;
  if (ctx_.services.conditionals != nullptr) {
    auto set = ctx_.services.conditionals->for_queue(queue.spec());
    if (!set) {
      out.end = QueueEnd::Failed;
      out.reason = "queue conditionals: " + set.error().what;
      set_state(ExecutorState::Finalizing);
      ended(out, false);
      set_state(ExecutorState::Idle, out.reason);
      return out;
    }
    queue_level = std::move(*set);
  }
  run_checks_.emplace(std::move(queue_level));
  {
    std::lock_guard lock(queue_mutex_);
    queue_ = &queue;
    frozen_ = std::min(from_row, queue.size());
    version_ = 0;
  }

  if (options_.start_at && !ending() && clock_.now() < *options_.start_at)
    wait(std::chrono::duration<double>(*options_.start_at - clock_.now()), "scheduled start");
  if (!ending()) set_state(ExecutorState::Running);

  std::unique_ptr<Slot> in_flight;  // an overlapped run still finishing
  auto settle = [&](std::unique_ptr<Slot>& s) {
    if (!s) return;
    finish(queue, *s, out);
    s.reset();
  };
  // `pred` reads what mutex_ guards; whoever changes that notifies cv_.
  auto wait_until = [&](const std::function<bool()>& pred) {
    std::unique_lock lock(mutex_);
    while (!pred()) clock_.wait(cv_, lock);
  };

  // A hold wakes when a link's health changes.
  std::optional<SignalBus::Subscription> health;
  if (ctx_.services.bus != nullptr)
    health.emplace(ctx_.services.bus->subscribe<TransportHealth>([this](const TransportHealth&) {
      std::lock_guard lock(mutex_);
      clock_.notify_all(cv_);
    }));

  int index = 0;
  std::size_t row = from_row;
  bool delayed = false;  // the delay before the next run has been waited
  // Set by the run before: hold before the next row (`held` names it when it
  // has not started), and the aliquot and step a row started again keeps.
  std::optional<std::string> pending_hold;
  std::optional<std::size_t> held;
  std::optional<Retry> again;
  // The limit of a hold resumed while its links were still down: the next
  // hold for the same row keeps it, so a resume does not lengthen the hold.
  std::optional<std::pair<std::optional<std::size_t>, TimePoint>> resumed;
  write_state(queue, row, out);
  while (true) {
    if (ending()) break;
    {
      std::lock_guard lock(mutex_);
      if (auto r = std::exchange(retry_, std::nullopt)) {
        row = r->row;  // not consumed: the same row again
        pending_hold = r->reason;
        held = row;
        again = std::move(r);
      } else if (auto h = std::exchange(hold_next_, std::nullopt)) {
        pending_hold = std::move(h);
        held.reset();  // the run held for has started
      }
    }
    if (options_.stop_at && clock_.now() >= *options_.stop_at) {
      stop();
      break;
    }
    RunSpec spec;
    Delays delays;
    std::optional<QueueFrontier> advanced;
    {
      std::lock_guard lock(queue_mutex_);
      if (row >= queue.size()) break;
      spec = queue.runs()[row];
      delays = queue.spec().delays;
      // The executor acts on this row now (a run once its delay is over):
      // from here on an edit may not change it.
      const bool acting = spec.skip || spec.id.type == AnalysisType::Pause || in_flight || delayed;
      if (acting && row + 1 > frozen_) {
        frozen_ = row + 1;
        advanced = QueueFrontier{frozen_, version_};
      }
    }
    if (advanced && ctx_.services.bus != nullptr) ctx_.services.bus->publish(*advanced);
    if (spec.skip) {
      ++row;
      continue;
    }
    if (spec.id.type == AnalysisType::Pause) {
      settle(in_flight);
      if (!wait(spec.extraction.duration, "pause")) break;
      delayed = false;
      ++row;
      continue;
    }
    if (!in_flight && !delayed) {
      Duration d = delays.before_analyses;
      if (previous_spec_) {
        if (previous_spec_->delay_after > Duration::zero()) d = previous_spec_->delay_after;
        else if (is_blank(previous_spec_->id.type)) d = delays.after_blank;
        else d = delays.between_analyses;
      }
      if (!wait(d, "delay before " + spec.id.identifier)) break;
      delayed = true;
      continue;  // the row may have been edited during the delay: read it again
    }
    delayed = false;

    // Hold (spec 4.6): after a run that failed for a link, and while a
    // pre-run check asks the queue to wait.
    std::optional<std::string> why = std::exchange(pending_hold, std::nullopt);
    if (!why) {
      if (auto w = ready(spec)) {
        if (w->kind == CheckKind::Refuse) break;
        why = w->error.what;
        held = row;
      }
    }
    if (why) {
      // A run in flight is let finish first: nothing new starts while holding.
      settle(in_flight);
      const std::optional<TimePoint> limit =
          resumed && resumed->first == held ? std::optional<TimePoint>(resumed->second) : std::nullopt;
      const auto e = hold(*why, spec, held, queue, out, limit);
      if (e == HoldEnd::Resumed) resumed.emplace(held, last_hold_until_);
      else resumed.reset();
      held.reset();
      if (e == HoldEnd::Ended) break;
      if (e == HoldEnd::Skipped) {
        if (again && again->row == row) again.reset();
        ++row;
        continue;
      }
      delayed = true;  // checked again, without a second delay
      continue;
    }

    bool blocked = false;
    if (ctx_.pre_run_metrics != nullptr) {
      if (auto trip = run_checks_->pre_run(spec, *ctx_.pre_run_metrics)) {
        std::lock_guard lock(mutex_);
        end_ = QueueEnd::Cancelled;
        end_reason_ = "pre_run conditional '" + trip->name + "' (" + trip->check + ")";
        clock_.notify_all(cv_);
        blocked = true;
      }
    }
    if (blocked) break;

    write_state(queue, row + 1, out);  // consumed from here on
    QueueSpec header;
    bool overlapped = false;
    {
      std::lock_guard lock(queue_mutex_);
      header = queue.spec();
      overlapped = overlaps(queue, row);
    }
    resumed.reset();
    if (again && again->row == row) {
      // Started again: the aliquot and step it was given, not new ones.
      spec.id.aliquot = again->aliquot;
      spec.id.step = again->step;
      again.reset();
    }
    auto slot = launch(row, spec, std::move(header), index++);
    Slot* s = slot.get();
    if (overlapped) {
      bool overlap = false;  // the inlet closed with the run still going
      wait_until([&] {
        overlap = !s->done && s->overlap_ready;
        return s->done || s->overlap_ready || end_.has_value();
      });
      if (overlap && !ending()) {
        settle(in_flight);  // at most two runs in flight
        if (wait(spec.overlap.duration, "overlap")) {
          in_flight = std::move(slot);
          ++row;
          continue;
        }
      }
    }
    wait_until([&] { return s->done; });
    settle(in_flight);
    settle(slot);
    ++row;
  }
  settle(in_flight);
  {
    std::lock_guard lock(queue_mutex_);
    queue_ = nullptr;
  }

  set_state(ExecutorState::Finalizing);
  ended(out, true);
  write_state(queue, row, out);
  set_state(ExecutorState::Idle, std::string(to_string(out.end)));
  return out;
}

}  // namespace pychron::experiment::executor
