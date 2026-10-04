# Laser system design, part 2b: patterns

Date: 2026-10-04
Status: Design, approved in conversation; not implemented
Owner: Jake Ross
Scope: sub-project 2 of the laser program, second of three parts.
Builds on: 2026-10-04-laser-system-design.md (2a: `LaserSystem`, trays, calibration),
2026-10-03-legacy-laser-survey.md §6 (legacy patterns), 2026-10-04-chromium-protocol-survey.md.

## 1. Goal

A run that names a pattern has its laser beam moved over the sample along that
pattern: the extraction script's `execute_pattern()` works on a real or
simulated laser. Scripts already call it, block on `running()` and call
`stop_pattern()` on cancel; queue runs already carry `pattern`. Nothing real
implements `IPatternRunner`.

## 2. Decisions

- **[decision]** All geometric kinds: polygon, linear, circular contour, line
  spiral, square spiral, random, rubberband, raster, trough. Not arc (needs a
  controller arc move), not seek or dragonfly (vision: 2c), no z or power
  series.
- **[decision]** Legacy `.lp` pickles come across with an export tool, once per
  lab.
- **[decision]** `IStage` gains a speed on xy moves and a `stop()`; the Chromium
  driver and its simulator implement both.
- **[decision]** Patterns are run by the host, point by point, with no thread:
  the caller's `running()` polls drive it.

## 3. Pattern files

`<lab>/patterns/<name>.toml`. The name is the file's stem; a run's `pattern`
names it.

```toml
kind = "polygon"
velocity = 1.0        # mm/s, above 0; default 1
iterations = 1        # whole pattern repeated; 1 to 200; default 1
# the kind's own keys
radius = 0.5
nsides = 6
rotation = 0
```

Lengths are millimetres, angles degrees, in the stage's axes (as legacy; not
the tray's frame). An unknown key, a key of another kind, a wrong type or a
value out of range is a Config error naming the file and the key.

| kind | keys (default; range) |
|---|---|
| `polygon` | `radius` (0.5; >0), `nsides` (6; 3–200), `rotation` (0) |
| `linear` | `length` (1; >0), `rotation` (0), `npasses` (1; 1–100) |
| `circular_contour` | `radius` (0.1; >0), `nsteps` (2; 1–10), `percent_change` (0.8; >0) |
| `line_spiral` | as circular contour, and `step_scalar` (5; 1–20) |
| `square_spiral` | as circular contour |
| `random` | `walk_x` (1; >0), `walk_y` (1; >0), `npoints` (10; 1–50), `seed` (unset: a new walk each run) |
| `rubberband` | `length` (15; >0), `offset` (0; ≥0), `rotation` (0) |
| `raster` | as rubberband, and `dx` (0.5; >0), `single_pass` (true) |
| `trough` | `length` (10; >0), `width` (10; >0), `rotation` (0), `use_x` (true) |

## 4. Points

`pattern_points(const Pattern&, std::uint64_t seed) -> std::vector<StageXY>`:
offsets from the pattern's centre, one pass. A pure function per kind, with
legacy's geometry (`pychron/lasers/pattern/pattern_generators.py`), `c` being
(0, 0):

