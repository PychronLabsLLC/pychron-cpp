#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

namespace {

struct Job {
  std::function<void()> run;
  std::function<void()> cancel;
};

// Where a queued call's result is left for its caller. Guarded by the queue
// mutex; `done` is waited on and notified through the clock.
template <class T>
struct Pending {
  std::optional<Result<T>> value;
  std::condition_variable done;
};

bool retryable(const Error& e) { return e.kind == ErrorKind::Timeout || e.kind == ErrorKind::Io; }

}  // namespace

struct QueuedTransport::Impl {
  explicit Impl(TransportOptions o) : options(std::move(o)), clock(options.clock ? options.clock : &steady) {
    if (options.retries < 0) options.retries = 0;
  }

  TransportOptions options;
  SteadyClock steady;
  const Clock* clock;

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<Job> queue;
  bool stopping = false;
  std::thread worker;
  std::thread::id worker_id;
  bool worker_done = false;        // the worker has left its loop
  std::size_t callers = 0;         // threads inside submit() waiting for a result
  std::condition_variable exited;  // either of those changed; through the clock

  bool open = false;  // worker thread only (or after the worker has stopped)

  mutable std::mutex health_mutex;
  Health health;

  Error attribute(Error e) const {
    if (e.device.empty()) e.device = options.name;
    return e;
  }

  Error cancelled() const { return Error{ErrorKind::Cancelled, "transport shut down", options.name}; }
  Error not_connected() const { return Error{ErrorKind::NotConnected, "transport is not open", options.name}; }

  Duration effective(Duration timeout) const { return timeout > Duration::zero() ? timeout : options.timeout; }

  // Applies `update` to the health snapshot and publishes a TransportHealth
  // event (outside the lock) if the state changed.
  template <class F>
  void update_health(F&& update) {
    std::optional<TransportHealth> event;
    {
      std::lock_guard lock(health_mutex);
      const auto before = health.state;
      update(health);
      if (health.state != before && options.bus) {
        event = TransportHealth{options.name, health.state != HealthState::Down, health.consecutive_failures,
                                health.last_error, clock->now()};
      }
    }
    if (event) options.bus->publish(*event);
  }

  void note_success() {
    const auto now = clock->now();
    update_health([now](Health& h) {
      h.state = HealthState::Connected;
      h.last_ok = now;
      h.consecutive_failures = 0;
    });
  }

  void note_failure(const Error& e) {
    if (e.kind == ErrorKind::NotConnected || e.kind == ErrorKind::Cancelled) return;
    const bool is_open = open;
    const auto down_after = options.down_after;
    update_health([&](Health& h) {
      ++h.consecutive_failures;
      h.last_error = to_string(e);
      h.state = (!is_open || h.consecutive_failures >= down_after) ? HealthState::Down : HealthState::Degraded;
    });
  }

  void note_closed() {
    update_health([](Health& h) { h.state = HealthState::Down; });
  }

  template <class T>
  Result<T> finish(Result<T> r) {
    if (r) {
      note_success();
      return r;
    }
    Error e = attribute(std::move(r).error());
    note_failure(e);
    return fail(std::move(e));
  }

  // Runs `fn` on the worker and blocks until it completes. Calls made from
  // the worker itself (re-entrancy) run inline to avoid self-deadlock.
  //
  // The caller waits in the clock: the worker is a participant, and a caller
  // that is one too must not hold time still while the worker's I/O takes
  // some.
  template <class T>
  Result<T> submit(std::function<Result<T>()> fn) {
    auto pending = std::make_shared<Pending<T>>();
    std::unique_lock lock(mutex);
    if (stopping) return fail(cancelled());
    if (std::this_thread::get_id() == worker_id) {
      lock.unlock();
      return fn();
    }
    queue.push_back(Job{[this, pending, fn = std::move(fn)] {
                          auto r = fn();
                          {
                            std::lock_guard held(mutex);
                            pending->value = std::move(r);
                          }
                          clock->notify_all(pending->done);
                        },
                        [this, pending, cancel_error = cancelled()] {
                          {
                            std::lock_guard held(mutex);
                            pending->value = Result<T>(fail(cancel_error));
                          }
                          clock->notify_all(pending->done);
                        }});
    ++callers;
    clock->notify_one(cv);
    while (!pending->value) clock->wait(pending->done, lock);
    Result<T> r = std::move(*pending->value);
    // Said with the mutex held: this thread touches nothing of the transport
    // once it lets go, so stop() may then let the transport be destroyed.
    if (--callers == 0 && stopping) clock->notify_all(exited);
    return r;
  }

  void run_worker(std::shared_ptr<Clock::Hold> started) {
    Clock::Participant participant(*clock, "transport." + options.name);
    started.reset();
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex);
        while (!stopping && queue.empty()) clock->wait(cv, lock);
        if (queue.empty()) {
          // Said while this thread is still a participant: stop() is runnable
          // again before the clock stops counting the worker.
          worker_done = true;
          lock.unlock();
          clock->notify_all(exited);
          return;
        }
        job = std::move(queue.front());
        queue.pop_front();
      }
      job.run();
    }
  }

  // Fails the queued calls and returns once the worker has finished the call
  // it was busy with and every caller has taken its result. The wait is in
  // the clock, where the worker says it has finished: what it is busy with
  // may take clock time, and a join alone would stop that. Called on the
  // worker itself (from inside a job) it only stops the queue.
  void stop() {
    std::deque<Job> cancelled_jobs;
    {
      std::lock_guard lock(mutex);
      stopping = true;
      cancelled_jobs.swap(queue);
    }
    clock->notify_all(cv);
    for (auto& job : cancelled_jobs) job.cancel();
    if (std::this_thread::get_id() == worker_id) return;
    std::unique_lock lock(mutex);
    while (!worker_done || callers != 0) clock->wait(exited, lock);
  }
};

