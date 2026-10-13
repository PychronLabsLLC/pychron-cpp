# Timeline Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep the Executor pane's Waits and Lanes timeline readable on a queue of any length: by default it follows the last six runs, the operator can zoom and pan to look back, and one action returns to now.

**Architecture:** `TimelineModel` keeps every segment, as now, and gains an index of its open segments and a `visible(t0, t1)` range so that neither an event nor a paint walks the whole queue. `TimelineView` draws a time window `[t0, t1]` chosen by its mode (Follow, Free, Whole queue) instead of `[0, now]`, and takes Ctrl+wheel, drag, double-click and a context menu. `ExecutorPane` puts the view in Follow when a queue starts; `ExperimentWindow` keeps the number of runs followed in its settings.

**Tech Stack:** C++20, Qt 6 Widgets (`QWidget` events, `QMenu`, `QToolButton`), QtTest.

**Spec:** `docs/superpowers/specs/2026-10-11-timeline-window-design.md`. Read it first; section numbers below refer to it.

## Global Constraints

- The view computes nothing about runs. Every time it draws comes from the model, and the model's come from event `ts` on the line's clock. No `steady_clock`, no `QDateTime::currentDateTime()`, no timer other than the pane's existing 250 ms `timeline_clock_` (AGENTS.md, "Time").
- Follow window: from the start of the 6th most recent run started (`follow_runs`, default 6; the menu offers 3, 6 and 12) to now. The queue's leading wait is included while fewer than `follow_runs` runs have started.
- Zoom: Ctrl+wheel only (`Qt::ControlModifier`, which is Cmd on macOS), factor 1.25 per 120 units of `angleDelta().y()`, about the time under the cursor. A wheel event without the modifier is `ignore()`d so the pane scrolls.
- Span is never under 10 s and never over `[0, now]`; the window is clamped inside `[0, now]`.
- Whole-queue mode draws exactly what the view draws today. The existing cases in `tests/ui/test_timeline.cpp` keep their numbers by selecting that mode.
- The 2 px minimum width and the 24 px label rule are unchanged.
- Segments are never pruned.
- Settings key: `experiment_window/timeline_follow_runs` (int). The mode is not stored.
- Qt code: a parent owns its child. `const` by default, `[[nodiscard]]` on results that must not be dropped.
- No thread is added.
- Commits: Conventional Commits with component scope (`feat(ui): ...`).
- Before each commit that changes C++: `python3 tools/quality_check.py` (exit 0), then the tests named in the task.
- Build and run:

  ```bash
  cmake --build --preset dev-ui
  ctest --preset dev-ui -R '^ui\.test_(timeline|experiment_window)$'
  ```

## File Structure

| File | Responsibility |
|---|---|
| `apps/pychron-ui/src/timeline.hpp`, `.cpp` | Model: open-segment index, `visible()`, run start times. View: window, modes, input, Follow button, context menu. |
| `apps/pychron-ui/src/executor_pane.hpp`, `.cpp` | Follow on queue start; forwards `follow_runs` to and from the view. |
| `apps/pychron-ui/src/experiment_window.cpp` | Reads and writes `experiment_window/timeline_follow_runs`. |
| `tests/ui/test_timeline.cpp` | Model and view cases. |
| `tests/ui/test_experiment_window.cpp` | The pane starts in Follow; the setting survives a new window. |
| `docs/user/04-experiments.md` | The timeline paragraph of the Executor pane. |

A helper used by several test cases, added in Task 1 and reused after:

```cpp
// n runs of 1500 s each: a 10 s wait, then preparing, extracting, a pump
// wait, measuring, post-measuring, success. Run i is "r<i>".
static void feed_long(TimelineModel& m, int n);
```

It is the loop of the benchmark recorded in spec section 1: `on_waiting` at `b`, `on_run_started` at `b + 10`, states at `b + 10`, `b + 20`, `b + 300`, `b + 1300`, success at `b + 1480`, and a `minimum pump time` wait for the run at `b + 100`, with `b = i * 1500`.

---

### Task 1: The model stops scanning the queue

**Files:** `apps/pychron-ui/src/timeline.hpp`, `apps/pychron-ui/src/timeline.cpp`, `tests/ui/test_timeline.cpp`

