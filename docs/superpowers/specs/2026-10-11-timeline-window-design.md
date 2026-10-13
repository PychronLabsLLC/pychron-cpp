# Timeline window: Waits and Lanes on a long queue

Status: implemented 2026-10-11 (owner's decisions in section 8; what the
code settled, section 9). Task T043
(`pychron usage notes.md`), from T006.

## 1. Problem

`TimelineView` (`apps/pychron-ui/src/timeline.cpp`) maps whole queue, first
event to now, onto widget width. No window, zoom, scroll. Measured 2026-10-11,
synthetic queue, 25 min per run, view 1200 px wide:

| runs | segments | widest segment | labelled segments | paint |
|---|---|---|---|---|
| 30 | 180 | 25 px | 30 (measuring only) | 4.5 ms |
| 100 | 600 | 7.6 px | 0 | 1.3 ms |
| 1000 | 6000 | 2 px (the floor) | 0 | 12.6 ms |

- Label drawn only when segment wider than 24 px: gone after about 30 runs.
- Segment never drawn narrower than 2 px: past about 500 runs neighbours
  overlap, lane is solid band, tooltip hits wrong segment.
- Current run, the thing operator looks for, is sliver at right edge.
- Cost is not the problem: 12.6 ms per paint every 250 ms, model fed 1000
  runs in 213 ms total. `close_run` and `on_run_started` scan all segments
  per event (quadratic over a queue), small at this size.

Real queue: 1000 runs at 25 min is 17 days. View must stay readable for all
of it.

## 2. Goal

Operator sees current run and last few runs, labelled, at any queue length;
can look back at any earlier part; gets back to now with one action.

## 3. Design

View draws time window `[t0, t1]` (seconds since first event), not `[0, now]`.

### 3.1 Modes

- **Follow** (default, and on queue start): `t1 = now`. Window moves as time
  does. `t0` by `follow_runs`:
  - 0, the default since 2026-10-12 (T053, section 10): the active run.
    `t0` = start of the earliest run still in progress (overlapped runs are
    all in progress: both show); when none is (delay between runs, queue
    ended), start of the run that started last; before any run, 0.
  - N > 0: start of the Nth most recent run started; queue's leading wait
    included when fewer than N runs have started.
- **Free**: operator moved or zoomed window. Window stays where put; time
  marker ("now" dashed line) drawn only when inside window.
- **Whole queue**: `[0, now]`, as today. Kept as a choice, not the default.

### 3.2 Controls (on the view)

- Ctrl+wheel (Cmd on macOS, Qt's `ControlModifier`): zoom about cursor's
  time, factor 1.25 per notch. Enters Free. Wheel without the modifier is
  not taken by the view: it scrolls the pane as before.
- Drag with left button: pan. Enters Free.
- Double-click: back to Follow.
- Context menu: Follow the active run, Follow last 3 / 6 / 12 runs, Whole
  queue.
- While not in Follow, a small "Follow" button shows at view's right edge;
  click returns to Follow.
- Zoom limits: span never under 10 s, never over `[0, now]`. Pan clamped to
  `[0, now]`.
- `follow_runs` kept in window's QSettings with rest of experiment window
  state. Mode itself not kept: every queue start is Follow.

### 3.3 Drawing

- `segment_rect` maps through window. Segment partly outside window is
  clipped to plot area; one wholly outside is not drawn.
- 2 px floor and 24 px label rule unchanged.
- Axis ticks: same step table, over window span; labels still elapsed time
  since first event (`h:mm:ss`).
- Only segments intersecting window are visited (3.4): paint cost follows
  what is shown, not queue length.

### 3.4 Model

- Segments stay in one vector, appended in event order: starts are
  non-decreasing. All kept for the queue's life (6000 at 1000 runs; under
  2 MB). No pruning.
- New: `visible(t0, t1)` returns index range to visit: binary search for
  first segment with `start >= t0 - longest_`, where `longest_` is longest
  closed segment's duration, plus every still-open segment (tracked by
  index).
