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
open Window > Spectrometer (Ctrl+Shift+S); the example
`spectrometer.sim-integrated.toml` is loaded with a beam that follows its
field table, and the window starts the scan when it opens:

```bash
build/dev-ui/apps/pychron-ui/pychron-ui --sim
```

Use `--spectrometer <file>` to load another spectrometer config (the menu
item stays disabled when neither flag is given or the file fails to load; the
error goes to the log dock). Window layout and graph settings are saved per
spectrometer under the `PychronLabs` organization in `QSettings`.

Window > Experiment (Ctrl+Shift+E) runs experiment queues against a lab
directory, as `elctl exp run` does: open a queue (Queue > Open, or
`--queue <file>`), edit it (rows revalidate as you type; red rows have
errors, see their tooltips), then Start (F5) from the selected row. The lab
is `--lab <dir>` (default: the extraction line config's directory) and
records go to `--data <dir>` (default: `<lab>/data`). The Run Factory dock
adds runs: fill in an identifier and position (a range such as `1-4` gives one
run per hole), check the preview, then Add (Ctrl+Return); new runs start from
the lab's `defaults.toml`, and `blocks/*.toml` are reusable sequences. With `--sim`,
`--sim-speed <x>` runs the whole app on simulated time x times faster than
real time:

```bash
build/dev-ui/apps/pychron-ui/pychron-ui --sim --sim-speed 50 --queue configs/examples/experiment.toml
```

On Ubuntu 24.04, `apt install qt6-base-dev libqt6sql6-sqlite` is enough for
the UI (Qt 6.4). If qcustomplot.com is unreachable, point
`FETCHCONTENT_SOURCE_DIR_QCUSTOMPLOT` at an unpacked QCustomPlot 2.1.1 source
(Debian's `qcustomplot_2.1.1+dfsg1.orig.tar.xz` has the same two files).

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
4. Run the app on that config and open Window > Spectrometer:

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

## 6. Checklist

- `ctest --preset dev` passes.
- `spec-router status` lists the merged units and the next wave.
- `~/Programming/pychron/pychron` exists.
