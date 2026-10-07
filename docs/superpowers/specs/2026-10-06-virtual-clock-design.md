# Virtual clock: simulated time that runs as fast as the work allows

Date: 2026-10-06
Status: Implemented
Owner: Jake Ross
Builds on: `2026-09-29-instrument-control-design.md` (the injected `Clock`,
`Scheduler`, transports), `2026-09-29-experiment-system-design.md` §3 (run
threads and overlap).
Needed by: `2026-10-06-lab-simulator-design.md`.

## 1. Intent

Before this work time was already injected: `Clock` was abstract,
`SteadyClock` real time, `ManualClock` moved when a test said so, and a
`ClockPump` advanced a `ManualClock` for `--sim-speed`. Simulated runs were
still slow and still varied from run to run, for three reasons.

- A floor of real sleeps. `ManualClock::wait_until` polls every real
  millisecond and `ClockPump` stepped once per real millisecond. How fast a
  simulated run went was set by thread wake-ups, not by the work.
- Waits that bypassed the clock. `Executor::Resource::acquire` and the
  executor's `wait_until(pred)` polled every 10 ms of real time; the
  peak-center bridge in `measurement/adapters.cpp` slept 10 ms in a helper
  thread; `SimTransport::do_read` and `FramePacer::wait` carried real-time
  deadlines.
- No wall clock. `run.cpp`, `laser_system.cpp` and `switch_manager.cpp` called
  `std::chrono::system_clock::now()`, so a simulated overnight queue had
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
  // Calendar time, for stamps that are written down.
  virtual WallTime wall_now() const = 0;

  // Block on `cv` (whose mutex `lock` holds) until notified or until this
  // clock reaches `deadline`. Spurious returns are allowed; callers re-check.
  virtual void wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                          TimePoint deadline) const = 0;
  // The same with no deadline.
  virtual void wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock) const = 0;

  // Wake waiters that blocked on `cv` through this clock. A condition
  // variable waited on through a clock is notified through that clock, after
  // its state has been changed under the waiters' mutex.
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
  // RAII. While one is alive this clock does not jump. Held across the
  // start of a participant thread (section 3.7).
  class Hold;

 protected:
  // What the guards call. They do nothing unless a clock overrides them.
  virtual void enter(std::string_view name) const;
  virtual void leave() const;
  virtual void detach() const;
  virtual void reattach() const;
  virtual void hold() const;
  virtual void unhold() const;
};
```

| Clock | `wait_until` | `wait` | `notify_*` | guards | `wall_now` |
|---|---|---|---|---|---|
| `SteadyClock` | `cv.wait_until` | `cv.wait` | `cv.notify_*` | no-ops | `system_clock::now()` |
| `ManualClock` | 1 ms real poll | `cv.wait` | `cv.notify_*` | no-ops | epoch + advanced |
| `VirtualClock` | section 3 | section 3 | section 3 | counted | epoch + elapsed |

With `SteadyClock` nothing changes on real hardware: every new call is a
pass-through. `ManualClock` stays for single-threaded unit tests that step
time by hand: `advance()` wakes no untimed waiter (only a notify does), and a
thread waiting on a `ManualClock` that nobody advances waits for good. A test
with threads that wait uses a `VirtualClock`.

## 3. `VirtualClock`

```cpp
class VirtualClock final : public Clock {
 public:
  struct Options {
    double speed = std::numeric_limits<double>::infinity();  // simulated seconds per real second
    TimePoint start = TimePoint{} + std::chrono::hours(1);
    WallTime epoch = WallTime{} + std::chrono::hours(24 * 365 * 56);  // fixed; apps pass the real time
    Duration stall_report_after = std::chrono::seconds(10);  // real time; zero disables
    std::function<void(std::string)> on_stall;               // default: stderr
  };
  VirtualClock();
  explicit VirtualClock(Options options);
  void set_speed(double speed);  // at once, also on a jump being paced
  double speed() const;

