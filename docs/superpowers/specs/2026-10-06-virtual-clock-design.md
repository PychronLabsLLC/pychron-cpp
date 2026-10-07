# Virtual clock: simulated time that runs as fast as the work allows

Date: 2026-10-06
Status: Draft
Owner: Jake Ross
Builds on: `2026-09-29-instrument-control-design.md` (the injected `Clock`,
`Scheduler`, transports), `2026-09-29-experiment-system-design.md` §3 (run
threads and overlap).
Needed by: `2026-10-06-lab-simulator-design.md`.

## 1. Intent

Time is already injected: `Clock` is abstract, `SteadyClock` is real time,
`ManualClock` moves when a test says so, and `ClockPump` advances a
`ManualClock` for `--sim-speed`. Simulated runs are still slow and still vary
from run to run, for three reasons.

- A floor of real sleeps. `ManualClock::wait_until` polls every real
  millisecond and `ClockPump` steps once per real millisecond. How fast a
  simulated run goes is set by thread wake-ups, not by the work.
- Waits that bypass the clock. `Executor::Resource::acquire` and the
  executor's `wait_until(pred)` poll every 10 ms of real time; the peak-center
  bridge in `measurement/adapters.cpp` sleeps 10 ms in a helper thread;
  `SimTransport::do_read` and `FramePacer::wait` carry real-time deadlines.
- No wall clock. `run.cpp`, `laser_system.cpp` and `switch_manager.cpp` call
  `std::chrono::system_clock::now()`, so a simulated overnight queue has
  analysis times a few real seconds apart, different every run.

This spec adds a `VirtualClock` that jumps straight to the next deadline when
every thread living in simulated time is waiting, and routes every such wait
through the clock. Two uses, one mechanism:

- tests: unlimited speed. A twenty-minute run costs its CPU time.
- apps: `--sim-speed N`. The same clock, paced so a person can watch.

Not in scope: a single-threaded discrete-event rewrite of the executor, run or
scripting; determinism of the order in which two threads runnable at the same
instant execute.

## 2. Interface

`libs/core/include/pychron/core/clock.hpp`:

```cpp
using WallTime = std::chrono::system_clock::time_point;

class Clock {
 public:
  virtual ~Clock() = default;

  virtual TimePoint now() const = 0;
  // Calendar time for timestamps the user sees or the store keeps.
  virtual WallTime wall_now() const = 0;

  // Block on `cv` (whose mutex `lock` holds) until notified or until this
  // clock reaches `deadline`. Spurious returns are allowed; callers re-check.
  virtual void wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                          TimePoint deadline) const = 0;
  // The same with no deadline.
  virtual void wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock) const = 0;

  // Wake waiters that blocked on `cv` through this clock. A condition
  // variable waited on through a clock is notified through that clock.
  virtual void notify_one(std::condition_variable& cv) const = 0;
  virtual void notify_all(std::condition_variable& cv) const = 0;

  // Blocks the calling thread for `d` of this clock's time. In a test whose
  // main thread is a participant, this is how time is moved on.
  void sleep_for(Duration d) const;

  // RAII. The calling thread lives in this clock's time from construction
  // to destruction. `name` appears in the stall report.
  class Participant;
  // RAII. A participant is blocked on the outside world (real I/O, the UI,
  // joining a thread) and must not hold time back.
  class Detached;

 protected:
  virtual void enter(std::string_view name) const = 0;
  virtual void leave() const = 0;
  virtual void detach() const = 0;
  virtual void reattach() const = 0;
};
```

| Clock | `wait*` | `notify_*` | guards | `wall_now` |
|---|---|---|---|---|
| `SteadyClock` | `cv.wait[_until]` | `cv.notify_*` | no-ops | `system_clock::now()` |
| `ManualClock` | 1 ms real poll (as today) | `cv.notify_*` | no-ops | epoch + elapsed |
| `VirtualClock` | section 3 | section 3 | counted | epoch + elapsed |

With `SteadyClock` nothing changes on real hardware: every new call is a
pass-through. `ManualClock` stays for unit tests that step time by hand.

## 3. `VirtualClock`

```cpp
class VirtualClock final : public Clock {
 public:
  struct Options {
    double speed = std::numeric_limits<double>::infinity();  // simulated seconds per real second
    TimePoint start = TimePoint{} + std::chrono::hours(1);
    WallTime epoch = WallTime{} + std::chrono::years(56);    // fixed; apps pass the real time
    Duration stall_report_after = std::chrono::seconds(10);  // real time; zero disables
    std::function<void(std::string)> on_stall;               // default: stderr
  };
  explicit VirtualClock(Options options = {});
  void set_speed(double speed);
  double speed() const;
};
```

### 3.1 Bookkeeping

Under one internal mutex the clock keeps:

- `participants`: threads between `enter` and `leave`, with their names;
- per waiting participant: the condition variable, the deadline (or none) and
  a `runnable` flag;
- `detached`: participants inside a `Detached` guard.

