# Follow (simulated) implementation plan

Part B of `docs/superpowers/specs/2026-10-30-dragonfly-grain-sim-design.md`
(section 8). Part A (grain model, `SimTrayCamera` with `[<device>.sim.grain]`,
`elctl laser track`) is on `develop`.

Goal: under `pychron-ui --sim` the developer can run a dragonfly by hand from
the laser window and watch it follow a grain, with a cross at the grain's true
center beside what the finder sees. Tracker behaviour does not change.
`elctl laser pattern` stays as it is.

## Decisions the spec leaves to the plan

- **Who may run it.** `LaserBridge::offers_simulated_follow()`: the driver is
  simulated, the camera's `source` is `sim`, the camera may move the stage,
  and the device has a laser. `Lasers` already works out "simulated" while it
  attaches cameras; it keeps the answer (`Lasers::simulated(device)`). The
  button is not created otherwise. The command checks again on the worker: a
  button is not what keeps a real laser from being fired by this path.
- **The sequence.** One bridge command, `follow_simulated(pattern, percent)`,
  so that a stop, an emergency stop or the bridge going all reach it through
  the worker's existing cancel: enable, extract, fire, `execute_pattern`,
  `wait`. Whatever that returns, `end_extract()` and `disable()` are both
  tried; the first error is reported. `disable()` closes the beam as well, so
  a failed `end_extract` does not leave it open. The cancel is checked between
  steps: nothing fires after a stop.
- **The laser is left disabled**, also if it was enabled before: the spec's
  bench ends "laser off", and the bridge's destructor does the same.
- **Threads.** `observe()` and `grab()` share no mutex and both run the grain
  model. Every `grab()` in the tree is already under `LaserSystem`'s gate
  (`frame_now`, a centering's look through `moving()`, a dragonfly's look
  through `GatedRunner::running()`), so `observe()` is reached only through a
  new gated call, `LaserSystem::grain_px()`. No new mutex.
- **The cross.** The video thread asks for it only while `system_.looking()`,
  as it does for the finder: each `observe()` asks the driver for the sight
  again, and nothing else needs it. Any following dragonfly gets it on a
  camera with a grain, a queue's too. Without `[sim.grain]` there is no truth
  and no cross.
- **Pixels.** The window does not convert. `CameraStageMap::to_px` is the
  inverse of `to_mm`; `grain_px` is `aim_px + to_px(grain - beam)`, and the
  frame position lands in `laser::CameraView::grain_px`.
- **Output of zero.** Follow is disabled at output 0: there would be no glow.

## Tasks

Each task is test first, then one commit.

### 1. `CameraStageMap::to_px`

- Files: `libs/vision/include/pychron/vision/calibration.hpp`,
  `libs/vision/src/calibration.cpp`, `tests/vision/test_calibration.cpp` (or
  the suite that holds the map's tests).
- Test: round trip of `to_mm` for `from_scale` with each flip and for a sheared
  map.

### 2. `Lasers::simulated`

- Files: `libs/experiment/include/pychron/experiment/lab/lasers.hpp`,
  `libs/experiment/src/lab/lasers.cpp`, `tests/integration/test_lab_session.cpp`.
- `simulated_` holds the device names the constructor found simulated, camera
  or no camera. Test: `true` under the line's sim, `false` with a `simulated`
  function saying no, `false` for an unknown device.

### 3. `LaserSystem::grain_px`

- Files: `libs/laser/include/pychron/laser/laser_system.hpp`,
  `libs/laser/src/laser_system.cpp`, `tests/laser/test_laser_system.cpp`.
- `CameraView` gains `std::optional<StageXY> grain_px`. `grain_px(seen)` takes
  the gate, finds the `SimTrayCamera` by `dynamic_cast`, calls `observe()`.
- Tests: no grain table, no camera: empty. Grain under the aim: the aim point.
  Grain off the aim in x and y on the usual camera, and with `flip_x`: the
  pixel a frame shows it at (checked against the frame's truth).

### 4. Bridge

- Files: `apps/pychron-ui/src/laser_bridge.hpp`,
  `apps/pychron-ui/src/laser_bridge.cpp`, `tests/ui/laser_fixture.hpp`,
  `tests/ui/test_laser_bridge.cpp`.
- `offers_simulated_follow()`, `follow_simulated()`, and `show()` asks
  `grain_px` while looking.
- `SimLaserLab` gains two optional arguments: a function that edits the
  scratch lab before it is loaded, and the `simulated` function for `Lasers`.
- Tests: offered on the example lab; not on a driver said to be real, nor on a
  `use = "view"` camera, and the command is refused there with no enable and
  no fire. A follow reaches `firing` at the output given and ends off and
  disabled when its duration is up. Off and disabled after `stop_pattern`,
  `stop_beam`, an emergency stop and the bridge going. Off after a stage error
  part way, an unknown pattern, a tripped interlock, and when `end_extract`
  itself fails. With a grain, views carry `grain_px` while it follows.

### 5. Camera view

- Files: `apps/pychron-ui/src/camera_view.hpp`,
  `apps/pychron-ui/src/camera_view.cpp`, `tests/ui/test_laser_views.cpp`.
- A cross in `theme().warning` at `grain_px`, clipped to the picture, gone
  with a stale picture.
- Tests: drawn where it says; absent without `grain_px`.

### 6. Window

- Files: `apps/pychron-ui/src/laser_window.hpp`,
  `apps/pychron-ui/src/laser_window.cpp`, `tests/ui/test_laser_window.cpp`.
- `pattern_follow` button, created only when offered; live for a selected
  dragonfly at an output above 0, while nothing runs. `following_` covers the
  time between a click and the snapshot that says busy, so a second click
  cannot queue a second burn.
- Tests: absent on a real driver; disabled for a pattern that is not a
  dragonfly and at output 0; a click follows at the field's output and the
  beam is off at the end; Stop leaves the beam off; an error shows in the
  status line and the beam is off; a window closed part way leaves it off.

### 7. Docs

- `docs/user/05-laser.md` (sections 4, 8.2, 8.3), `docs/dev_setup.md`,
  `docs/user/07-configuration-reference.md` (the `[<laser>.sim.grain]` row),
  headers of `laser_bridge.hpp` and `camera_view.hpp`.
- The spec: status line, section 8 (decisions above), section 9 (TSan is owed:
  `observe()` is now reached from a thread that is not the driver's).

### 8. Check

- `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py
  --build-dir build/dev-ui`, `ctest --preset dev-ui`.
- `tests/ui` laser suites under `-DPYCHRON_SANITIZE=thread`.
- Run `pychron-ui --sim --laser` with a grain table and look at it.

## Open questions

None.
