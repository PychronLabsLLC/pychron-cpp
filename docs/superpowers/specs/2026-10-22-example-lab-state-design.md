# Shipped examples stay as shipped: runtime state elsewhere

Status: agreed, 2026-10-22.

## 1. Problem

`elctl -c configs/examples/extraction_line.toml --sim exp run configs/examples/queues/experiment.laser.toml`
writes `configs/examples/stage_corrections/co2.example-9.toml`
(`CorrectionStore`, `<lab>/stage_corrections`, `libs/experiment`
`lab.cpp`). `pychron-ui --sim` on examples writes
`configs/examples/extraction_line.state.toml` (`ExtractionLine::load` default:
`<system file stem>.state.toml` beside config). Also, by same rule: records
and rise rates under `<lab>/data`, snapshots under `<lab>/snapshots/<device>`,
camera scales under `<lab>/camera_scales`.

All git-ignored: `git status` silent. But:

- Tests copying `configs/examples` carried stray state in: one stray
  correction failed 9 integration tests (`ViewCameraTest`, `LaserCameraCmd.*`,
  `HiddenHole*`) deterministically.
- macOS install: examples ship inside app bundle (`<app>/Contents/Resources/examples`,
  `setup::find_resources`). Writing there wrong: alters signed bundle, shared
  by every user, lost on update.

## 2. Rule

Shipped examples read-only to programs. Program run on a lab that is shipped
examples (or folder under it, e.g. `examples/nmgrl`) writes its runtime state
under per-user examples state folder, never beside examples.

Runtime state = what program writes without operator asking to edit a lab
file:

| What | Today | Examples |
|---|---|---|
| line state (locks, valve states, stats) | `<config stem>.state.toml` beside config | `<state>/<config stem>.state.toml` |
| autocenter corrections | `<lab>/stage_corrections/` | `<state>/stage_corrections/` |
| camera scales | `<lab>/camera_scales/` | `<state>/camera_scales/` |
| snapshots | `<lab>/snapshots/<device>/` | `<state>/snapshots/<device>/` |
| data (records, rise rates), when no `--data` / install data | `<lab>/data` | `<state>/data` |
| field tables (peak center saves a version, rewrites `current`) | `<spectrometer dir>/tables/` | `<state>/tables/`, see 3.5 |

`<state>` for lab `<examples>/<rel>` is `<user examples dir>/<rel>`
(`<rel>` empty for examples root).

Any lab that is not shipped examples (an install, a scratch copy in a test,
any other folder): unchanged, state beside lab.

## 3. Interfaces

### 3.1 Per-user folder (`libs/core` `log_dir.hpp`, beside `default_log_dir`)

```cpp
// Where the programs keep what they write while running the shipped
// examples: macOS ~/Library/Application Support/Pychron/examples, Linux
// $XDG_STATE_HOME/pychron/examples (~/.local/state/pychron/examples when unset
// or empty), Windows %LOCALAPPDATA%\pychron\examples. Empty when the variable
// that folder needs (HOME / LOCALAPPDATA) is unset or empty.
[[nodiscard]] std::filesystem::path default_examples_state_dir(HostOs os, const EnvLookup& env);
```

Pure, as `default_log_dir`. macOS folder same parent as `site.toml`.

### 3.2 Deciding (`libs/setup` `installer.hpp`)

```cpp
// The folder a program keeps a lab's runtime state in: `lab` itself, unless
// `lab` is `examples` or a folder under it; then the same place under
// `user_dir` (default_examples_state_dir). Compared after weakly_canonical,
// so a relative or symlinked path to the examples is still the examples. When
// `user_dir` is empty: <temp>/pychron/examples.
[[nodiscard]] std::filesystem::path lab_state_dir(const std::filesystem::path& lab,
                                                  const std::filesystem::path& examples,
                                                  const std::filesystem::path& user_dir);
```

Only place the rule lives. Programs call it once with
`find_resources().examples`.

### 3.3 Lab (`libs/experiment` `lab.hpp`)

