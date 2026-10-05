# Developer setup (macOS)

How to set up a machine to build, test and continue developing pychron-cpp,
including the spec-router agent workflow. CI (`.github/workflows/ci.yml`)
covers Linux and Windows builds as well; this page is the macOS dev setup.

## 1. Toolchain

C++20, CMake >= 3.25. asio, toml++, GoogleTest and pybind11 are fetched by
CMake at configure time, so no library manager is required.

```bash
xcode-select --install                              # Apple clang
curl -LsSf https://astral.sh/uv/install.sh | sh     # uv (Python tooling)
uv tool install cmake
uv tool install ninja
```

Homebrew `cmake`/`ninja` work equally well.

### Python >= 3.12 (embedded scripting host)

`libs/scripting` embeds CPython through pybind11
(`find_package(Python 3.12 COMPONENTS Interpreter Development.Embed)`).
Without a suitable Python, CMake builds the stub host instead.
The python.org 3.14 installer (`/Library/Frameworks/Python.framework`) is
known to work.

### Qt 6 (`apps/pychron-ui` and `libs/persistence`)

```bash
brew install qt
```

Qt's PrintSupport module (needed by QCustomPlot) ships with `brew install qt`;
no extra package is required. QCustomPlot itself is downloaded at configure
time (UI builds only).

The `dev-ui` preset searches `$QT_PREFIX`, `~/Qt/6.12.0/macos`,
`/opt/homebrew/opt/qt` and `/usr/local/opt/qt`. To use an official Qt build
instead, install it under `~/Qt` (for example with `uvx aqtinstall`) or set
`QT_PREFIX`.