- [ ] **Step 1: failing tests.** Add `feed_long` and:
  - `visibleIsTheWindowsSegments`: after `feed_long(m, 1000)`, `m.visible(t0, t1)` for the last run's span returns indices whose segments all satisfy `start <= t1 && drawn_end >= t0 - longest`, includes every segment of run `r999`, and has fewer than 100 entries.
  - `visibleIncludesOpenSegments`: `feed(m)` (the existing fixture, which leaves A's and C's states open), `m.advance(at(10000))`; `m.visible(9000, 10000)` contains the open segments although they started long before `t0`.
  - `runStartsAreKept`: after `feed_long(m, 10)`, `m.run_starts()` has 10 non-decreasing entries and `run_starts()[3] == 3 * 1500 + 10`.
  - `aThousandRunsFeedQuickly`: `feed_long(m, 1000)` leaves 6000 segments, none open, and `m.open_count() == 0`.
- [ ] **Step 2:** run `ctest --preset dev-ui -R '^ui\.test_timeline$'`; the new cases fail to compile.
- [ ] **Step 3: implement.**
  - `std::vector<std::size_t> open_;` holds the indices of segments without an `end`. `close_run`, the queue-wait close in `on_run_started` and `on_queue_ended` walk `open_` only, and erase what they close.
  - `double longest_ = 0;` is raised whenever a segment is closed.
  - `std::vector<double> run_starts_;` is appended in `on_run_started`.
  - `std::vector<std::size_t> visible(double t0, double t1) const;` uses `std::ranges::lower_bound` on `start` for `t0 - longest_`, walks forward while `start <= t1`, then adds the members of `open_` not already in the range. The result is ascending.
  - Accessors `run_starts()`, `open_count()`. `clear()` resets all of it.
- [ ] **Step 4:** quality check, then `ctest --preset dev-ui -R '^ui\.test_timeline$'`: all pass, the existing cases unchanged.
- [ ] **Step 5: commit** `refactor(ui): the timeline model indexes its open segments and what a window shows`.

### Task 2: The view draws a window, and follows the last runs

**Files:** `apps/pychron-ui/src/timeline.hpp`, `apps/pychron-ui/src/timeline.cpp`, `tests/ui/test_timeline.cpp`

- [ ] **Step 1: failing tests.**
  - In the existing `viewHitsSegments`, call `v.set_mode(TimelineView::Mode::WholeQueue)` after constructing the view; its numbers stay as they are.
  - `followShowsTheLastRuns`: `feed_long(m, 1000)`, view 1200 px wide, default mode. `v.mode() == Follow`; `v.window()` starts at `m.run_starts()[994]` and ends at `m.now()`; the measuring segment of `r999` is wider than 24 px; `v.drawn_count()` after a `render` into a `QImage` is under 100.
  - `followIncludesTheLeadingWaitEarlyOn`: `feed_long(m, 3)`; the window starts at 0.
  - `aSegmentAcrossTheWindowsEdgeIsClipped`: in a window that starts inside `r995`'s measuring segment, `segment_rect` of that segment starts at the plot area's left edge and `segment_at` just inside that edge returns it; a segment wholly before the window has an empty rectangle and is never hit.
  - `aPlannedEndBeyondTheWindowIsClipped`: an open wait with `planned_end` past `t1` has a rectangle that ends at the plot area's right edge.
- [ ] **Step 2:** run the test; the new cases fail to compile.
- [ ] **Step 3: implement.**
  - `enum class Mode { Follow, Free, WholeQueue };`, `Mode mode() const`, `void set_mode(Mode)`, `int follow_runs() const`, `void set_follow_runs(int)` (clamped to 1 or more), `std::pair<double, double> window() const`, `int drawn_count() const` (segments the last paint drew).
  - `window()`: WholeQueue is `[0, max(1, now)]`; Follow is `[start, max(start + 10, now)]` with `start = run_starts()[size - follow_runs]` when there are at least `follow_runs` starts, else 0; Free is the stored `[t0_, t1_]`.
  - `segment_rect` maps through `window()` and intersects with the plot area's horizontal extent; a segment outside it returns an empty `QRectF`. The 2 px minimum applies only to a segment that intersects the window.
  - `paintEvent` and `segment_at` iterate `model_.visible(t0, t1)`. Axis ticks use the step table over the window's span, starting at the first multiple of the step at or after `t0`. The "now" line is drawn only when `now` is inside the window.
  - `span()` is removed in favour of `window()`.
- [ ] **Step 4:** quality check; `ctest --preset dev-ui -R '^ui\.test_(timeline|experiment_window)$'`.
- [ ] **Step 5: commit** `feat(ui): the timeline follows the last six runs`.

### Task 3: Zoom, pan and the way back

**Files:** `apps/pychron-ui/src/timeline.hpp`, `apps/pychron-ui/src/timeline.cpp`, `tests/ui/test_timeline.cpp`

