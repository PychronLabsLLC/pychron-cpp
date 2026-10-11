# Timeline window: Waits and Lanes on a long queue

Status: DRAFT 2026-10-11. Not agreed. Open questions in section 8. Task T043
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

- **Follow** (default, and on queue start): `t1 = now`, `t0` = start of the
  Nth most recent run started, N = `follow_runs` (default 6); queue's leading
  wait included when fewer than N runs have started. Window moves as time
  does.
- **Free**: operator moved or zoomed window. Window stays where put; time
  marker ("now" dashed line) drawn only when inside window.
- **Whole queue**: `[0, now]`, as today. Kept as a choice, not the default.

### 3.2 Controls (on the view)

- Wheel: zoom about cursor's time, factor 1.25 per notch. Enters Free.
- Drag with left button: pan. Enters Free.
- Double-click: back to Follow.
- Context menu: Follow last 3 / 6 / 12 runs, Whole queue.
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
- Wheel zoom keeps time under cursor fixed; span clamps at 10 s and at
  `[0, now]`.
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
- Summary strip of whole queue above the window (see 8).
- Events list (T011, T044), queue table.
- `elctl`.

## 8. Open questions (owner)

1. Follow window by run count (proposed, N = 6) or by time (last 2 h)? Run
   count keeps labels readable whatever a run's length; time is steadier
   when runs differ much in length.
2. Wheel to zoom: acceptable inside a scrolling pane, or zoom only with a
   modifier held (Ctrl+wheel) so the wheel still scrolls the pane?
3. Want a thin whole-queue strip above the window (one mark per run, state
   colour, window's position shown)? Proposed: not in this spec; add later
   if looking back proves hard.
