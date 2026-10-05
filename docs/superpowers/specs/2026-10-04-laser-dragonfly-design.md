# Laser system design, part 2c-2: dragonfly

Date: 2026-10-04
Status: Design, approved in conversation; not implemented
Owner: Jake Ross
Scope: sub-project 2 of the laser program, part 2c-2.
Builds on: 2026-10-03-vision-design.md §8 (the `Dragonfly` controller) and §9
(caller contract), 2026-10-04-laser-patterns-design.md (2b: `PatternRunner`),
2026-10-04-laser-autocenter-design.md (2c-1: cameras, `usable_for_autocenter`).

## 1. Goal

While the laser heats a grain, the stage follows the glow so the beam stays on
the sample as it moves and melts. A run names a dragonfly pattern; its
script's `execute_pattern()` runs it. The vision library already has the
controller (frames and stage position in; hold, move or done out); nothing
runs it.

## 2. Decisions

- **[decision]** Dragonfly is a pattern kind, in the same files and run by the
  same poll-driven `PatternRunner`.
- **[decision]** Its duration is the pattern's, as in legacy.
- **[decision]** It needs a camera that follows the stage (the gate of 2c-1).
  A queue naming one on a device without is refused before it starts.
- **[decision]** A camera that dies part way: the stage returns to where the
  pattern started; then the camera's `on_failure` decides: `continue` holds
  there until the duration ends (a plain timed heat), `fail` is an error.
- **[decision]** The simulated camera shows a glow at the sample while the
  simulated laser fires.

## 3. Pattern file

```toml
kind = "dragonfly"
duration = 30                 # seconds; required; above 0, at most 3600
velocity = 1.0                # mm/s for its moves (the common key)
perimeter_radius = 2.5        # mm; above 0
saturation_threshold = 0.75   # above 0, at most 1
aggressiveness = 1.0          # 0 to 10
move_threshold = 0.033        # mm; 0 or more
max_step = 0.5                # mm; above 0
spiral = "hexagon"            # hexagon | square
spiral_base = 0.5             # mm; above 0
target_radius = 0.5           # mm; above 0: sizes the crop and the mask
```

`iterations` is not a key of a dragonfly. `Pattern` gains these fields and
`PatternKind::Dragonfly`; `pattern_points` gives no points for it and
`pattern_point_count` 0; `pattern_path` is a Config error (it has no path).
`Pattern::follows_glow()` says whether a pattern is one.

## 4. Runner

`PatternRunner` is given the means to see (`PatternVision`: a frame source,
the camera's config, a clock), by `LaserSystem` when it has a camera.

`execute_pattern(name)` for a dragonfly: Config error without vision. The
centre is the stage position (it must have stopped, as for any pattern). The
controller is started at the clock's now.

`running()`, one step per poll:

1. stage moving → true.
2. within `settle` of the stage coming to rest → true.
3. grab `frames_per_step` frames (a failure → camera lost, below), read the
   stage position, `Dragonfly::step`.
   - `Move`: one absolute move to centre + target at the pattern's velocity → true.
   - `Hold` → true.
   - `Done`: move back to the centre; when it arrives → false.
   - an error from the controller that is a stale batch: waited out for five
     polls as in 2c-1, then camera lost.
4. Camera lost: move back to the centre. With `on_failure = "continue"` the
   pattern then holds there until the duration has passed and ends normally;
   `last_note()` says so. With `"fail"`, the poll that sees the stage back
   returns a Config error naming the pattern and the cause.

`stop_pattern()` stops the stage and ends it. A move the stage refuses ends
it with the error. The perimeter is the controller's (targets are projected
onto it); the runner additionally refuses any target further than
`perimeter_radius` + 1 µm from the centre as a defect.

`IPatternRunner` gains `virtual std::string last_note()` (empty by default):
what the last finished pattern has to say for the run's log; the script host
logs it after a blocking `execute_pattern`, as it does a hole move's.

## 5. Queue check

A run that names a dragonfly pattern on a device whose camera table is absent,
recorded, or did not load: an error on that row ("pattern <p> follows the glow
and <device> has no camera to see it").

## 6. Simulated glow

`TraySight` gains `bool firing` and `double output_percent`; `LaserSystem::sight()`
fills them from the driver. `[<device>.sim]` gains

```toml
grain_offset_mm = [0.2, 0.1]     # the sample, from its hole's centre
glow_drift_mm_per_s = [0.01, 0]  # how it creeps while heated
glow_sigma_mm = 0.3
```

While `firing`, `SimTrayCamera` renders `vision::GlowScene` at the nearest
hole's real position + grain offset + drift × (time since firing began);
peak = `output_percent / 25` capped at 1.2 (so 20 % is bright and unsaturated
is possible at low output). Not firing: the holes, as before.

## 7. Legacy export

`DragonFlyPeakPattern` exports: `duration`, `base`→`spiral_base`,
`perimeter_radius`, `saturation_threshold`, `velocity`. `limit`,
`pre_seek_delay` and the mask kind are reported and dropped. Legacy's default
duration (0.1 s) is exported as written. `SeekPattern` stays unexported.

## 8. Vision clean-ups

- `vision::aim_crop(frame, side, aim_offset)`: the shared "crop about the aim
  point, or say it is clipped" both controllers use.
- Both controllers take their scale and map from one place in the laser
  system (`CameraConfig`); nothing in `libs/laser` passes a second scale.

## 9. Testing

Files (keys, ranges, `iterations` refused); runner on `ChromiumSim` with the
simulated glow: follows an offset grain to within the deadband; follows a
drifting grain; holds when saturated; searches when the glow is hidden and
re-acquires when it returns; never further than the perimeter; ends on time,
back at the centre; stop part way; camera death under both policies; one
stage command per poll; no vision → refused. Queue check. Session: a laser
queue whose run names a dragonfly ends with the beam having followed the
grain and the laser off. Export of pickles, and by hand of the real co2 files.

## 10. Not in this part

Seek; power control from brightness; autofocus (2c-3); a live camera
(sub-project 4); the laser window (sub-project 3).