- [ ] **Step 1: failing tests** (events built with `QWheelEvent` and `QTest::mousePress` / `mouseMove` / `mouseRelease` / `mouseDClick`; the view shown, 1200 px wide, on `feed_long(m, 1000)`):
  - `ctrlWheelZoomsAboutTheCursor`: one notch in with Ctrl at a point `p`: the mode is Free, the span is the old span divided by 1.25, and the time under `p` is unchanged to within 1e-6 of the span.
  - `aPlainWheelIsLeftToThePane`: a wheel event without Ctrl is not accepted; mode and window are unchanged.
  - `zoomClamps`: many notches in stop at a span of 10 s; many notches out stop at `[0, now]`.
  - `dragPansAndClamps`: dragging right by 100 px moves the window earlier by `100 / plot width * span`; the mode is Free; dragging far past either end stops at 0 and at `now`.
  - `aFreeWindowStaysPut`: after a pan, `m.advance(...)` by an hour leaves `window()` unchanged.
  - `doubleClickAndTheButtonFollow`: the Follow button is hidden in Follow and visible in Free; a double-click, and separately a click on the button, return the mode to Follow.
  - `theMenuSetsTheRunsFollowed`: `v.context_actions()` holds "Follow last 3 runs", "Follow last 6 runs", "Follow last 12 runs" and "Whole queue"; triggering the first sets `follow_runs() == 3` and Follow, and emits `follow_runs_changed(3)`; the last sets WholeQueue.
- [ ] **Step 2:** run the test; fails to compile.
- [ ] **Step 3: implement** `wheelEvent`, `mousePressEvent`, `mouseMoveEvent`, `mouseReleaseEvent`, `mouseDoubleClickEvent`, `contextMenuEvent`; a child `QToolButton` ("Follow") placed at the plot area's right edge in `resizeEvent`; `QList<QAction*> context_actions() const`; signal `follow_runs_changed(int)`. Entering Free copies the current `window()` into `t0_`, `t1_`. One private `void set_window(double t0, double t1)` does the clamping for zoom and pan both.
- [ ] **Step 4:** quality check; `ctest --preset dev-ui -R '^ui\.test_timeline$'`.
- [ ] **Step 5: commit** `feat(ui): the timeline zooms with Ctrl+wheel, pans by dragging and returns to now`.

### Task 4: The pane and the window

**Files:** `apps/pychron-ui/src/executor_pane.hpp`, `apps/pychron-ui/src/executor_pane.cpp`, `apps/pychron-ui/src/experiment_window.cpp`, `tests/ui/test_experiment_window.cpp`

- [ ] **Step 1: failing tests.**
  - `theTimelineFollowsWhenAQueueStarts`: put the pane's view in WholeQueue, start the example queue, and the view is in Follow while it runs and after it ends.
  - `theRunsFollowedAreRemembered`: trigger "Follow last 12 runs" on one window's view, destroy the window, make another on the same settings file: its view's `follow_runs()` is 12. With no key stored it is 6.
- [ ] **Step 2:** run `ctest --preset dev-ui -R '^ui\.test_experiment_window$'`; the new cases fail.
- [ ] **Step 3: implement.** Where the pane clears the timeline on start (`timeline_.clear()`), also `timeline_view_->set_mode(TimelineView::Mode::Follow)`. `ExperimentWindow` reads `experiment_window/timeline_follow_runs` after building the pane and writes it on `follow_runs_changed`.
- [ ] **Step 4:** quality check; the two test binaries.
- [ ] **Step 5: commit** `feat(ui): a queue's start puts the timeline back on now, and the runs followed are remembered`.

### Task 5: User guide, spec, and the whole suite

**Files:** `docs/user/04-experiments.md`, `docs/superpowers/specs/2026-10-11-timeline-window-design.md`

- [ ] **Step 1:** in the Executor pane's description (the table row at "Bottom", and the paragraph on overlapping runs that mentions the timeline), say what the timeline shows by default, Ctrl+wheel, drag, double-click, the Follow button and the context menu.
- [ ] **Step 2:** if the code departed from the spec anywhere, change the spec in this branch; set its status line to implemented.
- [ ] **Step 3:** rebase on `origin/develop`; `python3 tools/quality_check.py`; `cmake --build --preset dev-ui`; `ctest --preset dev-ui` (the whole suite).
- [ ] **Step 4:** repeat the benchmark of spec section 1 for 1000 runs in Follow and put paint time and labelled-segment count in the commit message (AGENTS.md: a change made for speed comes with a measurement; this one is made for readability, and the numbers show both).
- [ ] **Step 5: commit** `docs: the timeline's window in the user guide`, merge into `develop`, push.