A participant is **blocked** when it is inside `wait`/`wait_until` with
`runnable` false, or detached. Everything else is **runnable**.

### 3.2 The jump

When a thread is about to block and that leaves no runnable participant:

1. take the earliest deadline `d` among the waiters; if there is none (every
   waiter is untimed or detached), nothing happens: the system is idle until
   a thread outside the clock notifies;
2. pace (3.4), then set `now = d`;
3. mark every waiter whose deadline is `<= now` runnable and notify its
   condition variable.

There is no pump thread. The thread that blocks last does the jump before it
sleeps on its own condition variable; if its own deadline was the earliest it
returns at once.

`leave` and `detach` run the same check, since a thread that stops
participating can also be the last runnable one.

### 3.3 Notification

A `VirtualClock` waiter does not sleep on the caller's condition variable.
`wait*` registers a record keyed by the condition variable's address, releases
the caller's lock, and sleeps on a condition variable the record owns, under
the clock mutex; it re-takes the caller's lock after the clock mutex is
released. The caller's condition variable is only a name. This keeps one lock
order (caller's mutex, then the clock's), lets the jumping thread wake any
waiter without touching that waiter's mutex, and means a condition variable
on some thread's stack can go away without the clock holding a pointer to it.

`notify_one`/`notify_all` mark the records keyed by that condition variable
runnable under the clock mutex and wake them. A woken thread therefore counts
as runnable from the instant it is notified, not from the instant the kernel
schedules it, and the clock cannot jump past work it is about to do.
`notify_one` wakes every waiter on the key (a spurious return is allowed, and
the clock cannot know which one the caller meant). Both also call the real
`notify_all`, for a waiter that blocked on the condition variable directly.

A waiter that wakes, finds its predicate false and waits again becomes blocked
again in the usual way.

The waiter registers while it holds the caller's lock, and the notifier
changes the shared state under the same lock before it notifies, so the
ordinary condition-variable rule (no lost wake-up) carries over unchanged.

Threads that are not participants (a test's main thread, the UI thread) may
call `now`, `notify_*` and `wait*`. Their waits are registered so they can be
woken by a jump, but they never hold time back.

### 3.4 Pacing

With a finite `speed`, a jump of Δ first sleeps `Δ / speed` of real time on an
internal condition variable, with the clock mutex released. If anything
becomes runnable meanwhile (a notify from the UI thread, a new participant),
the sleep ends, `now` advances by the part of Δ already paid for, and the
jump is abandoned; the next thread to block starts another. `set_speed` also
ends the sleep.

At infinite speed there is no sleep and no real-time dependence at all.

### 3.5 The rule, and what happens when it is broken

> A participant never waits for another participant except through the clock.

A participant blocked some other way (a raw `cv.wait`, `future.get()`,
`thread.join()`, a socket read) looks runnable, so time stops. It never jumps
wrongly: the failure is a stall, not a wrong result.

The converse mistake is a raw `cv.notify_*` on a condition variable that is
waited on through the clock: a `VirtualClock` waiter does not hear it. A timed
waiter then wakes at its deadline and an untimed one never does. Section 4.2
lists every such condition variable; the last step of the work greps for raw
notifies on them.

`Detached` is the escape for waits that really are on the outside world. It
is not for waiting on another participant: when that participant finishes and
the detached thread has not yet been scheduled, the clock would see nobody
runnable and jump. Waits on participants go through `wait` and `notify_*`.

A watchdog thread, started only when `stall_report_after` is non-zero, reports
when there are timed waiters, no jump has happened for that long in real time
and no pacing sleep is in progress. The report lists the runnable
participants by name.

### 3.6 What is reproducible

The simulated time at which each wait ends is a function of the program, not
of the machine. The order in which two threads runnable at the same instant
run is not. Code that must give the same numbers on every run cannot depend
on that order; the simulator's noise is drawn accordingly (simulator spec
§5.3).

## 4. Call sites

### 4.1 Participants

A `Clock::Participant` is constructed first thing in each of these threads:

| Thread | File |
|---|---|
| scheduler dispatcher and workers | `libs/core/src/scheduler.cpp` |
| `QueuedTransport` worker | `libs/transport/src/queued_transport.cpp` |
| executor slot (one per run) | `libs/experiment/src/executor/executor.cpp` |
| lab session | `libs/experiment/src/lab/session.cpp` |
| run `post_eq` | `libs/experiment/src/run/run.cpp` |
| notifier | `libs/experiment/src/lab/notifier.cpp` |
| NGX link reader | `libs/devices/src/spectrometer/ngx_link.cpp` |

The caller of `Executor::run`, `Scheduler::run_pending` (threads = 0) or a
script entry point is a participant if it wants time to wait for it; the
elctl commands and the tests that drive these construct one.

The log hub flusher, the vision live feed, the `process.cpp` reader and the
store-source workers deal with the outside world only and are not
participants.

### 4.2 Waits moved onto the clock

