# Virtual Clock Implementation Plan

Executed 2026-10-06/07; see the spec for what was built (the plan's Detached-around-join steps were replaced by done flags, and ClockMutex, RecursiveClockMutex and Clock::Hold were added).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Simulated time that jumps to the next deadline when every thread living in it is waiting, so a simulated run costs its CPU time, with `--sim-speed N` pacing for apps and reproducible wall-clock timestamps.

**Architecture:** `Clock` gains `wall_now`, an untimed `wait`, `notify_one/all`, `sleep_for` and two RAII guards (`Participant`, `Detached`); on `SteadyClock` they are pass-throughs. A new `VirtualClock` counts participants and blocked waiters, and the last thread to block advances time to the earliest deadline. Every wait between threads on a simulated path moves onto the clock; `ClockPump` and the test pumps go.

**Tech Stack:** C++20, `std::condition_variable`/`std::mutex`, GoogleTest, CMake presets (`dev`, `dev-ui`).

**Spec:** `docs/superpowers/specs/2026-10-06-virtual-clock-design.md`. Read it first; section numbers below refer to it.

## Global Constraints

- Branch `feat/virtual-clock`, cut from `origin/develop`. Rename the current worktree branch before the first commit: `git branch -m feat/virtual-clock`.
- Conventional Commits with the component as scope (`feat(core): ...`, `refactor(experiment): ...`, `test(integration): ...`). End each message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Build and test: `cmake --preset dev && cmake --build build/dev -j` then `ctest --test-dir build/dev --output-on-failure -R <regex>`. UI targets: preset `dev-ui`, directory `build/dev-ui`.
- CI does not run on work branches. Before the last commit of each task run the task's tests; before Task 12's last commit run the whole suite on `dev` and `dev-ui`.
- Never skip or disable a failing test; find the cause. A failure on one compiler only is a real bug.
- With `SteadyClock` behaviour must not change: every new `Clock` method is a pass-through and both guards are no-ops.
- Lock order everywhere: the caller's mutex, then the clock's. `VirtualClock` never takes a caller's mutex while it holds its own.
- A condition variable that is waited on through a clock is notified only through that clock, and the state its waiters test is changed under the mutex they hold. No `std::atomic` flag is changed outside that mutex and then notified.
- No new third-party dependency. Qt does not appear in `libs/`.
- Lifetime: a clock outlives everything given a reference to it. In tests declare the clock first.
- No test may run git against this repository.

## Review Focus

1. Cancel or abort from a thread that is not a participant (the UI) while every participant is waiting and the clock is in a pacing sleep: the run ends within milliseconds of real time, not after the sleep. (Task 3 and Task 7 tests.)
2. A `Participant` constructed on a thread that already has one (an `elctl` command that is a participant calls `Executor::run`, which makes one too): the thread counts once and stays a participant until the outer guard goes. (Task 2 test.)
3. A periodic scheduler job with no other waiter, at unlimited speed: time runs away, and `Scheduler::stop()` and the destructor still return promptly. (Task 4 test.)
4. `wait_until` with a deadline already past, and `sleep_for` of zero or a negative duration: return at once, time does not move. (Task 2 test.)
5. A participant thread that starts while another is deciding to jump, and one that exits (`leave`) as the last runnable thread: no lost jump, no jump past the new thread's first wait. (Task 2 stress test.)

## File Structure

| File | Responsibility |
|---|---|
| `libs/core/include/pychron/core/clock.hpp`, `src/clock.cpp` | `Clock` interface, guards, `SteadyClock`, `ManualClock` |
| `libs/core/include/pychron/core/virtual_clock.hpp`, `src/virtual_clock.cpp` (new) | `VirtualClock` only |
| `libs/core/include/pychron/core/clock_pump.hpp`, `src/clock_pump.cpp`, `tests/core/test_clock_pump.cpp`, `tests/integration/sim_pump.hpp` | deleted in Task 11 |
| `tests/core/test_virtual_clock.cpp` (new) | `VirtualClock` behaviour |
| `tests/support/virtual_time.hpp` (new) | the one test helper: `eventually_real()` for assertions from non-participant threads |

---

### Task 1: The wider `Clock` interface

**Files:**
- Modify: `libs/core/include/pychron/core/clock.hpp`, `libs/core/src/clock.cpp`
- Test: `tests/core/test_clock.cpp`

**Interfaces:**
- Produces, on `pychron::Clock`:
  ```cpp
  using WallTime = std::chrono::system_clock::time_point;
  virtual WallTime wall_now() const = 0;
  virtual void wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock) const = 0;
  virtual void notify_one(std::condition_variable& cv) const = 0;
  virtual void notify_all(std::condition_variable& cv) const = 0;
  void sleep_for(Duration d) const;      // non-virtual; a local mutex and cv, loop on wait_until
  class Participant { public: Participant(const Clock& clock, std::string_view name); ~Participant(); /* no copy, no move */ };
  class Detached    { public: explicit Detached(const Clock& clock); ~Detached(); /* no copy, no move */ };
  protected:
  virtual void enter(std::string_view name) const;   // default: nothing
  virtual void leave() const;                        // default: nothing
  virtual void detach() const;                       // default: nothing
  virtual void reattach() const;                     // default: nothing
  ```
- `ManualClock(TimePoint start = TimePoint{}, WallTime epoch = WallTime{})`; `wall_now()` is `epoch + (now - start)`.

- [ ] **Step 1: Write the failing tests** in `tests/core/test_clock.cpp`:
  - `SteadyClock.WallNowIsSystemTime`: `wall_now()` within 1 s of `system_clock::now()`.
  - `SteadyClock.WaitAndNotifyPassThrough`: a thread in `clock.wait(cv, lock)` on a flag returns after another sets the flag under the mutex and calls `clock.notify_all(cv)`.
  - `ManualClock.WallNowFollowsAdvance`: epoch `WallTime{} + 1000h`, `advance(90s)` gives `epoch + 90s`.
  - `ManualClock.SleepForReturnsWhenAdvanced`: a thread in `sleep_for(5s)` joins after the test calls `advance(5s)`.
  - `Clock.GuardsAreNoOpsOnSteadyClock`: constructing `Participant` and `Detached` compiles and does nothing observable.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'SteadyClock|ManualClock|^Clock\.'`. Expected: build fails, `wall_now` is not a member.
- [ ] **Step 3: Implement** the interface above. `SteadyClock`: `cv.wait`, `cv.notify_*`, `system_clock::now()`. `ManualClock::wait` is `cv.wait(lock)`; its `wait_until` keeps the 1 ms poll.
- [ ] **Step 4: Run** the same `ctest`, then `cmake --build build/dev -j` for the whole tree (every `Clock` subclass in tests must still compile; fix fakes that derive from `Clock` by adding the four pure virtuals as pass-throughs). Expected: PASS, tree builds.
- [ ] **Step 5: Commit** `feat(core): a clock tells wall time and carries waits and notifies`.

---

### Task 2: `VirtualClock`, unlimited speed

**Files:**
- Create: `libs/core/include/pychron/core/virtual_clock.hpp`, `libs/core/src/virtual_clock.cpp`, `tests/core/test_virtual_clock.cpp`
- Modify: `libs/core/CMakeLists.txt` (add the source), `tests/core/CMakeLists.txt` (add the test)

**Interfaces:**
- Consumes: Task 1.
- Produces:
  ```cpp
  class VirtualClock final : public Clock {
   public:
    struct Options {
      double speed = std::numeric_limits<double>::infinity();
      TimePoint start = TimePoint{} + std::chrono::hours(1);
      WallTime epoch = WallTime{} + std::chrono::hours(24 * 365 * 56);
      Duration stall_report_after = std::chrono::seconds(10);   // real; zero disables
      std::function<void(std::string)> on_stall;                // default writes to stderr
    };
    explicit VirtualClock(Options options = {});
    ~VirtualClock();
    void set_speed(double speed);
    double speed() const;
    std::size_t participants() const;   // for tests
  };
  ```
  This task ignores `speed`, `stall_report_after` and `on_stall` (Task 3).

**Mechanism (spec 3.1 to 3.3, as amended):**

- One mutex `m_`. State: `now_`; a map from thread id to participant `{name, depth, detached}`; a list of waiter records `{const void* key, optional<TimePoint> deadline, bool runnable, std::thread::id owner, std::condition_variable wake}` held by `shared_ptr`.
- `enter` on a thread that already has an entry increments `depth`; `leave` decrements and erases at zero.
- `wait*(cv, lock)`: take `m_`; if a deadline is given and `now_ >= deadline`, return. Add a record keyed `&cv`. Call `maybe_jump_locked()`. `lock.unlock()`. Sleep on `record.wake` under `m_` until `record.runnable`. Remove the record, release `m_`, `lock.lock()`.
- `notify_one/all(cv)`: under `m_`, set `runnable` and signal `wake` on every record keyed `&cv`; after releasing `m_`, `cv.notify_all()`.
- `maybe_jump_locked()`: if any participant is neither detached nor the owner of a non-runnable record, return. Find the earliest deadline among non-runnable records; none, return. Set `now_` to it; mark every record with `deadline <= now_` runnable and signal it.
- `leave` and `detach` call `maybe_jump_locked()`; `reattach` only clears the flag.
- A waiter whose thread is not a participant is registered and woken the same way; it is simply never counted in the "any participant runnable" test.

- [ ] **Step 1: Write the failing tests** in `tests/core/test_virtual_clock.cpp`. Each test's own thread constructs `Clock::Participant main(clock, "test")` unless it says otherwise. `kStart` is `clock.now()` at the top of the test.
  - `SleepForJumpsToTheDeadline`: `sleep_for(20min)`; `now() == kStart + 20min`; real elapsed under 100 ms.
  - `WaitersWakeInDeadlineOrderAtTheirOwnDeadlines`: two participant threads sleep 5 s and 2 s and record `now()` on waking; main sleeps 10 s; recorded times are `kStart + 2s` and `kStart + 5s`.
  - `ARunnableParticipantHoldsTime`: thread A sleeps 1 s; main does not wait, checks 20 ms of real time later that `now() == kStart` and A has not returned; then main `sleep_for(1s)` and A is joined.
  - `NotifyMakesTheWaiterRunBeforeAnyJump`: 10 000 rounds. B waits untimed on a flag. A sets the flag, `notify_all`, then `sleep_for(1h)`. B records `now()` when it sees the flag: equals the time of the notify in every round.
  - `ANonParticipantNotifyWakesAnUntimedWaiter`: participant thread waits untimed; the test thread (no `Participant`) notifies; the waiter returns and `now() == kStart`.
  - `DetachedDoesNotHoldTime`: thread A is a participant inside `Clock::Detached`, blocked on a real `std::promise`; main `sleep_for(1s)` returns.
  - `LeaveByTheLastRunnableThreadJumps`: thread A sleeps 3 s; thread B is a participant that exits without waiting; the test thread is not a participant; A returns with `now() == kStart + 3s`.
  - `NestedParticipantCountsOnce`: two guards on the main thread, `participants() == 1`; after the inner one is destroyed still 1 and `ARunnableParticipantHoldsTime` still holds. (Review Focus 2)
  - `PastDeadlineAndNonPositiveSleepReturnAtOnce`: `wait_until` with `kStart - 1s`, `sleep_for(0s)`, `sleep_for(-1s)`; `now() == kStart`. (Review Focus 4)
  - `ThreadsStartingAndLeavingNeverLoseAJump`: 2 000 rounds; in each, main starts 4 participant threads that each `sleep_for(i * 1ms)` then exit, and main `sleep_for(10ms)`; every round ends with `now()` advanced by exactly 10 ms and all threads joined within 5 s real. (Review Focus 5)
  - `WallNowIsEpochPlusElapsed`: after `sleep_for(90s)`, `wall_now() == options.epoch + 90s`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R VirtualClock`. Expected: build fails, no `virtual_clock.hpp`.
- [ ] **Step 3: Implement** `VirtualClock` per the mechanism above.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R VirtualClock --repeat until-fail:20`. Expected: PASS every repeat.
- [ ] **Step 5: Run under TSan.** `cmake -S . -B build/tsan --preset dev -DPYCHRON_SANITIZE=thread && cmake --build build/tsan -j --target pychron_core_tests && ctest --test-dir build/tsan -R VirtualClock`. Expected: PASS, no data race report.
- [ ] **Step 6: Commit** `feat(core): a virtual clock that jumps to the next deadline`.

---

### Task 3: Pacing and the stall report

**Files:**
- Modify: `libs/core/src/virtual_clock.cpp`, `tests/core/test_virtual_clock.cpp`

**Interfaces:**
- Produces: `Options::speed`, `set_speed`, `Options::stall_report_after`, `Options::on_stall` take effect.

**Mechanism (spec 3.4, 3.5):**

- In `maybe_jump_locked()` with a finite speed and a jump of Δ: record `real_start`, set `pacing_ = true`, sleep on an internal cv under `m_` for `Δ / speed` or until `interrupt_` is set. On an interrupt, advance `now_` by `min(Δ, real_elapsed * speed)`, wake records now due, and return without completing the jump. Only one thread paces at a time; another thread that blocks while `pacing_` is set does not start a second jump.
- `interrupt_` is set by `notify_*` when it makes a record runnable, by `enter`, by `reattach`, and by `set_speed`.
- Stall watchdog: one thread, started in the constructor when `stall_report_after > 0`, joined in the destructor. Every `stall_report_after / 4` of real time: if a timed record exists, `pacing_` is false and the jump counter has not changed for `stall_report_after`, call `on_stall` once per stall with `"virtual clock stalled; runnable: <name>, <name>"` (participants that are neither detached nor waiting).

- [ ] **Step 1: Write the failing tests:**
  - `PacingTakesDeltaOverSpeed`: `speed = 100`; `sleep_for(1s)` takes between 8 ms and 60 ms of real time; `now() == kStart + 1s`.
  - `ANotifyEndsThePacingSleep`: `speed = 1`; participant thread W waits untimed on a flag; main `sleep_for(60s)` in a helper participant thread; the test thread (not a participant) waits 50 ms real, sets the flag and notifies. W returns within 50 ms real of the notify, and `now() - kStart` is between 20 ms and 500 ms. (Review Focus 1)
  - `SetSpeedTakesEffectDuringASleep`: `speed = 1`, a participant in `sleep_for(100s)`; `set_speed(infinity)` from the test thread; the sleeper returns within 100 ms real at `kStart + 100s`.
  - `AStallNamesTheRunnableParticipant`: `stall_report_after = 50ms`, `on_stall` appends to a vector; participant "sleeper" in `sleep_for(1s)`; participant "culprit" blocked on a raw `std::condition_variable`. Within 1 s real the vector has one string containing `culprit` and not `sleeper`. Then release the culprit.
  - `NoStallReportWhilePacing`: `speed = 1`, `stall_report_after = 20ms`, one participant in `sleep_for(200ms)`: `on_stall` is not called.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R VirtualClock`. Expected: the five new tests FAIL (pacing test returns instantly, stall never reported).
- [ ] **Step 3: Implement** pacing and the watchdog.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R VirtualClock --repeat until-fail:20`, then the TSan command of Task 2. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): the virtual clock paces to a speed and reports a stall`.

---

### Task 4: Scheduler on the clock

**Files:**
- Modify: `libs/core/src/scheduler.cpp`
- Test: `tests/core/test_scheduler.cpp`

**Interfaces:**
- Consumes: `Clock::wait`, `notify_*`, `Participant`.
- Produces: `Scheduler` threads are participants named `scheduler.dispatch` and `scheduler.worker`. Public API unchanged.

Changes: `worker_loop` and `dispatcher_loop` start with a `Clock::Participant`. `work_ready_.wait`, `wake_.wait` and `idle_.wait` become `clock_.wait` in a predicate loop. All seven `notify_*` calls on `wake_`, `work_ready_`, `idle_` go through `clock_`. `stop()` and the destructor join threads that may be waiting in clock time: wrap each `join()` in `Clock::Detached` when the calling thread is a participant (the joined thread has been told to stop and notified, so it does not need time to move).

- [ ] **Step 1: Write the failing tests** (suite `SchedulerVirtual`):
  - `PeriodicJobRunsAtExactTimes`: `VirtualClock`, started scheduler, `every("tick", 1s, record now())`; main is a participant and `sleep_for(10s + 1ms)`; `wait_idle()`; recorded times are exactly `kStart + 1s .. + 10s`.
  - `AfterRunsOnceAtItsDelay`: `after("once", 90s, ...)` recorded at `kStart + 90s`.
  - `WatchdogFiresWithoutHeartbeat`: `watchdog("w", 5s, ...)`, main `sleep_for(6s)`: fired once at `kStart + 5s`.
  - `StopReturnsWhileTimeRunsAway`: a `every(1s)` job, the test thread is **not** a participant; after 20 ms real, `stop()` and the destructor return within 1 s real; the job ran at least once. (Review Focus 3)
  - `RunPendingInlineStillWorks`: `Options{0}`, no `start()`; main participant loop `sleep_for(1s); run_pending();` five times runs the job five times.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R SchedulerVirtual`. Expected: FAIL. `PeriodicJobRunsAtExactTimes` hangs or stalls (workers in a raw `cv.wait` look runnable); the test's 60 s ctest timeout reports it.
- [ ] **Step 3: Implement** the changes above.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'Scheduler'`. Expected: PASS, existing `Scheduler.*` tests included.
- [ ] **Step 5: Commit** `refactor(core): the scheduler waits and notifies through its clock`.

---

### Task 5: Transports on the clock

**Files:**
- Modify: `libs/transport/src/queued_transport.cpp`, `libs/transport/src/sim_transport.cpp`
- Test: `tests/transport/test_queued_transport.cpp`, `tests/transport/test_sim_transport.cpp` (use the existing files of those components; names per `tests/transport/CMakeLists.txt`)

**Interfaces:**
- Consumes: Tasks 1 to 3.
- Produces: no API change. The worker is a participant named `transport.<options.name>`.

Changes:
- `QueuedTransport::Impl::run_worker`: `Clock::Participant` first; `cv.wait` becomes `clock->wait`.
- `QueuedTransport::Impl::submit`: replace `std::promise`/`future.get()` with a result slot: `struct Pending<T> { std::optional<Result<T>> value; std::condition_variable done; }` in a `shared_ptr`, set under `mutex` by the job's `run` and `cancel` closures, followed by `clock->notify_all(pending->done)`; the caller waits `clock->wait(pending->done, lock)` until `value`. `cv.notify_one()` after enqueue becomes `clock->notify_one(cv)`.
- Shutdown: every `cv.notify_*` in the file goes through `clock`; the join of the worker is wrapped in `Clock::Detached`.
- `SimTransport::do_read`: delete `real_deadline`. With an unsolicited source, compute `deadline = clock.now() + timeout`, and between polls `clock.wait_until(state_->cv, lock, min(deadline, clock.now() + 1ms))` on a condition variable added to the state; leave the loop when `clock.now() >= deadline`. If `SimTransport` has no clock reference today, take the one in `TransportOptions` the same way `QueuedTransport` does.

- [ ] **Step 1: Write the failing tests:**
  - `QueuedTransportVirtual.CallerWaitsForTheWorkerInClockTime`: `VirtualClock`; a `SimTransport` hook that calls `clock.sleep_for(2s)` before replying; the calling participant's `query` returns the reply and `now() == kStart + 2s`; real time under 200 ms.
  - `QueuedTransportVirtual.ShutdownCancelsAPendingCall`: a call blocked in the hook as above is failed with `ErrorKind::Cancelled` when the transport is destroyed from a non-participant thread.
  - `SimTransportVirtual.UnsolicitedReadTimesOutInClockTime`: an unsolicited source that never produces; `read` with a 3 s timeout fails with `ErrorKind::Timeout` at `kStart + 3s`, real time under 200 ms.
  - `SimTransportVirtual.UnsolicitedReadSeesLateInput`: the source produces a frame once `clock.now() >= kStart + 1s`; `read` with a 3 s timeout returns it at a time in `[kStart + 1s, kStart + 1s + 2ms]`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'QueuedTransportVirtual|SimTransportVirtual'`. Expected: FAIL: the first stalls (caller in `future.get()` looks runnable), the third takes 3 s of real time and fails the real-time bound.
- [ ] **Step 3: Implement** the changes above.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'transport|Transport'`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(transport): a queued call and a simulated read wait on the clock`.

---

### Task 6: `CancelToken` and the peak-center bridge

**Files:**
- Modify: `libs/scripting/include/pychron/scripting/cancel_token.hpp`, `libs/scripting/src/cancel_token.cpp`, `libs/experiment/src/measurement/adapters.cpp`
- Create: `tests/scripting/test_cancel_token.cpp` (add it to `tests/scripting/CMakeLists.txt`)
- Test: `tests/integration/test_measurement_sim.cpp` (the file that covers `SpectrometerPeakCenter`)

**Interfaces:**
- Produces on `scripting::CancelToken`:
  ```cpp
  // Called once, on the cancelling thread, on each cancel() or abort() while
  // registered. Returns an id for remove_on_cancel. If the token is already
  // requested the callback runs before add_on_cancel returns.
  std::uint64_t add_on_cancel(std::function<void()> callback);
  void remove_on_cancel(std::uint64_t id);   // returns after a running callback has finished
  // Untimed form of wait_until.
  WaitResult wait(const Clock& clock);
  // Wakes waiters without changing the token (a resource they wait for changed).
  void notify(const Clock& clock);
  ```
- `wait_until` and `wait` remember the clock they were called with (`const Clock* waiter_clock_`, under the token mutex, a count of waiters, cleared when the last leaves). `cancel`, `abort` and `wake` notify through `waiter_clock_` when set, else `cv_.notify_all()`. One token is waited on through one clock at a time.

Changes in `adapters.cpp`: delete the `bridge` thread and its `sleep_for(10ms)` loop in `SpectrometerPeakCenter::peak_center`. Register `token.add_on_cancel([&] { job_token.cancel(); if (runner_) if (auto id = runner_->current()) (void)runner_->cancel(*id); })` before `runner_->run(...)` and `remove_on_cancel` after it returns. The old loop repeated the cancel because the job might not be registered with the runner yet: `job_token.cancel()` covers that case, since the job checks its own token when it starts.

- [ ] **Step 1: Write the failing tests:**
  - `CancelTokenVirtual.CancelWakesAClockWaiter`: participant thread in `token.wait_until(clock, kStart + 1h)`; a non-participant calls `cancel()`; the waiter returns `Cancelled` with `now() == kStart`.
  - `CancelTokenVirtual.ElapsesAtTheDeadline`: returns `Elapsed` at `kStart + 30s`.
  - `CancelTokenVirtual.WakeEndsTheWaitEarly`: `wake()` gives `Woken`, time unchanged.
  - `CancelToken.OnCancelRunsOncePerRequestAndNotAfterRemoval`: callback count 1 after `cancel()`; after `remove_on_cancel` and `reset()` + `cancel()`, still 1.
  - `CancelToken.OnCancelRunsAtOnceWhenAlreadyRequested`.
  - `SpectrometerPeakCenter.CancellingTheRunCancelsTheJob`: with the file's existing fixture, cancel the run token while the job runs; `peak_center` returns `ErrorKind::Cancelled`.
  - `SpectrometerPeakCenter.RunsWithoutAHelperThread`: on a `VirtualClock` with `stall_report_after = 200ms`, a peak center completes and `on_stall` is never called (the old bridge thread slept in real time beside it).
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'CancelToken|SpectrometerPeakCenter'`. Expected: build fails, `add_on_cancel` is not a member.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same `ctest` plus `-R 'scripting|measurement'`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(scripting): a cancel reaches clock waiters and registered callbacks`.

---

### Task 7: Executor, run, session and notifier

**Files:**
- Modify: `libs/experiment/src/executor/executor.cpp`, `libs/experiment/src/run/run.cpp`, `libs/experiment/src/lab/session.cpp`, `libs/experiment/src/lab/notifier.cpp`, `libs/experiment/src/measurement/engine.cpp`
- Test: `tests/experiment/test_executor.cpp` (existing executor test file)

**Interfaces:**
- Consumes: Tasks 1 to 6.
- Produces: no public API change. Participants: `executor.run`, `executor.slot.<row>`, `run.post_eq`, `lab.session`, `lab.notifier`.

Changes:
- `Executor::Resource`: `acquire(const Clock& clock, scripting::CancelToken& token)`. While held, the waiter registers `token.add_on_cancel` that locks `mutex_` and `clock.notify_all(cv_)`, then `clock.wait(cv_, lock)`; `release(const Clock& clock)` notifies through the clock. No 10 ms poll.
- `Slot::done` and `Slot::overlap_ready` stop being atomics set outside the lock: they are set under `Executor::mutex_` and followed by `clock_.notify_all(cv_)`; so is `end_`. The local `wait_until(pred)` lambda becomes `while (!pred()) clock_.wait(cv_, lock);`. Find every writer with `grep -n 'done\b\|overlap_ready\|end_ =' libs/experiment/src/executor/executor.cpp libs/experiment/src/run/*.cpp`.
- Thread bodies start with a `Clock::Participant`: the slot lambda (`executor.cpp:263`), `post_eq` (`run.cpp:373`), the session thread (`session.cpp:152`), the notifier (`notifier.cpp:7`). `Executor::run` itself constructs `Clock::Participant p(clock_, "executor.run")`.
- Joins of those threads happen after the thread's done flag was awaited through the clock (slots: `wait_until([&]{ return s->done; })` already precedes the join; `post_eq`: add a done flag under the run's mutex, notified through the clock, awaited before `join()`).
- `run.cpp:35`: `std::chrono::system_clock::now()` becomes `clock.wall_now()`, using the run's context clock.
- `MeasurementEngine` (`engine.cpp:605`) already waits on `ctx_.clock`: no change beyond confirming its token is the one whose cancel notifies through the clock.

- [ ] **Step 1: Write the failing tests** (suite `ExecutorVirtual`, built on the file's existing fake services, with `VirtualClock` in `services.clock` and the test thread as a participant unless stated):
  - `QueueOfThreeRunsTakesNoRealTime`: three runs whose fake extraction and measurement each wait 300 s on the clock, 60 s `delay_between`; `run()` returns `Completed`, `now() - kStart >= 3 * 600s`, real time under 2 s.
  - `RunTimestampsFollowTheClock`: the three results' start wall times differ by at least 600 s and the first equals `options.epoch + (first start - kStart)` to the second.
  - `OverlappedRunWaitsForTheResource`: the file's existing overlap scenario; the second run's extraction starts at exactly the time the first releases the resource.
  - `CancelFromOutsideEndsAPacedQueue`: `speed = 1`; `run()` on a helper participant thread; the test thread (not a participant) calls `cancel()` after 50 ms real; `run()` returns `Cancelled` within 500 ms real. (Review Focus 1)
  - `AbortWhileWaitingForAResource`: run B waits for a resource run A holds; abort; `run()` returns `Aborted` and no stall is reported (`stall_report_after = 200ms`).
  - `ScheduledStartWaitsOnTheClock`: `options.start_at = kStart + 8h`; the first run starts at that time; real time under 1 s.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R ExecutorVirtual`. Expected: FAIL: the first test stalls or runs for seconds of real time (10 ms polls), reported by `on_stall` naming `executor.run`.
- [ ] **Step 3: Implement** the changes above.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'experiment|Executor|Run\.|LabSession'`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(experiment): the executor and its runs wait on the clock`.

---

### Task 8: Spectrometer and line waits; real-time fallbacks removed

**Files:**
- Modify: `libs/systems/src/spectrometer/acquisition.cpp`, `libs/systems/src/spectrometer/move_protocol.cpp`, `libs/systems/src/switch_manager.cpp`, `libs/sim/src/spectrometer/sim_drivers.cpp`, `libs/devices/src/spectrometer/thermo_qtegra.cpp`, `libs/devices/src/spectrometer/legacy/polled_acquirer.cpp`, `libs/devices/src/spectrometer/ngx.cpp`, `libs/laser/src/laser_system.cpp`
- Test: `tests/sim/test_sim_spectrometer_drivers.cpp`, `tests/systems/test_acquisition.cpp` (existing acquisition test file), `tests/systems/test_switch_manager.cpp`

**Interfaces:**
- Produces: no API change.

Changes:
- Every `notify_*` on a condition variable that some code passes to `clock_.wait*` goes through the clock: `acquisition.cpp` (8), `thermo_qtegra.cpp` (2), `polled_acquirer.cpp` (1), `sim_drivers.cpp` (1), and `ngx.cpp`'s `acq_cv_`.
- Delete the `real_deadline` variable and its comparisons in `FramePacer::wait` (`sim_drivers.cpp:96,104`), `ngx.cpp:360,375`, `thermo_qtegra.cpp:362,364`, `polled_acquirer.cpp:63,65`. In `ngx.cpp` the two `acq_cv_.wait_for` calls (`:319`, `:376`) become `clock_.wait_until(acq_cv_, lock, clock_.now() + <same duration>)`.
- `acquisition.cpp:604`: the `clock_.now() + 1h` stand-in for "no deadline" becomes `clock_.wait(collect_cv_, lock)`.
- `switch_manager.cpp:336` and `move_protocol.cpp:35` sleep on a local cv nobody notifies: replace each with `clock.sleep_for(...)`.
- Any thread these files start that waits on the clock gets a `Clock::Participant` (check `acquisition.cpp` for its poll threads: `grep -n 'std::thread' libs/systems/src/spectrometer/*.cpp`).
- Wall time: `switch_manager.cpp:203` and `laser_system.cpp:243,443` use `clock.wall_now()`.

- [ ] **Step 1: Write the failing tests:**
  - `FramePacerVirtual.FramesArriveOnThePeriodWithNoRealDelay`: period 1 s, 600 frames on a `VirtualClock`; frame k is stamped `kStart + k * 1s`; real time under 1 s.
  - `FramePacerVirtual.TimeoutIsClockTimeOnly`: `speed = 0.5`, a stopped pacer, `wait(timeout = 100ms)`: returns `nullopt` after about 200 ms real, at `kStart + 100ms` (with the real-time fallback it returns at 100 ms real with the clock at `kStart + 50ms`).
  - `IntensityStreamVirtual.CollectForADurationEndsAtTheDeadline`: with the sim acquirer, `collect(duration = 300s)` returns 300 readings ± 1 at `kStart + 300s`.
  - `SwitchManagerVirtual.ActuationDelayIsClockTime`: the file's existing settle-time scenario, real time under 100 ms.
  - `SwitchManager.LockTimeIsTheClocksWallTime`: on a `ManualClock` with a fixed epoch, the recorded lock time equals `epoch + elapsed` floored to the second.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'FramePacerVirtual|IntensityStreamVirtual|SwitchManagerVirtual|SwitchManager.LockTime'`. Expected: FAIL on the real-time bounds and on the lock time.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'sim|systems|devices|laser'`. Tests that passed only because of a real-time fallback on an unadvanced `ManualClock` now hang: move each to `VirtualClock` with the test thread as a participant. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(spectrometer): acquisition waits on the clock alone`.

---

### Task 9: `NgxLink` on the clock

**Files:**
- Modify: `libs/devices/include/pychron/devices/spectrometer/ngx_link.hpp`, `libs/devices/src/spectrometer/ngx_link.cpp`
- Test: the existing NGX link test under `tests/devices/spectrometer/` (`grep -rl NgxLink tests/devices`), `tests/systems/test_ngx_system.cpp`, `tests/systems/test_qtegra_system.cpp`

`NgxLink` already holds `clock_` but times its protocol on `std::chrono::steady_clock` (`steady_now()`, `owed_until_`, five `cv_.wait_for/wait_until`). Its reader reads a transport that, simulated, now waits in clock time (Task 5), so the link must too. On hardware `SteadyClock` makes the two identical.

Changes: delete `steady_now()` and `SteadyTime`; every use becomes `clock_.now()`; `owed_until_` becomes `std::optional<TimePoint>`. Each `cv_.wait_for(lock, d, pred)` becomes a loop `while (!pred() && clock_.now() < until) clock_.wait_until(cv_, lock, until)`. Every `cv_.notify_*` goes through `clock_`. `reader()` starts with `Clock::Participant p(clock_, "ngx.reader")`; the join in the destructor is wrapped in `Clock::Detached`.

- [ ] **Step 1: Write the failing tests:**
  - `NgxLinkVirtual.CommandTimeoutIsClockTime`: a sim peer that never answers; `command()` fails with `Timeout` at `kStart + options.command_timeout`; real time under 200 ms.
  - `NgxLinkVirtual.ReconnectBackoffIsClockTime`: peer down for the first two opens; the link is up after the configured backoff sum in clock time; real time under 200 ms.
  - `NgxSystemVirtual.AcquiresSixHundredFramesWithNoPump`: the existing `test_ngx_system.cpp` acquisition scenario with its local `Pump` class replaced by a `VirtualClock` and a started scheduler; 600 one-second integrations; real time under 2 s.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'NgxLinkVirtual|NgxSystemVirtual'`. Expected: FAIL on the real-time bounds (the first takes `command_timeout` of real time).
- [ ] **Step 3: Implement.** Delete the local `Pump` class from `test_ngx_system.cpp` and from `test_qtegra_system.cpp`, moving their tests to `VirtualClock`.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'Ngx|Qtegra'`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(devices): the NGX link keeps time on its clock`.

---

### Task 10: Apps

**Files:**
- Modify: `apps/elctl/src/exp.cpp`, `apps/pychron-ui/src/main.cpp`, `apps/pychron-ui/src/command_line.cpp`, `apps/pychron-ui/src/command_line.hpp`
- Test: `apps/elctl/tests/test_exp_commands.cpp`, `tests/ui/test_command_line.cpp` (existing)

**Interfaces:**
- `--sim-speed <x>`: a positive number, or `max` for unlimited. `elctl exp run` and `pychron-ui` build `VirtualClock{.speed = x, .epoch = std::chrono::system_clock::now()}`. `--sim` without `--sim-speed` keeps `SteadyClock`.

Changes:
- `exp.cpp:140-170`: replace `ManualClock` + `ClockPump` + `PumpGuard` with the `VirtualClock`. Remove the lines that set `options.scheduler.threads = 0` and `options.run_scheduler = false` for the simulated case and whatever calls `pump->drive(...)`. The command's thread constructs `Clock::Participant p(clock, "elctl")` for the duration of the run.
- `main.cpp:327-337`: the same for the UI; the UI thread is **not** a participant.
- `command_line.cpp:57-71` and `exp.cpp:347-351`: accept `max`; `--sim-speed max` is refused by `pychron-ui` with `"--sim-speed max is for tests; give a number"` (a UI at unlimited speed finishes a queue before it paints).

- [ ] **Step 1: Write the failing tests:**
  - In `test_exp_commands.cpp`: change every `"--sim-speed", "400"` to `"--sim-speed", "max"`; add `ExpRun.SimulatedQueueTakesNoRealTime`: the existing full-queue case finishes in under 5 s real; add `ExpRun.SimulatedAnalysesAreStampedInSimulatedTime`: in the output of that queue, each analysis's timestamp is later than the one before by at least the plan's measurement duration.
  - In `test_command_line.cpp`: `--sim --sim-speed max` is an error with the message above; `--sim --sim-speed 50` gives `sim_speed == 50`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'exp_commands|ExpRun'` and `ctest --test-dir build/dev-ui -R CommandLine`. Expected: FAIL: `max` is rejected by `std::stod`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** both again. Expected: PASS. Then by hand: `build/dev/apps/elctl/elctl --sim exp run configs/examples/experiment.toml --sim-speed 50 --data <scratch dir>` completes and prints run times a plausible distance apart.
- [ ] **Step 5: Commit** `feat(elctl): --sim-speed runs on the virtual clock, and max is unlimited`.

---

### Task 11: Tests off the pumps; `ClockPump` deleted

**Files:**
- Delete: `libs/core/include/pychron/core/clock_pump.hpp`, `libs/core/src/clock_pump.cpp`, `tests/core/test_clock_pump.cpp`, `tests/integration/sim_pump.hpp`
- Create: `tests/support/virtual_time.hpp`
- Modify: `libs/core/CMakeLists.txt`, `tests/core/CMakeLists.txt`, `tests/integration/test_measurement_sim.cpp`, `tests/integration/test_spectrometer_sim.cpp`, `tests/integration/test_lab_session.cpp`, `tests/integration/test_example_line_sim.cpp`, `tests/integration/test_nmgrl_line_sim.cpp`, `tests/ui/experiment_fixture.hpp`, `tests/ui/laser_fixture.hpp`, `tests/ui/CMakeLists.txt`

**Interfaces:**
- Produces, in `tests/support/virtual_time.hpp`, namespace `pychron::testing`:
  ```cpp
  // For a thread that is not a participant (a Qt test's main thread): polls
  // `done` in real time for up to `limit`. Participants use clock.sleep_for.
  template <class F> bool eventually_real(F done, std::chrono::milliseconds limit = std::chrono::seconds(10));
  ```

Rules for the move:
- Integration tests (no Qt): `ManualClock` + `Pump`/`ClockPump` become `VirtualClock clock;` (unlimited), the fixture's scheduler is started normally with its default threads, the test thread holds a `Clock::Participant`, and every `clock.advance(d)` that existed to let the system work becomes `clock.sleep_for(d)`. Every `pace(...)` and `std::this_thread::sleep_for(...)` is deleted.
- UI fixtures: `VirtualClock clock{{.speed = 400}}` (the speed the pump had); the Qt thread is not a participant; waits in tests use the fixture's existing Qt wait helpers or `eventually_real`. Update the comment at `tests/ui/CMakeLists.txt:25`.
- Unit tests that step a `ManualClock` by hand and drive `run_pending()` themselves stay as they are.

- [ ] **Step 1: Record the baseline.** `time ctest --test-dir build/dev -R 'integration|exp_commands'` and `time ctest --test-dir build/dev-ui -R 'ui'`. Save both wall times for the merge note.
- [ ] **Step 2: Move** the integration tests, one file per commit-sized change, running `ctest --test-dir build/dev -R <that file's suites>` after each. Expected after each: PASS.
- [ ] **Step 3: Move** the UI fixtures; `ctest --test-dir build/dev-ui`. Expected: PASS.
- [ ] **Step 4: Delete** the four files and their CMake entries. `grep -rn 'ClockPump\|sim_pump\|testing::Pump\|testing::pace' apps libs tests` prints nothing. Build both presets.
- [ ] **Step 5: Check the acceptance greps.** `grep -n 'sleep_for\|sleep_until' tests/integration/*.cpp apps/elctl/tests/test_exp_commands.cpp | grep -v 'clock\.\|clock_\.'` prints nothing.
- [ ] **Step 6: Record the result.** Repeat the two `time ctest` commands; put before and after in the commit body.
- [ ] **Step 7: Commit** `test: simulated tests run on the virtual clock; the clock pump is gone`.

---

### Task 12: Close the rule, document, verify

**Files:**
- Modify: `AGENTS.md`, `docs/dev_setup.md` (the `--sim-speed` paragraph, if it has one: `grep -n sim-speed docs README.md`), any file the grep below turns up

- [ ] **Step 1: Grep for raw notifies on clock-waited condition variables.** For each file changed in Tasks 4 to 9, list the condition variables passed to `clock*.wait`, `wait_until` (`grep -n 'wait\(_until\)\?(' <file>`), then `grep -n '<name>\.notify' <file>`. Expected: no hit. Fix any, with a test that fails without the fix.
- [ ] **Step 2: Grep for real time left on simulated paths.** `grep -rnE 'sleep_for|steady_clock::now|system_clock::now|\.wait_for\(' libs --include='*.cpp' --include='*.hpp'`. Every hit must be in the spec's 4.4 / 4.5 lists (script `max_wall_time`, camera live timeouts, log hub, vision live feed, `process.cpp`, `persistence/ids.cpp`, `processing/report.cpp`, `time_series.cpp`, `asio_stream.hpp`) or inside `clock.cpp` / `virtual_clock.cpp`. Anything else: move it onto the clock with a test, or add it to the spec's list with the reason.
- [ ] **Step 3: Add to `AGENTS.md`**, after "Lifetime rules":

  ```markdown
  ## Time

  Time is injected (`pychron::Clock`). Simulated runs use `VirtualClock`, which
  jumps to the next deadline when every participant thread is waiting
  (`docs/superpowers/specs/2026-10-06-virtual-clock-design.md`).

  - A thread on a simulated path starts with a `Clock::Participant`.
  - It waits for time or for another such thread only through the clock
    (`wait`, `wait_until`, `sleep_for`), and a condition variable waited on
    that way is notified through the clock, with the state changed under the
    waiter's mutex. A raw wait stalls simulated time; a raw notify is not heard.
  - Timestamps come from `clock.wall_now()`, not `system_clock::now()`.
  - A test that needs time to pass holds a `Clock::Participant` and calls
    `clock.sleep_for(d)`. No real sleeps.
  ```
- [ ] **Step 4: Run everything.** `cmake --preset dev -DPYCHRON_SANITIZE=address,undefined && cmake --build build/dev -j && ctest --test-dir build/dev --output-on-failure`; the same for `dev-ui`; `ctest --test-dir build/tsan -R 'VirtualClock|SchedulerVirtual|QueuedTransportVirtual|CancelTokenVirtual'` after rebuilding `build/tsan`. Expected: all PASS, no sanitizer report.
- [ ] **Step 5: Run the simulated suites twenty times.** `ctest --test-dir build/dev -R 'Virtual|integration|exp_commands' --repeat until-fail:20`. Expected: PASS every time. A single failure is a real race: find it.
- [ ] **Step 6: Commit** `docs: how code keeps time, for agents and developers`.
- [ ] **Step 7: Land, when the owner says so.** Rebase on `origin/develop`, rerun Step 4, merge into `develop`, push. No pull request.