  // For tests: threads between enter and leave, and threads asleep in a wait.
  std::size_t participants() const;
  std::size_t waiters() const;
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

With a finite `speed`, a jump of Δ is first paid for with `Δ / speed` of real
time, slept on an internal condition variable with the clock mutex released.
Time is continuous meanwhile: during the sleep `now()` and `wall_now()`
advance at `speed`, so a reader outside (the UI, a thread that is not a
participant) sees time pass and its own waits take their own time.

The sleep is always done by a thread that is already waiting in the clock,
never by one that is leaving, detaching or dropping a hold: such a thread
hands the sleep to the waiter with the earliest deadline and returns at once.
Only one thread sleeps at a time.

If anything may have become runnable meanwhile (a notify that wakes a waiter,
a new participant, the end of a `Detached`), a `Hold` is taken or `set_speed`
is called, the sleep ends. The time paid for is credited at that instant,
and the jump check runs again at once: if somebody is runnable time stands
where it has got to; if nobody is, the jump is still pending and goes on from
the same real instant, so that simulated time keeps to real time × speed
however often the sleep is broken.

A speed that is not positive stops time until `set_speed` is called again.
At infinite speed there is no sleep and no real-time dependence at all.

### 3.5 The rule, and what happens when it is broken

> A participant never waits for another participant except through the clock.

A participant blocked some other way (a raw `cv.wait`, `future.get()`,
`thread.join()`, a socket read) looks runnable, so time stops. It never jumps
wrongly: the failure is a stall, not a wrong result.

The converse mistake is a raw `cv.notify_*` on a condition variable that is
waited on through the clock: a `VirtualClock` waiter does not hear it. A timed
waiter then wakes at its deadline and an untimed one never does. Section 4.2
lists every such condition variable; the audit that closed the work went
through each of them and its notifies and found no raw one.

`Detached` is the escape for waits that really are on the outside world. It
is not for waiting on another participant: when that participant finishes and
the detached thread has not yet been scheduled, the clock would see nobody
runnable and jump. Waits on participants go through `wait` and `notify_*`.

A watchdog thread, started only when `stall_report_after` is non-zero, reports
when there are timed waiters, no jump has happened for that long in real time
and no pacing sleep is in progress. The report (`virtual clock stalled;
runnable: <names>`, once per stall) lists the runnable participants by name;
when there is none and a `Hold` is alive it says that the clock is held.

### 3.6 What is reproducible

The simulated time at which each wait ends is a function of the program, not
of the machine. The order in which two threads runnable at the same instant
run is not. Code that must give the same numbers on every run cannot depend
on that order; the simulator's noise is drawn accordingly (simulator spec
§5.3).

### 3.7 Starting a thread

A thread started by a participant is unknown to the clock until its own
`Participant` is constructed. If the starter blocks first, nobody is runnable
and time jumps past the child. So the starter makes a shared `Clock::Hold`
before `std::thread` and gives the child a copy; the starter drops its own
when the thread has been started and the child drops its copy once its
`Participant` is constructed. While any copy is alive the clock does not
jump and no pacing sleep begins; one in progress ends, with the time paid for
kept. When the last copy goes the jump check runs again.

On `SteadyClock` and `ManualClock` a `Hold` does nothing.

### 3.8 Mutexes held across simulated time

A thread blocked on a `std::mutex` is blocked in the kernel, where the clock
cannot see it: it looks runnable. If the holder of that mutex is waiting in
the clock (a command waiting for its reply, a connect waiting for a banner),
time stands, the holder's wait never ends, and the program stalls.

`pychron::ClockMutex` (`libs/core`, `clock_mutex.hpp`) is a mutex whose
contended `lock()` waits through the clock: a flag under an inner
`std::mutex`, waited for with `clock.wait` and released with
`clock.notify_one`. A contender is then blocked like any other waiter and
time goes on for the holder. It is Lockable (`std::lock_guard`,
`std::unique_lock`, `std::scoped_lock`), not recursive, and promises no order
among contenders. On `SteadyClock` it is a plain mutex.

`pychron::RecursiveClockMutex` (same header) is the same for a mutex its
owner takes again: an owner and a depth under the inner mutex, free once the
owner has unlocked as often as it locked. On `SteadyClock` it is a plain
recursive mutex.

