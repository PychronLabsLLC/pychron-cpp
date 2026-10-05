# Live camera

Sub-project 4 of the laser work. Until now a laser's camera was simulated or
a recording. This part adds cameras that are really there: any camera OpenCV
can open (a built-in or USB camera, for testing), with the seam and the
configuration for GigE cameras through Basler's pylon SDK, whose driver is
written when there is an SDK and a camera to prove it on. Screen capture of
Chromium's video window, the earlier plan, is dropped.

## 1. Sources

`cameras.toml`, per device:

```toml
[co2]
source = "opencv"        # sim | recorded | opencv | pylon
use = "center"           # center (default) | view: a picture only, never moves the stage
px_per_mm = 23.0

[co2.opencv]
device = "0"             # a camera's index, or a video file
width = 1280             # 0 or absent: the camera's own
height = 720
fps = 30
channel = "luma"         # luma | r | g | b
rotate = 0               # 0 | 90 | 180 | 270
roi = [0, 0, 0, 0]       # x, y, w, h; zero size: the whole frame

[co2.pylon]
serial = "40012345"      # empty: the first camera found
exposure_us = 10000
gain_db = 0
pixel_format = "Mono8"
packet_size = 1500
timeout_ms = 1000
```

The pylon table is read and checked now. Opening it says "built without
pylon" until the driver exists.

