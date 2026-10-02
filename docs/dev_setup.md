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

## 5. Checklist

- `ctest --preset dev` passes.
- `spec-router status` lists the merged units and the next wave.
- `~/Programming/pychron/pychron` exists.
