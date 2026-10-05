# Laser window

Sub-project 3 of the laser work. Sub-project 2 made a laser something a queue
can drive: trays, calibration, patterns, autocenter, dragonfly. All of it is
reached from queues and from `elctl laser`. This part gives the operator a
window: see the tray and the camera, drive the stage and the laser by hand,
calibrate a tray on screen, make patterns, and stop everything at once.

## 1. What the operator gets

One **laser window** per extraction device that has a stage or a laser.

- **Tray**: the current tray's holes, the stage as a crosshair, the calibration
  points and the saved hole corrections marked. Click a hole to go to it.
- **Camera**: the picture (the simulated camera for now), a crosshair at the
  aim point, the target the finder sees outlined, an Autocenter button, how the
  last centering ended.
- **Stage**: x, y, z readout, a jog pad with a step size, Stop.
- **Laser**: Enable, output percent, Fire, Stop, the tripped interlocks, a
  status line saying what the device is doing.
- **Calibration**: the tray's points as a table, add a point at a hole from
  where the stage is now, remove one, the solution (center, rotation, scale,
  rms) and its cautions, Clear. The same files as `elctl laser calibrate`.
- **Patterns**: the lab's patterns with kind, length and time; Run about where
  the stage is; Stop. A dragonfly is listed and runs only from a queue.
- **Pattern maker**: a window to make and edit a pattern: pick a kind, set its
  fields, see the path drawn with its length and time, save it to the lab.
- **Emergency stop**: one button, always live.

Reached two ways: View ▸ Laser in `pychron-ui`, and `pychron-ui --laser`, which
opens the laser window as the only window, for a laser PC.

Firing by hand is *enable, then fire*: Fire is live only when the laser is
enabled, no interlock is tripped and no emergency stop is latched. There is no
arming step.

While a queue runs, the window watches: everything is shown, nothing can be
driven, and the emergency stop still works.

Not in this part: a points programmer, power and pyrometer and optics panes,
snapshots and video recording, focus controls beyond jogging z, a live camera
(sub-project 4).

## 2. One owner for each laser: `Lasers`

`LabSession` builds a `LaserSystem` per extraction driver and keeps them to
itself. The window must drive the same objects: the same tray, calibration,
corrections, camera and centering state. So the systems move into an object the
application builds once.

`libs/experiment`, `pychron/experiment/lab/lasers.hpp`:

```cpp
class Lasers {
 public:
  // One LaserSystem per driver of `line` that is an extraction device, with
  // the lab's corrections and (when it may be used here) its camera: what
  // LabSession did. `lab` and `line` must outlive it.
  Lasers(const Lab& lab, systems::ExtractionLine& line, std::function<bool(std::string_view)> simulated = {});

  laser::LaserSystem* find(std::string_view device);   // null: no such device
  std::vector<std::string> names() const;              // sorted
  std::vector<std::string> problems() const;           // a camera that cannot be used, per device
  const std::string* problem_of(std::string_view device) const;

  enum class Driver { None, Queue, Manual };
  class Lease;                                          // movable; releases when it goes
  // Config error naming who drives now when it is someone else. Manual may be
  // taken again by the holder of Manual (commands are serialised elsewhere).
  Result<Lease> drive(Driver who);
  Driver driver() const;
  // Devices whose emergency stop is latched.
  std::vector<std::string> stopped() const;
};
```