`libs/persistence` (the DVC store) uses TinyORM on QtSql: Qt Core and Sql
only, no GUI. TinyORM and range-v3 are fetched from git at configure time.
The library is skipped when CMake finds no Qt. The plain `dev` preset does
not search Homebrew's Qt, so use `dev-ui` or pass
`-DCMAKE_PREFIX_PATH=$(brew --prefix qt)`. Persistence tests always run on
SQLite. To also run them on PostgreSQL (this needs Qt's QPSQL driver plugin),
point `PYCHRON_TEST_PG_URL` at an empty database:

```bash
brew install postgresql@16 && brew services start postgresql@16
createdb pychron_test
export PYCHRON_TEST_PG_URL=postgresql://$USER@localhost/pychron_test
```

### Vision / OpenCV (optional)

`libs/vision` builds `LegacyFinder` (the port of the Python target finder, for
comparison) and `OpenCvSource` (video file or camera) only with OpenCV.
Without it both compile to stubs and everything else is unaffected.

```bash
brew install opencv          # macOS
sudo apt install libopencv-dev   # Ubuntu
```

`-DPYCHRON_VISION_OPENCV=AUTO|ON|OFF`: `AUTO` (default) uses OpenCV when CMake
finds it, `ON` fails the configure if it does not, `OFF` never looks. Built
and tested with OpenCV 5.0; the 4.x path is written but untested. Configure prints one line saying which was chosen.

## 2. Code

```bash
git clone https://github.com/PychronLabsLLC/pychron-cpp.git ~/Programming/pychron-cpp
git clone https://github.com/PychronLabsLLC/pychron.git ~/Programming/pychron
```

The Python pychron checkout must live at `~/Programming/pychron`: several
work units in `tools/spec_router/spec_router/units.toml` list
`~/Programming/pychron/pychron` as a read-only ground-truth reference
(`references = [...]`) that their agents read for vendor wire formats and
legacy behaviour.

## 3. Build and test

```bash
cd ~/Programming/pychron-cpp
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

With the UI:

```bash
cmake --preset dev-ui
cmake --build --preset dev-ui --parallel
ctest --preset dev-ui
```

To try the spectrometer window in simulation, run the app with `--sim` and
open View > Spectrometer (Ctrl+Shift+S); the example
`spectrometer.sim-integrated.toml` is loaded with a beam that follows its
field table, and the window starts the scan when it opens:

```bash
build/dev-ui/apps/pychron-ui/pychron-ui --sim
```

Use `--spectrometer <file>` to load another spectrometer config (the menu
item stays disabled when neither flag is given or the file fails to load; the
error goes to the log dock). Window layout and graph settings are saved per
spectrometer under the `PychronLabs` organization in `QSettings`.

View > Experiment (Ctrl+Shift+E) runs experiment queues against a lab
directory, as `elctl exp run` does: open a queue (Queue > Open, or
`--queue <file>`), edit it (rows revalidate as you type; red rows have
errors, see their tooltips), then Start (F5) from the selected row. The lab
is `--lab <dir>` (default: the extraction line config's directory) and
records go to `--data <dir>` (default: `<lab>/data`). The Run Factory dock
adds runs: fill in an identifier and position (a range such as `1-4` gives one
run per hole), check the preview, then Add (Ctrl+Return); new runs start from
the lab's `defaults.toml`, and `blocks/*.toml` are reusable sequences. The
Measurement tab beside it edits the selected row's plan and its exposed
parameters (overridden values are bold with a ● badge; Advanced allows any
value of the plan). Scripts > Script Editor (Ctrl+Shift+K) edits the lab's
scripts with highlighting, completion, the static check and estimate as you
type, and Ctrl+click on a gosub to open it. With `--sim`,
`--sim-speed <x>` runs the whole app on simulated time x times faster than
real time:

```bash
build/dev-ui/apps/pychron-ui/pychron-ui --sim --sim-speed 50 --queue configs/examples/experiment.toml
```

View > Data browses the records under `<data>/records`. With
`--db <url>` it browses a DVC store instead (`sqlite:/path/to/file.db` or
`postgresql://user:pw@host/db`; the schema must already be current, since
the UI never migrates it). Rescan picks up new analyses and revisions from
the store's change log. With a database, recall windows show each
analysis' revision history (History tab) and can save fit edits made in the
Evolutions tab (signal and baseline fits) as new revisions, or restore an
older revision from History, Plot > Isotope evolutions... refits many
analyses at once (Good / Bad under its preview train the isotope
classifier, kept in the app config directory), and Plot > Blanks... / IC factors... fits and
saves blanks and IC factors from reference analyses; saves are recorded under
`$USER` on this host's client:

```bash
build/dev-ui/apps/pychron-ui/pychron-ui --sim --db postgresql://me@labdb/pychron
```

On Ubuntu 24.04, `apt install qt6-base-dev libqt6sql6-sqlite` is enough for
the UI (Qt 6.4). If qcustomplot.com is unreachable, point
`FETCHCONTENT_SOURCE_DIR_QCUSTOMPLOT` at an unpacked QCustomPlot 2.1.1 source
(Debian's `qcustomplot_2.1.1+dfsg1.orig.tar.xz` has the same two files).

### Importing legacy data

`elctl import` brings legacy pychron repositories (and a converted database
dump) into a store; see `docs/legacy_import.md` for how to run it. A real run
against three public NMGRLData repositories is
`tools/import_fixture_check.sh build/dev` (needs network; not part of CI).

`libs/ingest` and `libs/dvc` build with persistence only. Their tests (and
`elctl`'s import tests) build git repositories and need `git` >= 2.32 on
`PATH`. Time zones come from `libs/ingest/src/tz.cpp`, which uses
`std::chrono`'s tz database where the standard library has one and Howard
Hinnant's `date` library otherwise; both need the system time-zone database
(`tzdata`, for example `apt install tzdata`; minimal container images often
lack it) on every platform path.

### Running against a Thermo instrument

The `thermo_qtegra` driver (Argus, Helix, through Qtegra's
RemoteControlServer) has never been run against an instrument. Every wire
detail comes from reading pychron's Python, and the tests run against a
simulated wire only. Expect the first contact to find differences, and treat
magnet moves, HV and detector protection as untested until you have watched
them work.

1. Copy `configs/examples/spectrometer.qtegra.toml`, `molecular_weights.toml`
   and `tables/` into a directory of your own, and copy
   `spectrometer.qtegra.local.toml.example` there as
   `spectrometer.qtegra.local.toml` (`*.local.toml` is git-ignored).

   The copied field tables, the detectors' deflection coefficients,
   `cdd_voltage`, `nominal_hv` and the protection and saturation thresholds
   are SIMULATION PLACEHOLDERS. Replace them with the instrument's
   calibration before any magnet move. Detector protection on a move is
   planned from the field table: with a table that does not describe the
   instrument, a move below the beam-blank threshold can leave the CDD
   unprotected while a major beam crosses it. Set the magnet's real range in
   both `limit_min` / `limit_max` under `[drivers.qtegra]` (enforced by the
   driver) and `[magnet].limits` (enforced by the facade; the stricter bound
   wins).

   Source ramping is not implemented: HV and trap current change in a single
   step, exactly as written (`SetHV v`, `SetParameter Trap Current Set,v`).
   The Qtegra example therefore has no `ramp` under `[source]`; adding one
   changes nothing. Step large changes by hand.
2. Set `host` and `port` of the PC running Qtegra under `[transports.qtegra]`
   in the `.local.toml` (it may override only `host`, `port`, `baud` and
   `timeout_ms`).
3. Set `trace = true` under `[transports.qtegra]` in the main config. The
   wire is recorded to `traces/qtegra.trace` under the directory the app is
   started from (`traces/<transport name>.trace`; the directory is created on
   demand). The trace file is truncated every time the app starts, so copy a
   capture you want to keep somewhere else before restarting.
4. Run the app on that config and open View > Spectrometer:

   ```bash
   build/dev-ui/apps/pychron-ui/pychron-ui --spectrometer <dir>/spectrometer.qtegra.toml
   ```

   Opening the spectrometer window starts a scan, which sends
   `SetIntegrationTime` if Qtegra's current period differs from the requested
   one. Do not add `--sim`: it refuses a spectrometer config that is not
   simulated, and this one is not. The connection is opened when the config
   loads, so an instrument that cannot be reached is a load failure (the
   error goes to the log dock).
5. Work through the bring-up checklist in section 8 of
   `docs/superpowers/specs/2026-10-01-qtegra-driver-design.md`. Its first
   item is what Qtegra replies to each setter: a setter that gets no reply
   fails with a timeout.

Build trees are 0.5-0.7 GB each; keep a few GB free, more when running
parallel agent waves (each agent worktree builds its own tree).

### Regenerating the Ar-Ar reduction golden vectors

The JSON under `tests/reduction/golden/` is produced by calling the read-only
legacy pychron checkout (`~/Programming/pychron`), never edited by hand. To
regenerate it, run the spec 9.1 command from the repo root (see the docstring
of `tools/reduction_golden/generate.py`):

```bash
uv run --no-project --python 3.12 \
  --with numpy==2.4.4 --with scipy==1.17.1 --with statsmodels==0.14.6 \
  --with uncertainties==3.2.3 --with traits==7.1.0 --with pyyaml==6.0.3 \
  python tools/reduction_golden/generate.py \
    --legacy ~/Programming/pychron --out tests/reduction/golden
```

Add `--check` to regenerate into a temporary directory and exit 1 if any
committed file differs (drift detector); it writes nothing to `--out`.

## 4. spec-router (agent workflow)

`tools/spec_router` splits the design specs into work units and runs each
unit through a headless Claude Code agent, then builds, tests, judges the
agent's report with TypeSafe (Jev) and merges.

1. Python venv:

   ```bash
   cd ~/Programming/pychron-cpp/tools/spec_router
   uv venv .venv
   uv pip install -e '.[dev]'
   .venv/bin/python -m pytest -q
   ```

2. Claude Code CLI (agents run as `claude -p`): install Node, then
   `npm install -g @anthropic-ai/claude-code` and run `claude` once to log in.

3. TypeSafe key: export `TYPESAFE_API_KEY` from `~/.zshrc`.

4. Router state is not in git. `.spec_router/` (state, saved plan, cached
   judgments) and `.worktrees/` are ignored. When moving machines, copy
   `.spec_router/` across. Without it, `spec-router status` reconciles merged
   units from the `merge unit/<id>` commits on `main`, but the plan is
   recomputed (TypeSafe calls).

Usage:

```bash
cd ~/Programming/pychron-cpp
tools/spec_router/.venv/bin/spec-router status
tools/spec_router/.venv/bin/spec-router run --wave <N> --parallel 3
tools/spec_router/.venv/bin/spec-router verify <unit> [--accept-judgment | --without-report]
```

The router refuses to run unless the repo is on `main` (merges land on HEAD).
A unit stopped by an account usage limit keeps its worktree; re-run the wave
to resume it.

## 5. Run the example experiment

`configs/examples` doubles as an example lab: `plans/`, `scripts/`,
`conditionals/` and a three-run `experiment.toml` for the simulated line and
spectrometer.

```bash
cd configs/examples
../../build/dev/apps/elctl/elctl -c extraction_line.toml exp validate experiment.toml \
    --spectrometer spectrometer.sim-integrated.toml
../../build/dev/apps/elctl/elctl -c extraction_line.toml --sim exp run experiment.toml \
    --spectrometer spectrometer.sim-integrated.toml --sim-speed 50
```

`--sim-speed` runs simulated time faster than real time. Records, the spool
and `executor_state.json` go to `./data` (ignored by git); `--resume` continues
after the last started run. Ctrl-C stops after the current run, a second
Ctrl-C cancels it, a third aborts.

Notifications: copy `notifications.toml.example` to `notifications.toml` in
the lab and fill in a channel (email, webhook or a local command) to get a
message when a run fails and when the queue ends. Email and webhooks need the
`curl` program; an SMTP password comes from the environment variable named by
`password_env`, a mail service's key (`provider = "brevo"`, `"resend"` or
`"postmark"`) from the one named by `api_key_env`. `elctl exp notify --lab .`
sends a test message on each channel (the experiment window: Executor > Send
Test Notification). The setup guide is `notifications.md`.

### A Chromium laser

`kind = "chromium"` drives a Photon Machines Chromium laser system (CO2
first): the Chromium program on the laser PC owns the laser, stage and camera,
and the driver sends it commands over one TCP connection it keeps open.
`configs/examples/laser.chromium.toml.example` has the two tables to add to a
line's `extraction_line.toml`; `elctl list-drivers` lists the keys.

- In Chromium, tick "TCP/IP Interface" in the Remote Control window (port
  1234).
- Output is percent, the only unit. Watts needs a power calibration and
  temperature a pyrometer; neither is done.
- The driver checks what legacy Pychron did not: that the program on the port
  is a Chromium, that no interlock is tripped before enabling and before each
  firing, that an output setpoint took, and that a move stays inside the
  configured travel. A command Chromium refuses (`?<n>`) is an error.
- The driver's own named positions are the scans Chromium has defined
  (`s3`). Holes on a tray are resolved above the driver (next section).
- With `kind = "sim"` on its transport the driver talks to a Chromium
  simulator (`ChromiumSim`).

It has not been run against a real Chromium; the protocol is from the vendor's
command reference (`docs/superpowers/specs/2026-10-04-chromium-protocol-survey.md`).

### Trays, calibration and a laser queue

A driver that is an extraction device (today: `chromium`) is named in a queue
by its driver name: `[drivers.co2]` in `extraction_line.toml` is
`extract_device = "co2"`. A line may have several. The lab directory holds
what turns a hole number into a stage position:

- `tray_maps/<tray>.txt`: the tray, in legacy Pychron's tray map format (copy
  the files from `setupfiles/tray_maps`). A queue's `tray` is the file's name
  without `.txt`.
- `stage_calibrations/<device>.<tray>.toml`: where that tray sits on that
  device's stage. Written by `elctl laser calibrate`; legacy calibrations
  (pickles) cannot be read and are made again.

To calibrate a tray, jog the stage onto a hole with the laser's own software
(Chromium), then record it:

```bash
elctl -c extraction_line.toml laser calibrate co2 221-hole center
```

```bash
elctl -c extraction_line.toml laser calibrate co2 221-hole right
```

`center` and `right` are the tray map's centre and east calibration holes;
`point <hole>` records any hole. One point places the tray, two also turn it
(the legacy "Tray" calibration), three or more are fitted and report an rms.
`--x` and `--y` give the position instead of reading it, with no hardware
opened. Then check it:

```bash
elctl -c extraction_line.toml laser goto co2 221-hole 17
```

```bash
elctl -c extraction_line.toml laser trays
```

Centre and right lie on one line, so they cannot show a mirrored axis, and
two exchanged holes fit perfectly with the tray half a turn round; neither
shows in the rms. `calibrate` and `trays` say so. Always check a new
calibration with `goto` on a hole off that line before firing.

What refuses to guess: a queue is not started if its device is unknown, its
tray has no map, a run's hole is not on the tray, or the tray is not
calibrated for the device. A calibration made before the tray map file was
edited is stale and must be made again. A fitted scale more than 2% from 1 is
refused (stage and map are both millimetres, so it means a wrong hole).

`configs/examples` has a simulated laser: `experiment.laser.toml` heats two
holes of `tray_maps/example-9.txt` with `scripts/extraction/laser_extract.py`.

```bash
elctl -c configs/examples/extraction_line.toml --sim exp run configs/examples/experiment.laser.toml --spectrometer configs/examples/spectrometer.sim-integrated.toml --sim-speed 50
```

`elctl laser goto` stops the stage on Ctrl-C and when `--timeout` runs out. A
script cancelled while it waits for a move stops the stage too. A hole move
does not change z.

### Autocenter and hole corrections

A calibration puts every hole within a fraction of a millimetre. With a
camera, a hole move goes on to centre the hole under the beam: the stage
arrives, waits `settle_ms`, the camera looks, the stage is nudged, and it
looks again, until the hole is within `tolerance_mm` of the aim point. A
script's `move_to_position()` asks for this by default
(`move_to_position(autocenter=False)` does not).

A device has a camera when the lab's `cameras.toml` has a table for it
(`configs/examples/cameras.toml` is commented):

```toml
[co2]
source = "sim"            # sim | recorded
px_per_mm = 23.0
flip_x = false
flip_y = true             # which way the picture moves when the stage does
aim_offset_px = [0, 0]
settle_ms = 200

[co2.autocenter]
tolerance_mm = 0.03
max_iterations = 4
max_step_mm = 0.5
frames_per_step = 3
on_failure = "continue"   # or "fail"
```

Only a camera that follows the stage is ever used to move it. A `recorded`
camera is for `elctl laser look` and never centres anything. A `sim` camera
centres holes only on a simulated laser: left in a lab whose laser is real,
it stops queues on that device from starting until the table is removed.

**There is no live camera yet.** `sim` is the simulated tray (the example's
is deliberately 0.15, -0.10 mm from its calibration, so `--sim` runs show
autocenter correcting it); `recorded` replays a folder of frames, for looking
at what the finder makes of real pictures. On a real Chromium nothing changes
until the screen-capture source exists.

What is found is kept per hole in `stage_corrections/<device>.<tray>.toml` and
is where the next move to that hole starts; a move that asks for autocenter
still checks, and updates it. Corrections are dropped when the tray map or the
stage calibration changes.

What it will not do:

- **Find the neighbour.** A hole is never taken more than 45% of the way to
  the nearest other hole (at most 1 mm) from its calibrated position; beyond
  that it is a failure, and a saved correction further off than that is
  ignored. A tray off by more than about half the hole spacing cannot be told
  from one that is right: recalibrate.
- **Guess.** When the hole is not seen, the camera fails, or the nudges make
  things worse (a wrong `flip_x`/`flip_y` shows as this), the stage goes back
  to where the centring started. With `on_failure = "continue"` (legacy
  Pychron's behaviour) the run carries on there; with `"fail"` the move is an
  error and the run stops before the laser fires.

```bash
elctl -c extraction_line.toml --sim laser autocenter co2 example-9 5
```

```bash
elctl -c extraction_line.toml laser corrections co2 example-9
```

```bash
elctl -c extraction_line.toml --sim laser look co2 --tray example-9
```

A script must wait for a move that centres: `move_to_position(block=False)`
is refused on a device with a camera unless it also says `autocenter=False`.
What each hole move did ("hole 3: centred, moved 0.150, -0.100 mm", or why
it was not centred) goes into the run's log.

`autocenter` moves to a hole, centres it and saves the correction (exit 1 if
it could not, whatever `on_failure` says); `corrections ... clear [<hole>]`
forgets them; `look` says what the finder sees and moves nothing.

### Laser patterns

A pattern is a path the beam is moved along while it heats. A run names one
(`pattern = "hexagon"` in its extraction) and its script runs it with
`execute_pattern()`; `configs/examples/experiment.laser.toml` does so on its
second run. Patterns are files in the lab, `patterns/<name>.toml`:

```toml
kind = "polygon"
velocity = 1.0      # mm/s
iterations = 1      # the whole pattern, repeated
radius = 1.0        # the kind's own keys
nsides = 6
rotation = 0
```

Lengths are mm and angles degrees, in the stage's axes, about wherever the
stage is when the pattern starts; it returns there at the end. The kinds and
their keys (names, geometry and defaults are legacy Pychron's):

| kind | keys |
|---|---|
| `polygon` | `radius`, `nsides`, `rotation` |
| `linear` | `length`, `rotation`, `npasses` |
| `circular_contour` | `radius`, `nsteps`, `percent_change` |
| `line_spiral` | `radius`, `nsteps`, `percent_change`, `step_scalar` |
| `square_spiral` | `radius`, `nsteps`, `percent_change` |
| `random` | `walk_x`, `walk_y`, `npoints`, `seed` (none: a new walk each run) |
| `rubberband` | `length`, `offset`, `rotation` |
| `raster` | `length`, `offset`, `rotation`, `dx`, `single_pass` |
| `trough` | `length`, `width`, `rotation`, `use_x` |

```bash
elctl laser patterns --lab configs/examples
```

lists them with their points, path length and time. To see where the beam
would go, `--dry-run` prints a pattern's points, and without it the pattern is
run from where the stage is, with the laser not fired:

```bash
elctl -c extraction_line.toml laser pattern co2 hexagon --dry-run
```

A queue is not started if a run names a pattern the lab lacks or whose file
does not load (a pattern may have 10 000 points over all its iterations). A
script must wait for its pattern: `execute_pattern(block=False)` is refused,
because the pattern only advances while the script waits. Whatever a script
leaves running is stopped when the run's extraction ends. A pattern is not checked against the stage's travel
beforehand: a point outside it ends the pattern there (the error names the
point) and the run's ending switches the laser off. Stopping a pattern stops
the stage where it is.

Legacy patterns are Python pickles (`setupfiles/patterns/*.lp`). Export them
once per lab; nothing in a pickle is run:

```bash
python3 tools/export_patterns.py /path/to/setupfiles/patterns /path/to/lab/patterns
```

It says what it could not carry over: arc, seek and dragonfly patterns, z and
power series, a spiral's inward direction.

Each segment is driven as a straight line at the pattern's velocity (the
speed is shared between the x and y axes).

Limits: the stage's arrival at each point is checked before the next is sent,
which pauses the beam for about 0.15 s at every point, so a pattern of
hundreds of points is slow and heats its vertices more. The speed is never
above the driver's `move_speed`.

Not done yet: a live camera, seek and dragonfly, autofocus, the laser window,
a pattern maker and on-screen calibration, watts and temperature.

## 6. Set up an install

`elctl init` installs a setup profile (`profiles/`: `argus`, `helix`, `ngx`,
`data-reduction`) into a folder and records it in your site config
(`~/.config/pychron/site.toml` on Linux; `$PYCHRON_SITE_CONFIG` overrides):

```bash
elctl init --list
elctl init argus --root ~/Pychron/argus     # asks; Enter accepts [defaults]
elctl doctor                                # checks the default install
elctl --install argus exp run experiment.toml --sim-speed 50
elctl init --reconfigure --set simulation=no --set qtegra_host=10.0.0.5
elctl doctor --probe                        # connects: the drivers' connect step
elctl init helix --root ~/Pychron/helix --set line_source=import \
  --set line_file=old/extraction_line.toml --set canvas_file=old/canvas.toml
elctl init helix --root ~/Pychron/helix --set line_source=legacy \
  --set legacy_folder=~/Pychron/setupfiles   # converts a legacy Pychron line
elctl import-line ~/Pychron/setupfiles       # prints the conversion; --out DIR writes it
```

A legacy Pychron line (`extractionline/valves.yaml` or `valves.xml`,
`canvas2D/`, `devices/`) is converted to `extraction_line.toml` and
`canvas.toml`. NGX valve controllers keep their address; controllers with no
pychron-cpp driver yet are simulated (`sim_valves`), and the report lists
what was not carried over (legacy extraction-line survey,
`docs/superpowers/specs/2026-10-03-legacy-extraction-line-survey.md`).

Instrument installs start in simulation; `CALIBRATE.md` in the install lists
what is still a placeholder. `--yes` takes every default, `--answers file.toml`
and `--set id=value` answer without prompts. A re-run never overwrites a file;
`--reconfigure` rewrites only files nobody edited and leaves `<file>.new`
beside edited ones.

`pychron-ui` does the same with a setup wizard. Started with nothing
installed, it opens the wizard: pick data reduction or an instrument, a
folder, and answer one page per question group; Install writes the files,
creates a local database, records the install and shows the doctor's checks.
After that `pychron-ui` opens the default install (`--install NAME` another;
File > Installations switches, sets the default or forgets one). A
data-reduction install opens the data browser alone. `pychron-ui --setup`
runs the wizard again; `--examples` (or `--sim` with nothing installed) opens
the example line in `configs/examples` as before.

## 7. Build the installers

`cpack` packages the programs, the profiles and the example configs
(`cmake/PychronInstall.cmake`, `cmake/PychronPackaging.cmake`): a `.deb` and
`.tar.gz` on Linux, a `.dmg` on macOS, an NSIS installer and `.zip` on Windows.

```bash
cmake -S . -B build/pkg -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_UI=ON -DBUILD_TESTS=OFF
cmake --build build/pkg
(cd build/pkg && cpack)
sudo apt install ./build/pkg/pychron_*.deb
elctl --version                                   # shows where the profiles were found
QT_QPA_PLATFORM=offscreen pychron-ui --self-test  # profiles, wizard, database plugin
```

To ship Python with the package (scripted extractions without a system
Python), unpack a python-build-standalone `install_only` archive and pass it
twice: `-DPython_ROOT_DIR=<dir> -DPYCHRON_BUNDLE_PYTHON=<dir>`. On macOS and
Windows the Qt runtime is copied in (`PYCHRON_DEPLOY_QT`). The `release`
workflow does all of this on a `v*` tag and attaches the packages to the
GitHub release; it also runs on pull requests that touch packaging.

## 8. Checklist

- `ctest --preset dev` passes.
- `spec-router status` lists the merged units and the next wave.
- `~/Programming/pychron/pychron` exists.
