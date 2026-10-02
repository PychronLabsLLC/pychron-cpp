#include "pychron/experiment/executor/executor.hpp"

#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>

#include "pychron/experiment/conditionals/metrics.hpp"

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
    case ExecutorState::StoppingAtBoundary: return "stopping";
    case ExecutorState::Cancelling: return "cancelling";
    case ExecutorState::Aborting: return "aborting";
    case ExecutorState::Finalizing: return "finalizing";
  }
  return "?";
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

// A mutex-like hardware resource a run waits for (cancellable).
class Executor::Resource {
 public:
  bool acquire(const scripting::CancelToken& token) {
    std::unique_lock lock(mutex_);
    while (held_) {
      if (token.requested()) return false;
      cv_.wait_for(lock, std::chrono::milliseconds(10));
    }
    held_ = true;
    return true;
  }
  void release() {
    {
      std::lock_guard lock(mutex_);
      held_ = false;
    }
    cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
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
  std::atomic<bool> done{false};
  std::atomic<bool> overlap_ready{false};
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
    if (state_ == to) return;
    ev = {state_, to, std::move(reason)};
    state_ = to;
  }
  if (ctx_.services.bus != nullptr) ctx_.services.bus->publish(ev);
}

void Executor::stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  set_state(ExecutorState::StoppingAtBoundary, "stop requested");
  queue_token_.wake();
  cv_.notify_all();
}

void Executor::cancel() {
  std::vector<Slot*> slots;
  {
    std::lock_guard lock(mutex_);
    if (!end_) {
      end_ = QueueEnd::Cancelled;
      end_reason_ = "cancelled by the operator";
    }
    slots = active_;
  }
  set_state(ExecutorState::Cancelling, "cancel requested");
  for (auto* s : slots) s->control.cancel();
  queue_token_.cancel();
  cv_.notify_all();
}