- **polygon**: for i = 0..nsides: `r (cos a, sin a)`, `a = 360 i / nsides + rotation`. Closed: the last point is the first.
- **linear**: `p1 = (0,0)`, `p2 = (length, 0)`, rotated by `rotation`; pass i goes p1→p2 when i is even, p2→p1 when odd.
- **circular_contour**: for ring i = 0..nsteps−1, radius `R (1 + i·percent_change)`: the circle from 0° to 360° in 10° steps (37 points, closed).
- **line_spiral** (outwards): for turn i = 0..nsteps−1, `n = 2 i + step_scalar` angles evenly from 0° to 360° inclusive; radius `R (1 + (i + t/360)·percent_change)`; the 360° point is dropped on every turn but the last.
- **square_spiral** (outwards): from the centre, `4 nsteps + 1` steps of length `R (1 + i·percent_change)` cycling +x, +y, −x, −y.
- **random**: `npoints` points uniform in the box ±walk_x, ±walk_y, redrawn until within `walk_x` of the centre (legacy's circle test). A fixed generator (`std::mt19937_64`) and its own uniform mapping, so a `seed` gives the same walk on every compiler.
- **rubberband**: `(-o, o), (L+o, o), (L+o, -o), (-o, -o), (-o, o)`, rotated.
- **raster**: legacy's zig-zag over the rubberband's box: `n = int((L + 2o)/dx)`; if `n·dx <= L + 2o`: `n` made even (`n+1` if odd), `dx = (L + 2o)/(n + 1)`, `n = int((L + 2o)/dx)`; points `(-o + dx·i, ∓o)` for i = 0..n (y = +o for even i); when not `single_pass`, the way back and a return to the first corner.
- **trough**: `p1 (0,0), p2 (L,0), p3 (L,-W), p4 (0,-W)`, rotated; order `p1 p2 p4 p3 p1` with `use_x`, else `p1 p2 p3 p4 p1`.

Rotation is counter-clockwise about the centre. Every generated point is
finite; a pattern of more than 10 000 points over all iterations is a Config
error.

`path_length(points)` and the estimated time (`iterations · length / velocity`)
are what `elctl` and, later, the UI show.

## 5. `PatternLibrary`

The patterns of `<lab>/patterns/*.toml` by name: `find`, `names`, `problems`
(a file that did not load). As `TrayLibrary`: never fails, skips hidden files,
and a bad file stops only the runs that name it.

## 6. Stage interface

```cpp
struct IStage {
  // speed_mm_s 0: the stage's own travel speed. A stage that cannot set a
  // speed moves at its own.
  virtual Result<void> set_xy(double x, double y, double speed_mm_s = 0) = 0;
  // Stop any motion now. NotSupported where the stage cannot.
  virtual Result<void> stop() { return fail(not_supported(...)); }
};
```

- **Chromium**: the speed goes into `Stage.MoveTo`'s x and y speed fields as
  whole microns per second, at least 1 and at most the configured
  `move_speed`; z keeps its own. `stop()` sends `Stage.Stop`, confirms with
  `Stage.Pos?`, and forgets the target (`moving()` is then false).
- **`ChromiumSim`** already moves at the commanded speeds and answers
  `Stage.Stop`.
- **`LaserSystem`** passes both through.
- Existing implementers (`SimExtractionDevice`, the fakes) take the new
  parameter and ignore it.

Follow-ons, in this part: `elctl laser goto` stops the stage on Ctrl-C and on
`--timeout`; a script cancelled while it waits in `move_to_position`, `set_xy`
or `set_x/y/z` stops the stage (the host's `wait_while` gets a stop action, as
`execute_pattern` has).

## 7. The runner

`PatternRunner : IPatternRunner`, owned by `LaserSystem`, returned by
`pattern_runner()` when the driver has a stage and no pattern runner of its
own.

- `execute_pattern(name)`: Config error for an unknown or unloadable pattern,
  or if one is running. Reads the stage position: that is the centre. Builds
  the whole path (points × iterations, then the centre) and starts the first
  move at the pattern's velocity.
- `running()`: one step. If the stage is still moving: true. Otherwise the
  next point is sent: true. After the last point has arrived: false.
- `stop_pattern()`: drops what is left and calls the stage's `stop()`; a
  stage that cannot stop is not an error. Safe when nothing runs.
- `patterns()`: the library's names.
- A move the stage refuses (outside travel) or that fails ends the pattern:
  `running()` returns that error, with "pattern <name>, point <i> of <n>"
  in front, and the next call says false. The run's ordinary ending switches
  the laser off.
- One mutex guards the state; it is not held across a stage call. Calls come
  from one thread at a time (the script host's).

Known limit, documented: arrival is the driver's (three good polls, each a
host poll of 50 ms), so a vertex costs about 150 ms. Fine for polygons and
rasters; a spiral of hundreds of points is slow.

## 8. Queue check

In a lab with extraction devices, for a run that extracts on one of them and
names a pattern: an error on that row if the lab has no such pattern, or its
file did not load (the message is the file's problem). Labs without
extraction devices check as before.

## 9. Legacy export

`tools/export_patterns.py <dir of .lp> <out dir>`: reads each pickle with a
stub unpickler (no legacy Pychron or traits needed: the pickles are a class
name and a dictionary), maps class and attributes to the keys of §3, writes
`<name>.toml`. Reported and skipped: `ArcPattern`, `SeekPattern`,
`DragonFlyPeakPattern`, `DiamondPattern` and anything unknown. Reported and
dropped: enabled z or power series, `disable_at_end`, a spiral's "in"
direction. Never overwrites without `--force`. It is run by hand and tested
with pickles made in the test and, where the lab's files are at hand, with
them.

## 10. `elctl laser`

| Command | Does |
|---|---|
| `patterns` | each pattern: kind, points, path length, estimated time; files that did not load |
| `pattern <device> <name> --dry-run` | the points, relative to the centre |
| `pattern <device> <name>` | runs it from where the stage is (the laser is not fired), Ctrl-C stops the stage; prints where it ended |

## 11. Testing

- Generators: each kind against points worked by hand from §4, legacy defaults,
  closure, rotation, the raster's `dx` adjustment, a seeded random walk
  repeated, the point limit.
- Files: every key and range; a kind's key on another kind; the library and
  its problems.
- Driver and simulator: a move at a given speed takes distance/speed of
  simulated time; the speed is clamped; `stop()` halts the simulated stage
  where it is and `moving()` is false after.
- Runner over `ChromiumSim`: the simulator's log shows the points in order at
  the pattern's speed; iterations; the return to the centre; `stop_pattern`
  mid-segment; a point outside travel ends it with the point's number; a
  second `execute_pattern` while running is refused; the pattern conformance
  suite.
- Queue check: unknown pattern, unloadable pattern, lab without devices.
- Session: the example laser queue with a pattern on one run.
- `elctl`: `patterns`, `pattern --dry-run`, `pattern` on the simulator, `goto`
  stopping the stage on timeout.
- Script host: a cancelled `move_to_position` stops the stage.
- Export tool: a `unittest` run by ctest when `python3` is found.

## 12. Not in this part

Seek and dragonfly (2c); arc; z and power series; a pattern maker (sub-project
3); patterns in the tray's frame; a check of the pattern against stage travel
before it starts.