`LabPaths` gains `std::filesystem::path state_dir;` (empty: `dir`) and
`const std::filesystem::path& state() const` (`state_dir` or `dir`).
`load_lab` makes `corrections` on `state() / "stage_corrections"`,
`camera_scales` on `state() / "camera_scales"`. `Lasers` sets snapshot dir to
`state() / "snapshots" / <device>`. `stage_calibrations` stays on `dir`
(shipped, read).

### 3.4 Programs

- `elctl exp run`, `elctl laser`: `state = lab_state_dir(lab, examples, default_examples_state_dir(...))`.
  `LabPaths::state_dir = state`; `ExtractionLine::Options::state_file =
  state / (<config stem> + ".state.toml")` when state differs from lab;
  default data dir `state / "data"` (only when `--data` and install data
  absent); laser snapshots `state / "snapshots" / <device>`.
- `pychron-ui`: same for `lab_dir` (examples via `--examples`, no args with
  `--sim`, or files under examples).
- When `state` differs from lab, programs log once at info:
  `examples: runtime state in <state>`.
- Folder made when first written (stores already do this); nothing created
  at start-up.

### 3.5 Field tables (added while implementing)

Queue's peak center saves new table version by default
(`PeakCenterConfig::save_table`): `<spectrometer dir>/tables/<name>/<stamp>.toml`
and tracked `current` rewritten. Unlike corrections, examples ship tables,
so state folder needs them to start from.

- `cfg::SpectrometerData::table_root` (empty: `root`): where tables are read
  and saved. `cfg::load_spectrometer(config, table_root = {})` reads them
  there; assembler sets `Spectrometer::Options::data_root` from it.
  Weights, profiles still read beside config.
- `LabPaths::spectrometer_tables` (empty: beside config) passed to
  `load_spectrometer` by `load_lab`.
- `setup::field_table_root(root, examples, user_dir) -> Result<path>`:
  `lab_state_dir(root, ...)`. When that is not `root`: each
  `<root>/tables/<name>` that has `current` is copied to
  `<state>/tables/<name>` unless `<state>/tables/<name>/current` exists. What a
  session saved kept; a version shipped later not seen until copy removed.
  Copy failure: `Io` error, spectrometer not loaded, said as any spectrometer
  error.
- `elctl exp run`, `pychron-ui`: pass it for spectrometer config. Field table
  editor follows (`Spectrometer::data_root`).

### 3.6 Tests

`tests/support/example_lab.hpp` `copy_lab(from, to)`: copy of a lab leaving
out runtime state (`data`, `notifications.toml`, any `stage_corrections`,
`snapshots`, `camera_scales`, any `*.state.toml`, `*.local.toml`; mirrors
`.gitignore`). Every fixture that copies `configs/examples` uses it. Tests then
pass with stray state beside examples.

New tests: `lab_state_dir` (examples root, subfolder, relative path, symlink,
other lab, empty user dir); `default_examples_state_dir` per OS; `load_lab`
with `state_dir` puts corrections and scales there; elctl `exp run --sim` on
`PYCHRON_EXAMPLES_DIR` pointed at a scratch "examples" leaves it byte-identical
and writes state under a scratch `HOME`/`XDG_STATE_HOME`.

## 4. Rules that must hold

- Running `elctl` or `pychron-ui` on shipped examples leaves examples folder
  byte-identical (state, data, snapshots, corrections, scales, field tables).
- Lab that is not shipped examples: every path as before. Install layout,
  doctor, installer unchanged.
- One function decides (`setup::lab_state_dir`); programs never restate rule.
- `libs/experiment` and `libs/systems` do not learn about examples: they take
  paths.

## 5. Out of scope

- Operator edits of lab files on examples: saving a queue, peak center
  config, `elctl laser calibrate` (writes `stage_calibrations`,
  shipped and tracked). Still write into examples. Follow-up: refuse with
  "the shipped examples are read-only; install a lab (elctl setup)" or save a
  copy.
- Moving existing stray state out of a developer's `configs/examples`: not
  migrated; delete by hand.
- Cleaning per-user examples folder.
- Process log (already per-user, global logging spec).
