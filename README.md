# pychron-cpp

A C++20 rewrite of [pychron](https://github.com/PychronLabsLLC/pychron), the
data acquisition and control software for noble-gas (Ar/Ar) geochronology
labs. It drives extraction lines (valves, gauges, pumps) and mass
spectrometers, runs automated experiment queues, and stores the results.

> **Status: in development.** Everything runs against the built-in simulator.
> No driver in this repository has been run against a real instrument yet;
> see [Hardware status](#hardware-status) before pointing it at one.

## What is here

| Path | What it is |
|---|---|
| `libs/core` | `Result`/`Error`, clock, scheduler, signal bus, logging, TOML config loading |
| `libs/transport` | Serial, TCP and simulated transports; trace recording and replay |
| `libs/codecs` | Pure wire-format encoders/decoders per device family |
| `libs/devices` | Drivers (gauges, relays, spectrometer roles) and the driver registry |
| `libs/sim` | Simulated extraction line and spectrometer (beam model, sim drivers) |
| `libs/systems` | Extraction line, switch manager, spectrometer facade, acquisition, jobs |
| `libs/experiment` | Queues, run plans, conditionals, the executor |
| `libs/scripting` | Embedded CPython host for extraction scripts |
| `libs/reduction` | Fits and data reduction |
| `libs/persistence` | Database-backed store for analyses (TinyORM on QtSql) |
| `libs/processing` | Browsing, recall and figures: analysis sources (record directories, the DVC store), quantities, figure options and presets, composable reduction units, time-series, ideogram, age-spectrum and inverse-isochron figures |
| `apps/elctl` | Command-line tool: validate configs, drive the line, run experiments |
| `apps/pychron-ui` | Qt 6 application: extraction-line canvas, log and alarm docks, spectrometer window, experiment window, script and conditionals editors, data browser, recall and figure windows |
| `configs/examples` | An example lab: line, canvas, spectrometers, plans, scripts, a three-run queue; `nmgrl/` is a full-size line (the NMGRL valve box, simulated) |
| `docs/superpowers/specs` | Design specs; `docs/superpowers/plans` holds implementation plans |
| `tools/spec_router` | Splits the specs into work units and runs them through coding agents |

The core libraries are Qt-free. Qt is used only by `apps/pychron-ui` and, for
its SQL layer, `libs/persistence`.

## Build and test

Requires a C++20 compiler and CMake 3.25 or newer. asio, toml++, spdlog,
GoogleTest and pybind11 are fetched by CMake at configure time.

```bash
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

With the Qt application (needs Qt 6; on macOS `brew install qt`):

```bash
cmake --preset dev-ui
cmake --build --preset dev-ui --parallel
ctest --preset dev-ui
```

[docs/dev_setup.md](docs/dev_setup.md) has the full macOS setup, including
Python for the scripting host and PostgreSQL for the persistence tests. CI
builds on macOS, Linux (gcc and clang with sanitizers) and Windows.

## Try it in simulation

Open the extraction-line window and the spectrometer strip chart
(View > Spectrometer):

```bash
build/dev-ui/apps/pychron-ui/pychron-ui --sim
```

Run the example experiment queue in the experiment window (Window >
Experiment, then Start or F5); `--sim-speed` runs simulated time faster:

```bash
build/dev-ui/apps/pychron-ui/pychron-ui --sim --sim-speed 50 --queue configs/examples/experiment.toml
```

Browse and plot the records a queue wrote (View > Data): filter, double-click
to recall an analysis, or select runs and choose a figure from "Plot" (time
series, ideogram, age spectrum, inverse isochron). Figure
options are edited in the dock and saved as named presets.

Run the example experiment queue from the command line:

```bash
cd configs/examples
../../build/dev/apps/elctl/elctl -c extraction_line.toml --sim exp run experiment.toml \
    --spectrometer spectrometer.sim-integrated.toml --sim-speed 50
```

`elctl help` lists every command.

## Hardware status

- **Extraction line:** Pfeiffer MaxiGauge, Granville-Phillips Micro-Ion and
  ProXR relay drivers exist and are tested against scripted and replayed
  traces.
- **Thermo Qtegra (Argus, Helix):** the `thermo_qtegra` driver is implemented
  from legacy pychron's Python and tested against a simulated wire only. The
  example config ships simulator calibration as placeholders. Read the
  bring-up checklist in
  [the driver spec](docs/superpowers/specs/2026-10-01-qtegra-driver-design.md)
  and "Running against a Thermo instrument" in
  [docs/dev_setup.md](docs/dev_setup.md) first.
- **Isotopx NGX:** not implemented. Findings and open questions are in
  [the NGX notes](docs/superpowers/specs/2026-10-01-ngx-driver-notes.md).

`pychron-ui --sim` refuses to load a spectrometer config that is not fully
simulated, so the flag cannot be used to dry-run against real hardware.

## Contributing

[AGENTS.md](AGENTS.md) covers the workflow, build notes and lifetime rules
for both people and coding agents.

## License

GNU General Public License v3. See [LICENSE](LICENSE).