A clock mutex is for a mutex that is held across a wait in clock time, a
transport call, or a call into something that does either: "one command in
flight", "one actuation at a time". A mutex that only guards a few fields for
a few lines stays a `std::mutex`, and so does the mutex of a
`std::condition_variable` (a clock mutex cannot be one): neither is ever held
across a wait other than that condition variable's own.

## 4. Call sites

### 4.1 Participants

A `Clock::Participant` is constructed first thing in each of these threads:

| Thread | Name in a stall report | File |
|---|---|---|
| scheduler dispatcher and workers | `scheduler.dispatch`, `scheduler.worker` | `libs/core/src/scheduler.cpp` |
| `QueuedTransport` worker | `transport.<name>` | `libs/transport/src/queued_transport.cpp` |
| the caller of `Executor::execute`, for the length of the call | `executor.run` | `libs/experiment/src/executor/executor.cpp` |
| executor slot (one per run) | `executor.slot.<row>` | `libs/experiment/src/executor/executor.cpp` |
| lab session (one per queue) | `lab.session` | `libs/experiment/src/lab/session.cpp` |
| run `post_eq` | `run.post_eq` | `libs/experiment/src/run/run.cpp` |
| NGX link reader | `ngx.reader` | `libs/devices/src/spectrometer/ngx_link.cpp` |
| the `elctl exp run` command, on a simulated clock | `elctl` | `apps/elctl/src/exp.cpp` |

Jobs (`JobRunner`) run on the scheduler's workers. The caller of
`Scheduler::run_pending` (threads = 0) or of a script entry point is a
participant if it wants time to wait for it; the tests that drive these
construct one. A guard nests: a thread that is already a participant (the lab
session's, inside `Executor::execute`) counts once.

Each of the threads in the table that is started by another is started under
a `Clock::Hold` (3.7) and joined after a done flag waited for through the
clock (4.2).

The notifier (`libs/experiment/src/lab/notifier.cpp`) is not a participant:
it waits untimed for work, and its work is running programs outside. A
participant that waits for it to drain does so inside a `Detached` (`elctl`
does, as it does while it waits for the queue to end and for the operator's
interrupt). Nor are the log hub flusher, the vision live feed, the
`process.cpp` reader, the store-source workers, the UI's own threads (the Qt
thread, the core and spectrometer bridges' executor threads, the laser
bridge's command and video threads, the processing and entry bridges'
workers) or the clock's watchdog: they deal with the outside world only. They
may call everything on the clock; their waits are woken by a jump but never
hold time back.

### 4.2 Waits moved onto the clock