`libs/vision` gets a seam (`camera_backend.hpp`): a backend has a name, says
whether this build has it, opens a `CameraRequest` (device, size, rate, the
shape options, and the backend's own key/value options) into an
`IFrameSource`, and lists the cameras it finds. `opencv` and `pylon` are the
known backends; adding the pylon driver is one source file and a build
option (`-DPYCHRON_VISION_PYLON`), and nothing else moves.

## 2. The live feed

A camera read blocks, and may block for ever. `vision::LiveFeed` puts it on
its own thread and keeps the newest frame.

- `grab()` gives a frame whose read began after the call, or an error within
  `timeout` (1 s by default): never longer. Once a read has failed or timed
  out, `grab()` fails at once until a frame arrives again.
- `latest()` never waits: the newest frame, how old it is, the frame rate
  seen, and what is wrong if anything is.
- A camera that is lost is reopened every second.
- Time here is real time: a live camera does not run on a simulated clock.
  Frames are still stamped from the caller's clock.

`LaserSystem::view()` uses `latest()` for a live camera, so watching never
holds the gate across a camera read, and the device side of an emergency
stop waits for a dead camera at most once, for `timeout` (the known limit in
the laser window's design).

## 3. Who may move a stage

- A live camera over a real stage centers holes and follows the glow.
- A live camera over a simulated stage does not follow it: with
  `use = "center"` that is a problem for the device (its queues are not
  started), as a simulated camera over a real laser is.
- `use = "view"`, for any source: the picture is shown, the finder's target
  is drawn, and nothing the camera sees ever moves the stage. Hole moves go
  to their calibrated positions, a dragonfly is refused, and queues run. This
  is how a built-in camera is used for testing.
- A camera that cannot be opened is said (and reopened when it appears): for
  a `center` camera a problem, for a `view` camera a note.

## 4. Pixel scale from jogs

`px_per_mm` and the flips can be measured instead of typed. With something
the finder can see under the aim point: look, move the stage `step` in x,
look, move it `step` in y, look, return. The three sightings give the map
from pixels to stage millimetres (`CameraStageMap::solve`). It is refused
when nothing is seen at one of the three places, when the two axes disagree
about the scale by more than 3%, or when they are more than 3 degrees from
square. The result goes to `<lab>/camera_scales/<device>.toml` and overrides
`px_per_mm`, `flip_x` and `flip_y` from `cameras.toml`, which is not
rewritten. `elctl laser camera-scale <device> [--step <mm>]`, and a button in
the laser window's Calibration tab.

## 5. Snapshots

A laser system with a camera is an `IImaging`: `snapshot(name)` writes
`<lab>/snapshots/<device>/<name>.png` (the UTC time when no name is given;
never over a file that is there: `-2`, `-3`... is added) and returns the
path. 8-bit grey PNG from a small writer in `libs/vision` (stored deflate, no
dependency). Scripts' `snapshot()` reaches it; the laser window has a button;
`elctl laser snapshot <device> [name]`.

## 6. Finding cameras

`elctl laser cameras`: each backend, whether this build has it, and what it
finds. OpenCV has no list of cameras: indexes 0 to 7 are tried and each one
that opens is listed with its size and rate.

## 7. Window

The camera's picture says how old its frame is when it is stale and what is
wrong when the camera is lost; the last picture stays, greyed. Snapshot and
Measure scale buttons.

## 8. macOS

The app bundle carries `NSCameraUsageDescription`. The first open asks the
operator; refused, the camera cannot be opened and the error says so.

## 9. Tests

- `LiveFeed` on fake cameras that hang, die, crawl and come back: the
  timeout, fail-fast, reconnect, a frame newer than the call.
- Config (every key, every refusal), the use rule, a view camera never
  centering, the scale measured on the simulator against what the simulated
  camera was told, the scale store, PNG (read back by our own reader and by
  Qt's), snapshots, the chooser on fake backends.
- No test in CI opens a real camera. `PYCHRON_TEST_CAMERA=<index>` runs one
  that does.

Everything that moves a stage by a live camera is still unproven on
hardware: there is no real camera on a real stage here.

Not in this part: the pylon driver, exposure and gain controls, video
recording, colour, autofocus.

## 10. As built

- **Section 2.** `LiveFeed`'s opener is handed the clock to stamp frames
  from, and the feed stops handing it out when it is destroyed: a read that
  returns after the feed has gone touches nothing of the caller's. The
  reader thread is detached only when it does not end within the timeout.
- **Section 3.** `LaserSystem::attach_viewer` is how a camera for looking
  comes in; `can_center()` is what centering and the window ask. A simulated
  camera marked `view` over a real laser is not shown (it would show a tray
  that is not there): a note. A recorded camera is still never attached.
- **A camera that is not there at start.** The feed is made and goes on
  trying. For a `center` camera that is a problem, and stays one until the
  program is started again even if the camera appears: `Lasers`' problems
  are fixed when it is built.
- **Section 4.** `camera-scale` in elctl takes a tray and a hole and goes
  there first. The measurement looks for targets of any size (the scale that
  says how big a hole should look is what is being measured). The store's
  file is the map (`m`, `residual_mm`) under a comment: only the map is read
  back. A scale file that cannot be read is a problem for a `center` camera.
- **Section 7.** Frame age is shown only when the camera has stopped.
- **Not proven here:** the built-in camera could not be opened by the
  session that wrote this (macOS did not allow it); `RealCamera` is the test
  to run by hand. No live camera has centered a hole on a real stage.

### After review

- A camera whose read (or open) has been in flight longer than its timeout is
  said to have stopped whether or not anything waited for it: a window that
  only looks is not shown a frozen picture as live.
- A centering that was waiting for its camera when the stop was latched sends
  nothing more to the stage. One look waits once for its frames, not once
  per frame, and a camera that centers may have a `timeout_ms` of 2000 at
  most (100 at least, for any).
- The scale measurement takes three sightings (x, y and both) and refuses: a
  move of less than 5 pixels (a picture that does not follow the stage); a
  move with something else about as near; a move nothing like what the scale
  believed so far expects, so `px_per_mm` must be roughly right to begin
  with; sightings that disagree. The default step is 0.5 mm.
- A measurement remembers the camera setup it was made with (source, device,
  size, rotation, roi, channel); with another setup it is a problem until it
  is measured again or cleared.
- Every elctl command uses the measured scale.
- A video file never centers a stage; a camera whose open does not answer is
  a problem for a camera that centers.

### Video

The picture was first shown four times a second, with the device's status,
and the finder was run on the whole frame under the gate: 0.3 s a look on a
1280 x 720 frame in a debug build. Now `LaserSystem::picture()` is the frame
alone; `view()` looks only in a square about the aim point (eight hole radii
wide, 160 pixels at least) and outside the gate; and the bridge has a video
thread that shows a picture every 40 ms, looks every 250 ms and draws the
last sighting on the frames between. A slow repaint drops frames instead of
queueing them. The window says the rate it shows.

The finder is not run on the video at all unless the device is itself
looking for something (`LaserSystem::looking()`: a hole being centered, a
pattern following the glow). The dashed circle of the expected hole size
and the aim crosshair are always drawn.