void Executor::abort() {
  std::vector<Slot*> slots;
  {
    std::lock_guard lock(mutex_);
    end_ = QueueEnd::Aborted;
    end_reason_ = "aborted by the operator";
    slots = active_;
  }
  set_state(ExecutorState::Aborting, "abort requested");
  for (auto* s : slots) s->control.abort();
  queue_token_.abort();
  cv_.notify_all();
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

bool Executor::wait(Duration d, const std::string& reason) {
  if (d > Duration::zero()) {
    if (ctx_.services.bus != nullptr) ctx_.services.bus->publish(ExecutorWaiting{reason, d});
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

std::unique_ptr<Executor::Slot> Executor::launch(ExperimentQueue& queue, std::size_t row, int index) {
  auto slot = std::make_unique<Slot>();
  Slot* s = slot.get();
  s->row = row;
  s->spec = queue.runs()[row];
  s->header = queue.spec();
  s->header.runs.clear();

  run::RunHooks hooks;
  hooks.acquire_extraction = [this, s] {
    if (ctx_.services.bus != nullptr) ctx_.services.bus->publish(ExecutorWaiting{"extraction device", {}});
    return extraction_->acquire(s->control.token());
  };
  hooks.release_extraction = [this] { extraction_->release(); };
  hooks.acquire_spectrometer = [this, s] {
    if (!spectrometer_->acquire(s->control.token())) return false;
    std::optional<TimePoint> pump;
    {
      std::lock_guard lock(mutex_);
      pump = pump_started_;
    }
    const auto min = to_clock(s->spec.overlap.min_delay);
    if (pump && min > pychron::Duration::zero()) {
      const auto ready = *pump + min;
      if (clock_.now() < ready) wait(std::chrono::duration<double>(ready - clock_.now()), "minimum pump time");
    }
    if (s->control.requested()) {
      spectrometer_->release();
      return false;
    }
    return true;
  };
  hooks.release_spectrometer = [this] { spectrometer_->release(); };
  hooks.on_overlap_ready = [this, s] {
    s->overlap_ready = true;
    cv_.notify_all();
  };
  hooks.on_pump_time_started = [this] {
    std::lock_guard lock(mutex_);
    pump_started_ = clock_.now();
  };

  s->run = std::make_unique<run::Run>(s->spec, s->header, ctx_.services, std::move(hooks), index);
  {
    std::lock_guard lock(mutex_);
    active_.push_back(s);
    last_started_row_ = row;
    // A cancel/abort that raced the launch still reaches this run.
    if (end_ == QueueEnd::Cancelled) s->control.cancel();
    if (end_ == QueueEnd::Aborted) s->control.abort();
  }
  if (ctx_.services.bus != nullptr) ctx_.services.bus->publish(RunStarted{row, s->run->id(), s->spec.id.identifier});
  s->thread = std::thread([this, s] {
    s->result = s->run->execute(s->control);
    s->done = true;
    { std::lock_guard lock(mutex_); }
    cv_.notify_all();
  });
  return slot;
}

void Executor::finish(ExperimentQueue& queue, Slot& slot, QueueResult& out) {
  if (slot.thread.joinable()) slot.thread.join();
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

  auto end_with = [&](QueueEnd e, std::string why) {
    std::lock_guard lock(mutex_);
    if (!end_) {
      end_ = e;
      end_reason_ = std::move(why);
    }
  };
  std::size_t at;
  {
    std::lock_guard lock(mutex_);
    at = last_started_row_;
  }

  const QueueSpec before = queue.spec();
  if (r.state == run::RunState::Success) {
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
  }
  if (r.measurement.cancel_queue) {
    std::string name = "cancelation";
    for (const auto& t : r.measurement.data.trips)
      if (t.kind == ConditionalKind::Cancelation || t.action.type == ActionSpec::Type::Cancel) name = t.name;
    end_with(QueueEnd::Cancelled, "conditional '" + name + "' cancelled the queue");
  }
  switch (r.state) {
    case run::RunState::Cancelled: end_with(QueueEnd::Cancelled, "run " + sum.identifier + " cancelled"); break;
    case run::RunState::Aborted: end_with(QueueEnd::Aborted, "run " + sum.identifier + " aborted"); break;
    case run::RunState::Failed:
      if (!r.save_error && !options_.continue_on_failure)
        end_with(QueueEnd::Failed, "run " + sum.identifier + " failed: " + sum.error.value_or(""));
      break;
    default: break;
  }
  if (slot.spec.end_after) {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  previous_spec_ = slot.spec;
  out.runs.push_back(sum);
  if (ctx_.services.bus != nullptr) {
    if (!(queue.spec() == before)) ctx_.services.bus->publish(QueueEdited{queue.spec(), sum.queue_changes});
    ctx_.services.bus->publish(RunFinished{sum});
  }
  write_state(queue, at + 1, out);
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
  j << "\n  ]\n}\n";
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
  if (!std::regex_search(text, m, std::regex("\"next_row\"\\s*:\\s*([0-9]+)")))
    return fail(ErrorKind::Config, state_file.string() + ": no next_row");
  return static_cast<std::size_t>(std::stoull(m[1].str()));
}

QueueResult Executor::execute(ExperimentQueue& queue, std::size_t from_row) {
  QueueResult out;
  {
    std::lock_guard lock(mutex_);
    stop_ = false;
    end_.reset();
    end_reason_.clear();
    pump_started_.reset();
    active_.clear();
  }
  queue_token_.reset();
  previous_spec_.reset();
  set_state(ExecutorState::Preparing, "queue " + queue.spec().name);

  if (ctx_.services.save != nullptr) (void)ctx_.services.save->recover();
  ConditionalSet queue_level;
  if (ctx_.services.conditionals != nullptr) {
    auto set = ctx_.services.conditionals->for_queue(queue.spec());
    if (!set) {
      out.end = QueueEnd::Failed;
      out.reason = "queue conditionals: " + set.error().what;
      set_state(ExecutorState::Idle, out.reason);
      return out;
    }
    queue_level = std::move(*set);
  }
  run_checks_.emplace(std::move(queue_level));

  if (options_.start_at && clock_.now() < *options_.start_at)
    wait(std::chrono::duration<double>(*options_.start_at - clock_.now()), "scheduled start");
  if (!ending()) set_state(ExecutorState::Running);

  std::unique_ptr<Slot> in_flight;  // an overlapped run still finishing
  auto settle = [&](std::unique_ptr<Slot>& s) {
    if (!s) return;
    finish(queue, *s, out);
    s.reset();
  };
  auto wait_until = [&](const std::function<bool()>& pred) {
    std::unique_lock lock(mutex_);
    while (!pred()) cv_.wait_for(lock, std::chrono::milliseconds(10));
  };

  int index = 0;
  std::size_t row = from_row;
  write_state(queue, row, out);
  while (row < queue.size()) {
    if (ending()) break;
    if (options_.stop_at && clock_.now() >= *options_.stop_at) {
      stop();
      break;
    }
    const RunSpec spec = queue.runs()[row];
    if (spec.skip) {
      ++row;
      continue;
    }
    if (spec.id.type == AnalysisType::Pause) {
      settle(in_flight);
      if (!wait(spec.extraction.duration, "pause")) break;
      ++row;
      continue;
    }
    if (!in_flight) {
      const auto& delays = queue.spec().delays;
      Duration d = delays.before_analyses;
      if (previous_spec_) {
        if (previous_spec_->delay_after > Duration::zero()) d = previous_spec_->delay_after;
        else if (is_blank(previous_spec_->id.type)) d = delays.after_blank;
        else d = delays.between_analyses;
      }
      if (!wait(d, "delay before " + spec.id.identifier)) break;
      if (ending()) break;
    }

    bool blocked = false;
    for (auto* check : ctx_.checks) {
      if (auto r = check->check(spec); !r) {
        std::lock_guard lock(mutex_);
        end_ = QueueEnd::Cancelled;
        end_reason_ = "pre-run check '" + check->name() + "': " + r.error().what;
        blocked = true;
        break;
      }
    }
    if (!blocked && ctx_.pre_run_metrics != nullptr) {
      if (auto trip = run_checks_->pre_run(spec, *ctx_.pre_run_metrics)) {
        std::lock_guard lock(mutex_);
        end_ = QueueEnd::Cancelled;
        end_reason_ = "pre_run conditional '" + trip->name + "' (" + trip->check + ")";
        blocked = true;
      }
    }
    if (blocked) break;

    write_state(queue, row + 1, out);  // consumed from here on
    auto slot = launch(queue, row, index++);
    Slot* s = slot.get();
    if (overlaps(queue, row)) {
      wait_until([&] { return s->done.load() || s->overlap_ready.load() || end_.has_value(); });
      if (!s->done && s->overlap_ready && !ending()) {
        settle(in_flight);  // at most two runs in flight
        if (wait(spec.overlap.duration, "overlap")) {
          in_flight = std::move(slot);
          ++row;
          continue;
        }
      }
    }
    wait_until([&] { return s->done.load(); });
    settle(in_flight);
    settle(slot);
    ++row;
  }
  settle(in_flight);

  set_state(ExecutorState::Finalizing);
  {
    std::lock_guard lock(mutex_);
    out.end = end_ ? *end_ : (stop_ ? QueueEnd::Stopped : QueueEnd::Completed);
    out.reason = end_ ? end_reason_ : (stop_ ? "stopped at a run boundary" : "");
  }
  write_state(queue, row, out);
  set_state(ExecutorState::Idle, std::string(to_string(out.end)));
  return out;
}

}  // namespace pychron::experiment::executor