`SessionHardware` gains `Lasers* lasers = nullptr`. Given one, the session
uses it; given none (elctl, the tests), it builds its own, so nothing that
exists changes. `LabSession::start` takes the `Queue` lease for the life of the
queue and refuses to start while a laser is being driven by hand ("the laser
is being driven by hand") or while any device's emergency stop is latched
("co2: emergency stop not reset").

The lease is for the whole lab, not per device: a queue names its device per
run, and one rule is easier to keep than several.

## 3. `LaserSystem`: watched, and stoppable

Today its calls come from one thread. Now a second thread watches, and a third
may stop it.

**The gate.** One recursive lock, taken by every call that reaches the driver
or the centering state: the `IExtractionDevice`, `IStage` and (new)
`ILaserDevice` calls, the camera, the simulated camera's `sight()`. The frame
source the pattern runner is given is wrapped so its `grab()` takes the same
gate: one lock, so no order to get wrong. `mutex_` (the system's small state)
is still never held across a driver call and is always taken inside the gate.
`laser()` returns the system itself when the driver has a laser, so firing is
gated too.

**Snapshot.** `LaserSnapshot snapshot()`: tray, calibration state and why,
stage position, enabled, output, firing, tripped interlocks, what it is doing
(`Idle | Moving | Centering | Pattern`), the pattern's progress, the last
autocenter outcome, whether it has a camera, whether the emergency stop is
latched, and the first thing that could not be read. It never calls
`moving()`: that call advances a centering and counts a driver's arrival
polls, and belongs to whoever started the move. "Moving" is what the last
`moving()` said, set true by every move the system starts.

Interlocks need a driver call that does not exist:
`ILaserDevice::tripped_interlocks()` (default: none), implemented by the
Chromium driver from the status and interlock queries it already makes.

**View.** `Result<CameraView> view()`: one frame, the target the device's
finder sees in it (hole mode; glow mode while firing), the aim point in
pixels, pixels per millimetre and the expected hole radius. Config error with
no camera. It moves nothing.

**Emergency stop.** `Result<void> emergency_stop()`: beam off, output 0,
disable, stage stop, pattern stop, centering abandoned. Every step is tried
whatever the earlier ones answered; the first error is returned. It latches:
until `reset_stop()`, `enable`, `extract`, `fire_laser`, `warmup`, every move
and every pattern are refused with an Interlock error ("emergency stop: reset
it in the laser window"). A script that has not yet seen its queue aborted
cannot fire again in the gap. `stopped()` says whether it is latched.

## 4. Patterns: written, and replaced while running

- `std::string to_toml(const Pattern&)`: `kind`, `velocity`, `iterations` (not
  for a dragonfly) and the kind's own keys, so `Pattern::parse(to_toml(p),
  p.name) == p` for a pattern holding its kind's fields.
- `Result<std::filesystem::path> save_pattern(const std::filesystem::path& dir,
  const Pattern&)`: the name must be a plain file-name part
  (`safe_file_part`); the pattern must parse back (which is where a pattern of
  too many points is refused); written to a temporary file and renamed.
- `PatternLibrary` becomes safe to change while it is read:
  `find()` returns `std::shared_ptr<const Pattern>`, `put(Pattern)` adds or
  replaces one, and a reader keeps the pattern it found. The runner, the queue
  check and elctl take the new return type.

## 5. `LaserBridge` (Qt)

As `SpectrometerBridge`: a QObject on the main thread, the blocking calls on
one worker thread, results posted back. One per device.

Out, as signals: `snapshot(LaserSnapshot)` and, with a camera,
`view(CameraView)` every 250 ms (taken on the worker between commands and
while one waits); `commandFinished(what, Result<void>)`; `driverChanged`
(none / queue / manual); `calibrationChanged`.

In, each non-blocking: `go_to(hole, autocenter)`, `autocenter()` (the hole the
stage was last sent to), `jog(dx, dy, dz)`, `stop_stage()`, `set_tray(name)`,
`enable(bool)`, `fire(percent)`, `set_output(percent)`, `stop_beam()`,
`run_pattern(name)`, `stop_pattern()`, `add_calibration_point(hole)`,
`remove_calibration_point(hole)`, `clear_calibration()`, `emergency_stop()`,
`reset_stop()`.

- A command that starts a motion waits for it on the worker: it polls
  `moving()` / `running()` every 50 ms, publishing snapshots as it goes, until
  it ends or is cancelled. So nothing is ever left half centered.
- Every driving command takes the `Manual` lease for as long as it runs. With
  a queue running it fails at once: "a queue is running".
- `stop_stage`, `stop_beam`, `stop_pattern` cancel the command in flight and
  then act.
- `emergency_stop()` drops every queued command, cancels the one in flight,
  calls `LaserSystem::emergency_stop()`, then the `abort_queue` callback it was
  given (the session's `abort()`). It needs no lease.
- `fire(percent)` is `extract(percent, Percent)` then `fire_laser()`;
  `stop_beam` is `end_extract()`.
- Calibration commands read the stage position, change the tray's points in
  the `CalibrationStore` (solving first: a point that does not solve is
  refused and the file is left), then set the tray again so the system reads
  the new calibration.

## 6. Widgets

- `TrayView`: the holes in the tray's frame, to scale, numbered when there is
  room. With a calibration the stage is a crosshair (through the inverse
  transform); calibration points are ringed and corrected holes marked.
  `holeClicked(id)`, `holeMenu(id, pos)`. Without a tray: "no tray".
- `CameraView`: the frame scaled to fit, aim crosshair, the found target's
  circle, the expected hole circle. Without a camera: says so.
- `LaserWindow`: tray left, camera right over three tabs (Control,
  Calibration, Patterns); a top bar with the tray, a banner and the emergency
  stop; a status line. Watch-only while the queue drives: every control but
  the emergency stop is disabled and the banner says "queue running: watch
  only". After an emergency stop the banner says so and offers Reset.
- `PatternMakerWindow`: name, kind, the kind's fields (only those), velocity
  and iterations; the path drawn with its start, length and time, or for a
  dragonfly its perimeter and "follows the glow: no path"; what is wrong, as
  the parser says it; New, Open (the lab's patterns), Save. Saving writes the
  file and puts the pattern in the library, so the next run uses it.

## 7. Application

- `MainWindow::set_lasers(...)`: View ▸ Laser (a submenu with several
  devices), with a glyph and a shortcut as the other views; Laser window ▸
  Tools ▸ Pattern Maker.
- `pychron-ui --laser [--device <name>]`: the line, the lab and `Lasers` are
  brought up as usual, with no spectrometer and no experiment session, and the
  laser window is the main window. With several extraction devices `--device`
  is required; the error names them.
- Teardown: laser windows, then their bridges, then the session, then
  `Lasers`, then the line.

## 8. Errors

Nothing is guessed and nothing is hidden: a command's error goes to the status
line and the log as the core said it. A snapshot that cannot read the device
shows what it could and says what it could not. A camera that fails shows the
last frame greyed with the error. An emergency stop whose steps partly failed
says which, and stays latched.

## 9. Tests

- `libs/laser`: `to_toml` round trip for every kind; `save_pattern` (bad name,
  too many points, replaces atomically); library `put` while a found pattern
  is held; snapshot and view while another thread drives a centering and a
  pattern (run under the sanitizers); emergency stop mid-move, mid-centering and
  mid-pattern; the latch refuses and resets; interlocks in the snapshot.
- `libs/experiment`: `Lasers` builds what the session built; the lease;
  a session given `Lasers` refuses a queue under a manual lease and under a
  latched stop, and manual driving is refused while a queue runs.
- UI, on the simulator: every bridge command; watch-only during a queue;
  emergency stop mid-pattern and mid-queue (laser off, stage at rest, queue
  aborted); the window's enabled and disabled states; click a hole and the
  stage goes there; the calibration flow writes the file and shows the
  cautions; the pattern maker's preview, errors and save; the command line.

Everything is proven on the simulator only. No real Chromium has been driven.

## 10. As built

Where the code went another way from the sections above, and why.

- **Section 3, the gate.** The frame source is not wrapped. The pattern runner
  is (`LaserSystem::GatedRunner`): every runner call holds the gate, so the
  camera it uses is behind it too, and so is a stop from another thread.
  `pattern_runner()` and `laser()` therefore return the system's own objects,
  never the driver's.
- **Section 3, the view.** The target reported is the one nearest the aim
  point, wherever in the frame it is.
- **Section 4.** `save_pattern` refuses what does not parse back; it does not
  compare the result with what it was given, so a field of another kind set
  on the pattern is simply not written. `check_pattern` is the same refusal
  without the write (the pattern maker's). A name may hold spaces; it may not
  be empty, a path, or start with a dot. `pattern_fields`, `field_value` and
  `set_field` name a kind's keys for a form.
- **Section 5.** The worker is a `std::thread` with its own queue, not a
  `QThread` with an event loop: it has to poll, publish and be cancelled in
  the middle of a command. The bridge is given the `Lab` (trays,
  calibrations, patterns). The three stops take the manual lease like any
  other command, so during a queue only the emergency stop acts.
- **Section 6.** The window has a "Center holes" tick box (what a click on a
  hole does) and a context menu on a hole: go, go and center, calibration
  point here. A new output typed while the beam is on is sent on Enter.
  The pattern maker saves over a pattern of the same name without asking, and
  says "saved over".
- **Section 7.** Ctrl+Shift+B opens the laser window. The pattern maker is
  opened from the laser window's Patterns tab, not from a Tools menu. In the
  `--laser` flavor problems with the lab go to stderr (there is no log dock).
- **Interlocks** are shown as text ("interlocks: ok" or the tripped ones'
  names), not as lights: a driver names only what is tripped.

- **After review.** The stop is latched on the thread the button is pressed
  on, before the worker is asked (`LaserSystem::latch_stop`). Reset is refused
  and hidden while a queue holds the lasers. A queue is not started while a
  beam is on (`Lasers::firing`). A bridge that goes, and a window that
  closes, with the beam on close it. The output box sends on Enter only. The
  pattern maker shows numbers in full and takes a seed as text.

Known limit: `view()` holds the gate while it grabs a frame. The simulated
camera cannot block; a live camera (sub-project 4) must be grabbed outside
the gate, or a stalled camera would hold up the device side of a stop (the
latch itself no longer waits).

Not built, and still wanted: the laser window does not remember its size, its
tray or its step between runs of the program; there is no keyboard jogging.
