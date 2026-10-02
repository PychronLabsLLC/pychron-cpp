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

### Qt 6 (only for `apps/pychron-ui`)

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

## 5. Checklist

- `ctest --preset dev` passes.
- `spec-router status` lists the merged units and the next wave.
- `~/Programming/pychron/pychron` exists.