| Site | Today | Becomes |
|---|---|---|
| `Scheduler::worker_loop`, `dispatcher_loop` (no job), `wait_idle` | `cv.wait` | `clock.wait`; their notifies go through the clock |
| `QueuedTransport::Impl::run_worker` | `cv.wait` | `clock.wait` |
| `QueuedTransport::Impl::submit` | `promise` / `future.get()` | a result slot guarded by the queue mutex, waited with `clock.wait` |
| `Executor::Resource::acquire` | `cv.wait_for(10 ms)` | `clock.wait`; `release` and the run's token notify through the clock |
| executor `wait_until(pred)` | `cv.wait_for(10 ms)` | `clock.wait`; slot completion notifies through the clock |
| `SpectrometerPeakCenter::peak_center` bridge | helper thread, `sleep_for(10 ms)` | no thread: the run's `CancelToken` gets an `on_cancel` callback that cancels this call's job token, which is linked to the job's own token when the job starts |
| `CancelToken::cancel/abort/wake` | `cv.notify_all` | notify through the clock the current waiter passed in (remembered under the token mutex; plain notify when nobody waits) |
| `IntensityStream`, `collect`, `SwitchManager`, `move_protocol`, `FramePacer`, Qtegra and polled acquirers | already `clock.wait_until` | their notifies go through the clock; `PolledAcquirer::start` and `FramePacer::start`, which move the time a frame is due, notify as well |
| `AcquisitionEngine` (`acquisition.cpp`): `start`, `stop` and `collect` waiting for polls under way and for readings | `cv.wait` | `clock.wait` on `polls_cv_` and `collect_cv_`; notified through the clock |
| `JobRunner::wait_idle`, `~JobRunner` | `cv.wait` | `clock.wait`; the end of a job notifies through the clock |
| `SimTransport::do_read` with an unsolicited source | `sleep_for(1 ms)` against a real deadline | `clock.wait_until` on the transport's condition variable, 1 ms of clock time per poll |
| `NgxLink` (`ngx_link.cpp`): command timeout, read timeout, backoff, late-reply window | `steady_clock` and `cv.wait_for` | `clock_.now()` and clock waits; its reader reads a transport that, simulated, waits in clock time |
| NGX mutexes held across a command: `NgxLink` `command_mutex_`, `connect_mutex_`, the valve mutex (`valve_mutex()`); `NgxSpectrometer` `wire_order_` | `std::mutex` | `ClockMutex` (section 3.8). `NgxLink::mutex_` and `NgxSpectrometer::acq_mutex_` guard short sections and have condition variables: they stay `std::mutex` |
| `Spectrometer::mutex_` (every hardware operation, a move's settle included) | `std::recursive_mutex` | `RecursiveClockMutex`, on the context clock |
| `LaserSystem::gate_` (every driver, camera and centering call) | `std::recursive_mutex` | `RecursiveClockMutex`, on the clock given at construction (the line's; `SteadyClock` when none is given). `LaserSystem::mutex_` guards fields and stays `std::mutex` |
| `ScanService::op_mutex_` (the engine start and stop sequences) | `std::mutex` | `ClockMutex`. The service's state mutex stays `std::mutex` |
| `SwitchManager::actuation_` (one actuation or refresh at a time, settle included), `Reconnector::mutex_` (close, open, handshake), `QtegraLink::handshake_mutex_` | `std::mutex` | `ClockMutex`. `SwitchManager::state_` stays `std::mutex` |
| `ExtractionLine::lifecycle_` (`start` talking to the devices, `stop` waiting for the jobs under way; `running`) | `std::mutex` | `ClockMutex`, on the line's clock |
| thread joins of participants (scheduler dispatcher and workers, `QueuedTransport` worker, NGX reader, `Executor` slots, `post_eq`, session) | `join()` while the thread may still wait | the thread, as its last act while still a participant, sets a done flag (or drops a live count) under the owner's mutex and notifies through the clock; the joiner waits for that with `clock.wait`, then `join()`. Never a `Detached` around the join (3.5) |

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
- camera live timeouts (the looks of a centering in `laser_system.cpp` and of
  a glow-following pattern in `pattern_runner.cpp`): a real camera on a real
  bus. The vision frame sources stamp frames with `steady_clock` unless given
  a clock;
- the log hub (record stamps, the flusher, the crash flush); the vision live
  feed; `process.cpp` (an outside program's timeout); `asio_stream.hpp` (a
  real socket or serial port);
- the notifier (`lab/notifier.cpp`, and the `Date:` of a mail in
  `lab/notifications.cpp`): it runs programs outside, on its own thread, and
  is not a participant;
- `CancelToken::remove_on_cancel` waiting for a callback that is running on
  another thread: callbacks are short and do not wait;
- `elctl exp run` waiting for the queue to end and for an interrupt: a 50 ms
  real-time poll inside a `Detached`, so that the operator is heard whatever
  simulated time is doing. The other `elctl` commands (`scan`, the laser
  commands) run on a `SteadyClock` only;
- the UI: its timers, the bridges' worker threads and their polls and
  settles (`laser_bridge.cpp`), the log dock, the plots' axes;
- inside the clocks: `SteadyClock`, `ManualClock::wait_until`'s 1 ms poll,
  and the `VirtualClock`'s pacing sleep and watchdog.

### 4.5 Wall time

`std::chrono::system_clock::now()` became `clock.wall_now()` for the time of
an analysis (`experiment/src/run/run.cpp`), the names of snapshots and the
stamps of hole corrections (`laser/src/laser_system.cpp`) and the time of a
switch's last actuation (`systems/src/switch_manager.cpp`).
`persistence/src/ids.cpp`, `processing/src/report.cpp` and `time_series.cpp`
are not on a simulated path and keep real time. The apps read
`system_clock::now()` once, for the `VirtualClock`'s epoch.

### 4.6 Apps

`--sim-speed` takes a positive, finite number, or `max` for unlimited
(`elctl` only: a UI at unlimited speed finishes a queue before it paints).
`elctl exp run --sim-speed N` and `pychron-ui --sim --sim-speed N` build a
`VirtualClock{.speed = N, .epoch = system_clock::now()}` whose stall report
goes to stderr. The line's scheduler runs with its normal dispatcher and
worker pool: the apps no longer set `options.scheduler.threads = 0` and
`run_scheduler = false`. `--sim` without `--sim-speed` keeps `SteadyClock`.

In `elctl` the command's thread is a participant from before the line starts
until it has stopped, and is `Detached` while it waits, in real time, for the
queue to end, for an interrupt, and for the notifier to drain. In the UI no
thread of the application's is a participant: the line's, the spectrometer's
and the session's threads are, and time never waits for the event loop.

`ClockPump` (`clock_pump.hpp/.cpp`), `tests/integration/sim_pump.hpp` and the
tests' own pumps are deleted and their users moved to `VirtualClock`.

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
- `Hold`: no jump while one is alive, the jump when the last one goes, and
  one taken during a pacing sleep ends it;
- pacing: speed 100, a 1 s wait takes 10 ms of real time within tolerance; a
  notify during the sleep ends it early and `now` has advanced in proportion;
  a thread that leaves hands the sleep to a waiter; `now()` moves during the
  sleep;
- stall report: a participant blocked on a raw condition variable is named;
- `wall_now` is epoch plus elapsed.

`tests/core/test_clock_mutex.cpp`: a contended `ClockMutex` and
`RecursiveClockMutex` under a holder that waits in the clock (time goes on;
with a plain mutex the test stalls), and both as plain mutexes on a
`SteadyClock`.

`tests/support/virtual_time.hpp` (`pychron::testing`) is what the tests in
simulated time share: `VirtualTimeTest`, a fixture whose dead-man aborts a
stuck test with a message after 30 s of real time; `Crew`, which starts test
threads as participants under a `Hold` and joins them through the clock;
`await_waiters`, `await_participants` and `eventually_real`, bounded real-time
waits for another thread to get as far as its sleep in the clock. The test's
own thread is a participant and moves time with `clock.sleep_for`.

Per migrated site: its existing tests stay green on `ManualClock` or
`SteadyClock`, and at least one test runs it on `VirtualClock` at infinite
speed (the `*Virtual` suites: `SchedulerVirtual`, `QueuedTransportVirtual`,
`SimTransportVirtual`, `CancelTokenVirtual`, `ExecutorVirtual`,
`SpectrometerPeakCenterVirtual`, `IntensityStreamVirtual`,
`AcquisitionEngineVirtual`, `ScanServiceVirtual`, `SpectrometerVirtual`,
`SwitchManagerVirtual`, `FramePacerVirtual`, `AdcBankVirtual`,
`QtegraAcquireVirtual`, `NgxLinkVirtual`, `NgxSystemVirtual`,
`LaserSystemVirtual`). The clock mutexes of the switch manager, the scan
service, the spectrometer, the laser system and the line each have a
contention test that stalls with a plain mutex; those of the NGX link, the
Qtegra handshake and the reconnector are covered by `test_clock_mutex.cpp`
only. `NgxLinkSteady`, `QtegraAcquireSteady`,
`AdcBankSteady` and `ExampleLineSteady` keep the hardware arrangement under
test: a `SteadyClock`, no participants, short real waits.

Acceptance, as met:

- `tests/integration`, the UI fixtures and
  `apps/elctl/tests/test_exp_commands.cpp` run on `VirtualClock` with no
  pump. The only real sleeps left in them are on purpose: the `SteadyClock`
  smoke test of the example line, and the test that interrupts a paced queue
  from outside the clock;
- the integration binary went from 62.2 s to 18.7 s of wall time
  (`tests/integration`, before and after the tests moved);
- the suite is clean under ASan/UBSan and the `*Virtual` suites and the
  integration binary under TSan, on Apple clang (2026-10-07); the simulated
  suites pass twenty times in a row. gcc and clang on Linux are CI's, on the
  pull request into `main`.