QueuedTransport::QueuedTransport(TransportOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {
  // Time does not jump until the worker has entered the clock.
  auto hold = std::make_shared<Clock::Hold>(*impl_->clock);
  impl_->worker = std::thread([impl = impl_.get(), hold]() mutable { impl->run_worker(std::move(hold)); });
  hold.reset();
  std::lock_guard lock(impl_->mutex);
  impl_->worker_id = impl_->worker.get_id();
}

QueuedTransport::~QueuedTransport() {
  // Derived classes already called shutdown(); this only reaps the thread if
  // one forgot, and cannot call the (now destroyed) primitives.
  impl_->stop();
  if (impl_->worker.joinable()) impl_->worker.join();
}

void QueuedTransport::shutdown() {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping && !impl_->worker.joinable()) return;
  }
  impl_->stop();
  if (impl_->worker.joinable() && std::this_thread::get_id() != impl_->worker_id) impl_->worker.join();
  if (impl_->open) {
    impl_->open = false;
    do_close();
    impl_->note_closed();
  }
}

const std::string& QueuedTransport::name() const { return impl_->options.name; }

const TransportOptions& QueuedTransport::options() const noexcept { return impl_->options; }

Health QueuedTransport::health() const {
  std::lock_guard lock(impl_->health_mutex);
  return impl_->health;
}

Result<void> QueuedTransport::open() {
  return impl_->submit<void>([this]() -> Result<void> {
    auto& impl = *impl_;
    if (impl.open) return {};
    auto r = do_open();
    impl.open = static_cast<bool>(r);
    return impl.finish(std::move(r));
  });
}

void QueuedTransport::close() {
  (void)impl_->submit<void>([this]() -> Result<void> {
    auto& impl = *impl_;
    if (!impl.open) return {};
    impl.open = false;
    do_close();
    impl.note_closed();
    return {};
  });
}

Result<Bytes> QueuedTransport::exchange(Bytes tx, ReadSpec rs, Duration timeout) {
  return impl_->submit<Bytes>([this, tx = std::move(tx), rs = std::move(rs), timeout]() -> Result<Bytes> {
    auto& impl = *impl_;
    if (!impl.open) return fail(impl.not_connected());
    const auto t = impl.effective(timeout);
    Result<Bytes> r = fail(ErrorKind::Io, "no attempt made");
    for (int attempt = 0; attempt <= impl.options.retries; ++attempt) {
      do_discard_input();
      if (auto w = do_write(tx, t); !w) {
        r = fail(w.error());
      } else {
        r = do_read(rs, t);
      }
      if (r || !retryable(r.error())) break;
    }
    return impl.finish(std::move(r));
  });
}

Result<void> QueuedTransport::write(Bytes tx) {
  return impl_->submit<void>([this, tx = std::move(tx)]() -> Result<void> {
    auto& impl = *impl_;
    if (!impl.open) return fail(impl.not_connected());
    Result<void> r;
    for (int attempt = 0; attempt <= impl.options.retries; ++attempt) {
      r = do_write(tx, impl.options.timeout);
      if (r || !retryable(r.error())) break;
    }
    return impl.finish(std::move(r));
  });
}

Result<void> QueuedTransport::transaction(std::function<Result<void>()> body) {
  // submit() runs calls made from the worker thread inline, so every
  // exchange/write/read that `body` makes executes immediately inside this
  // job, and nothing else is dequeued until body returns. Health is updated
  // by those inner calls, not by the transaction itself.
  return impl_->submit<void>(std::move(body));
}

Result<Bytes> QueuedTransport::read(ReadSpec rs, Duration timeout) {
  return impl_->submit<Bytes>([this, rs = std::move(rs), timeout]() -> Result<Bytes> {
    auto& impl = *impl_;
    if (!impl.open) return fail(impl.not_connected());
    return impl.finish(do_read(rs, impl.effective(timeout)));
  });
}

Result<std::optional<Bytes>> QueuedTransport::poll(ReadSpec rs, Duration timeout) {
  return impl_->submit<std::optional<Bytes>>([this, rs = std::move(rs), timeout]() -> Result<std::optional<Bytes>> {
    auto& impl = *impl_;
    if (!impl.open) return fail(impl.not_connected());
    auto r = do_read(rs, impl.effective(timeout));
    if (!r && r.error().kind == ErrorKind::Timeout) return std::optional<Bytes>{};  // nothing yet: not a failure
    auto done = impl.finish(std::move(r));
    if (!done) return fail(std::move(done).error());
    return std::optional<Bytes>(std::move(*done));
  });
}

}  // namespace pychron