| Site | Today | Becomes |
|---|---|---|
| `Scheduler::worker_loop`, `dispatcher_loop` (no job), `wait_idle` | `cv.wait` | `clock.wait`; their notifies go through the clock |
| `QueuedTransport::Impl::run_worker` | `cv.wait` | `clock.wait` |
| `QueuedTransport::Impl::submit` | `promise` / `future.get()` | a result slot guarded by the queue mutex, waited with `clock.wait` |
| `Executor::Resource::acquire` | `cv.wait_for(10 ms)` | `clock.wait`; `release` and the run's token notify through the clock |
| executor `wait_until(pred)` | `cv.wait_for(10 ms)` | `clock.wait`; slot completion notifies through the clock |
| `SpectrometerPeakCenter::peak_center` bridge | helper thread, `sleep_for(10 ms)` | no thread: the run's `CancelToken` gets an `on_cancel` callback that cancels the job token and the runner's current job |
| `CancelToken::cancel/abort/wake` | `cv.notify_all` | notify through the clock the current waiter passed in (remembered under the token mutex; plain notify when nobody waits) |
| `IntensityStream`, `collect`, `SwitchManager`, `move_protocol`, `FramePacer`, Qtegra and polled acquirers | already `clock.wait_until` | their notifies go through the clock |
| `SimTransport::do_read` with an unsolicited source | `sleep_for(1 ms)` against a real deadline | `clock.wait_until` on the transport's condition variable, 1 ms of clock time per poll |
| thread joins of participants (`Executor` slots, `post_eq`, session) | `join()` while the thread may still wait | wait for the thread's done flag through the clock, then `join()` |

### 4.3 Real-time fallbacks removed

`FramePacer::wait` (`sim_drivers.cpp`), `SimTransport::do_read`, and the
acquirer loops in `ngx.cpp`, `thermo_qtegra.cpp` and
`legacy/polled_acquirer.cpp` give up on a real-time deadline as well as the
clock's, so that a `ManualClock` nobody advances cannot hang them. Under
pacing below 1x, or on a loaded machine, that deadline fires first and the
read fails for no simulated reason. They are removed; the clock's deadline is
the only one. Tests that relied on the fallback use a `VirtualClock`.

### 4.4 Left on real time, on purpose

- script `max_wall_time` (`scripting/src/python/host_state.cpp`): a guard
  against a script that spins;
- camera live timeouts (`laser_system.cpp:775`, `pattern_runner.cpp:325`): a
  real camera on a real bus;
- `ngx_link.cpp` socket timeouts and backoff; the log hub; the UI's timers.

### 4.5 Wall time

`std::chrono::system_clock::now()` becomes `clock.wall_now()` in
`experiment/src/run/run.cpp:35`, `laser/src/laser_system.cpp:243,443` and
`systems/src/switch_manager.cpp:203`. `persistence/src/ids.cpp`,
`processing/src/report.cpp` and `time_series.cpp` are not on a simulated path
and keep real time.

### 4.6 Apps

`elctl exp run --sim-speed N` and `pychron-ui --sim --sim-speed N` build a
`VirtualClock{.speed = N, .epoch = system_clock::now()}`. The line's scheduler
runs with its normal dispatcher and worker pool: `options.scheduler.threads = 0`
and `run_scheduler = false` go away. `--sim` without `--sim-speed` keeps
`SteadyClock`. `ClockPump` (`clock_pump.hpp/.cpp`) and
`tests/integration/sim_pump.hpp` are deleted and their users moved to
`VirtualClock`.

## 5. Tests

`tests/core/test_virtual_clock.cpp`:

- one participant: `wait_until` returns with `now` equal to the deadline and
  no measurable real delay;
- two participants with different deadlines wake in deadline order, each at
  its own deadline;
- a runnable participant holds time: a waiter with a deadline does not return
  while another participant spins, and returns once that one blocks;
- notify before jump: A notifies B and then blocks until T; B must run at the
  time of the notify, not at T (repeated 10 000 times);
- an untimed waiter is woken by a non-participant's notify and time has not
  moved;
- `Detached`: a detached participant does not hold time;
- `leave` by the last runnable participant triggers the jump;
- pacing: speed 100, a 1 s wait takes 10 ms of real time within tolerance; a
  notify during the sleep ends it early and `now` has advanced in proportion;
- stall report: a participant blocked on a raw condition variable is named;
- `wall_now` is epoch plus elapsed.

Per migrated site: its existing tests stay green on `ManualClock` or
`SteadyClock`, and one test runs it on `VirtualClock` at infinite speed.

Acceptance:

- `tests/integration` and `apps/elctl/tests/test_exp_commands.cpp` run on
  `VirtualClock` with no `sleep_for` and no `Pump` in the test code;
- the wall time of `ctest -R 'integration|exp_commands'` before and after is
  recorded in the pull request;
- the suite is clean under ASan/UBSan on clang and gcc, and under TSan for
  `test_virtual_clock`.