- Open segments indexed per run id, so `close_run` and the queue-wait close
  in `on_run_started` touch open segments only. Scan of all segments goes.
- Model API otherwise unchanged; `ExecutorPane` wiring unchanged.

## 4. Rules

- View computes nothing about runs: all times from model, model's from event
  `ts` on line's clock (simulated time under `--sim-speed`). No
  `steady_clock`, no `QDateTime::currentDateTime()` (AGENTS.md, Time).
- Follow is the state after `clear()`. A test that drives a queue and reads
  the view without touching it sees Follow.
- Whole-queue mode draws exactly what today's view draws: existing
  `tests/ui/test_timeline.cpp` geometry cases run in that mode unchanged.
- Number of segments visited by a paint in Follow is bounded by what the
  window holds, whatever the queue's length.

## 5. Files

- `apps/pychron-ui/src/timeline.hpp`, `timeline.cpp`: window, modes, input,
  `visible()`, open-segment index.
- `apps/pychron-ui/src/executor_pane.cpp`: Follow on queue start; settings.
- `tests/ui/test_timeline.cpp`: cases below.
- `docs/user/04-experiments.md`: timeline section.

## 6. Tests

- Follow shows last N runs: after 1000 runs fed, first drawn segment belongs
  to run 995 or later (N = 6) and measuring segment of current run is wider
  than 24 px.
- Visited segments in Follow after 1000 runs: under 100.
- Ctrl+wheel zoom keeps time under cursor fixed; span clamps at 10 s and at
  `[0, now]`. Wheel without Ctrl leaves window and mode as they were and
  the event unaccepted.
- Drag pans, clamps at 0 and now; marks Free; time passing does not move a
  Free window.
- Double-click and Follow button return to Follow.
- Whole queue equals today's geometry (existing cases).
- `segment_at` and tooltip hit the right segment in a window that starts
  mid-segment.
- Open wait with `planned_end` beyond window clips, not drawn past plot area.
- Overlapped runs: lanes as today inside window.

## 7. Out of scope

- Pruning or persisting segments; timeline of a finished or resumed queue
  after restart.
- Wall-clock axis labels.
- Summary strip of whole queue above the window (decided against, 8).
- Events list (T011, T044), queue table.
- `elctl`.

## 8. Decisions (owner, 2026-10-11)

1. Follow window is by run count: last 6 runs. Not by time.
2. Zoom needs Ctrl held; plain wheel scrolls the pane.
3. No whole-queue strip above the window.

## 9. As built

- View's accessors are `time_window()` and `set_time_window(t0, t1)`:
  `window()` is `QWidget`'s.
- Follow window is never narrower than 10 s either: `[start, max(start + 10,
  now)]`.
- A press that moves under 3 px is a click, not a pan: view stays in Follow.
- `visible()` assumes starts never go backwards. Model notes the first one
  that does and from then on answers by looking at every segment: slower,
  still right.
- Follow button shows in Whole queue too (any mode but Follow).
- Settings key: `experiment_window/timeline_follow_runs`.
- Measured after, same synthetic 1000-run queue, 1200 px: Follow draws 36
  segments, 18 labelled, widest 127 px, 1.7 ms a paint; Whole queue 6000
  segments, 11.9 ms. Feeding the model 1000 runs: 11 ms (was 213 ms).

## 10. Changed 2026-10-12 (T053)

Owner: "the waits and lane widget should default to following just the
active run". Replaces decision 8.1's default (last 6 runs stays a choice).

- `follow_runs` 0 (`TimelineView::kFollowActive`) means the active run; it is
  the default and the first entry of the context menu.
- `TimelineModel::active_since()`: start of the earliest run between its
  `RunStarted` and its final state; nullopt when none.
- A stored `experiment_window/timeline_follow_runs` of 3, 6 or 12 keeps its
  meaning: only an install that never chose sees the new default.
