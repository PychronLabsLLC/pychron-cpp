# The spectrometer

The spectrometer window is a live view of the mass spectrometer: a scrolling
strip chart of every detector's signal, a table of the latest intensities, and
the few controls an operator needs while watching the beam (integration time,
which detectors are drawn, and which isotope the magnet sits on).

This page also covers the files that describe the spectrometer and its
measurement plans, and how far each instrument family is supported.

Related pages: [the extraction line](02-extraction-line.md),
[experiments](04-experiments.md), [data analysis](06-data-analysis.md),
[configuration reference](07-configuration-reference.md),
[scripting](09-scripting.md), [troubleshooting](10-troubleshooting.md),
[glossary](11-glossary.md).

## Opening the window

Choose **View > Spectrometer** (Ctrl+Shift+S, Cmd+Shift+S on a Mac) in any
Pychron window.

The menu entry is greyed out when:

- no spectrometer config was given or installed (an installation names one;
  otherwise start with `pychron-ui --spectrometer <file>`, or `--sim`, which
  uses the example simulated spectrometer);
- the config failed to load (the log dock shows
  `ERROR [ui] spectrometer not loaded: ...`);
- the extraction line failed to start. The spectrometer polls on the line's
  scheduler, so it is not offered without a running line
  (`ERROR [ui] spectrometer unavailable: extraction line did not start`).

The title reads **Spectrometer**, or **Spectrometer (Simulation)** when
everything in the loaded config is simulated.

**Opening the window starts the scan; closing it stops it.** The window starts
reading the detectors as soon as it is shown (not when it is merely restored
from being minimised), at the integration time you have selected, and stops
when you close it.
Close it before running something that wants the spectrometer to itself.

## What is on the screen

| Area | Contents |
|---|---|
| Centre | The strip chart. A red banner with a **Restart** button appears above it when something is wrong (see below). |
| Left dock **Controls** | Integration time, graph settings, the detector list, the Magnet group. |
| Right dock **Intensities** | A table of each detector's latest value. |

Docks can be dragged, floated and resized; the layout is remembered per
spectrometer and per user.

### Strip chart

Time (seconds since the chart started or was cleared) runs along the bottom,
signal up the side. Each detector is one line in its own colour (the colour
from the config, or a default palette). The chart redraws at most every 50 ms.

Under **Graph** in the Controls dock:

| Control | Effect |
|---|---|
| Scan Width (mins) | How much history the chart shows. Minimum about one second. |
| Scale | `linear` or `log`. On a log axis, zero and negative values are not drawn. |
| Autoscale Y | On: the axis follows the visible data (re-fitted about twice a second, with a 10 % margin). Off: you set Max and Min. |
| Max / Min | Y limits. Read-only while autoscale is on; when you turn autoscale off they start at the current range. A pair that is not Min < Max (or Min <= 0 on a log axis) is rejected and reverts. |
| Clear | Empties the chart. The data table and the instrument are unaffected. |

Unchecking a detector under **Detectors** hides its line only. It keeps being
read and still shows in the table; nothing is sent to the hardware.

### Intensities table

Columns: Colour, Name, Isotope, Intensity, ±1σ, Units.

- **Intensity** is the latest integrated value (5 decimals).
- **±1σ** is the standard deviation of that detector's recent readings (a
  short rolling window), so it tells you how steady the beam is, not the
  measurement uncertainty an analysis will report.
- **Isotope** is the isotope the magnet is currently set to put on that
  detector. It changes when you press **Apply**.
- A **red** Intensity cell with the tooltip `saturated` means the detector is
  reporting saturation for that reading (the detector's `saturation` limit).
- A dash means no reading has arrived for that detector yet.

### Integration time

| Field | Meaning |
|---|---|
| Requested | A choice of 0.1, 0.2, 0.5, 1, 2, 5 or 10 s (or the config's `integration_time_s` if that is something else). Changing it restarts the scan at the new period. |
| Actual | The period the instrument actually delivered. Vendor instruments snap the request to what they support, so the two can differ. It fills in once the first frame arrives. |

### Magnet group

| Control | What it does |
|---|---|
| Detector | The detector you want the beam on. Starts on the reference detector. |
| Isotope | The isotopes the field table defines for that detector. Changing the boxes moves nothing. |
| **Apply** | Moves the magnet so that isotope lands on that detector, then records that detector's isotope. Disabled while a move is in progress. |
| Position | The magnet's position in its native units (`dac`, `field` or `mass` according to `native_axis`). |
| Mass on H1 | The mass currently on the reference detector (the label uses your reference detector's name). |

**Large-move confirmation.** If the move would change the mass on the
reference detector by more than a threshold (default **5 amu**), or the
current or target position is unknown, Pychron asks "This moves H1 by N amu.
Move the magnet?" (or "The current magnet position is unknown. Move the
magnet?"). The default answer is No. Set the threshold in **File >
Preferences... > Spectrometer** ("Ask before moving the magnet more than");
set it to 0 ("Never ask") to switch the question off. Be careful with that
setting on a real instrument: on a vendor instrument the detector protection
and beam-blank behaviour for a move come from the field table.

### When the banner appears

The banner shows the most recent problem, with **Restart** (stops and restarts
the scan at the selected integration time):

- a command error, for example `position: <reason>` or `start: <reason>`,
  until a later command succeeds;
- a stall: `no frame from acquirer 0 within N s`. The acquirer is the part of
  the driver that delivers readings; the limit is `timeout_factor` times the
  integration time plus a small slack. The same message raises a warning in
  the **Alarms** dock under source `acquisition`.

A stalled scan is usually a lost connection or the instrument software
having stopped; check the health bar on the extraction-line window (the
spectrometer's transport appears there as a chip too) and the log.

### What the window does not do

The window is deliberately small. It has **no** controls for peak centering,
detector deflection, source (HV, trap current), baselines, saving a scan to a
file, event markers or magnet DAC sliders. Those are done by measurement plans
and scripts in the [experiment window](04-experiments.md) (see "Peak centering"
below). The 2026-10-01 spectrometer-window design spec lists these as out of
scope for version 1.

## Measurement plans

A **measurement plan** is a TOML file that tells the spectrometer what to do
for one run: which isotopes to put on which detectors, how many readings, how
long to equilibrate, where to take baselines and peak centres, how to fit.
The run in the experiment queue names the plan; the queue never contains the
plan itself.

Plans live in the `plans/` folder of the lab directory. The setup wizard
writes three starting plans for an instrument install:

| Plan | What it does |
|---|---|
| `multicollect` | One hop: every detector that has an isotope collects it at once. 2 cycles of 15 counts. |
| `multicollect_hop_ar39` | Two hops: everything except Ar39, then the magnet moves Ar39 onto the ion counter. 10 cycles of 5 counts per hop. |
| `detector_ic` | Detector intercalibration: the reference isotope is hopped onto each Faraday in turn, 10 cycles, to compare detectors on the same gas. |

The simulated example lab ships `plans/sim_multicollect.toml`. Choose a plan
for a run in the experiment window's **Measurement** dock (Family, then Plan;
a plan is meant for the analysis types in its `analysis_types`). Edit plan files freely,
with the program closed or at least not mid-run, and check them with:

```bash
elctl exp validate experiment.toml --lab <lab dir> --spectrometer spectrometer.toml
```

### Reading a plan

This is `configs/examples/plans/sim_multicollect.toml`, annotated. A run
executes the blocks in this fixed order, skipping any that are switched off:

`peak centre (before)` > `baseline (before)` > `move to the first hop` >
`equilibrate while sniffing` > `main (cycles x hops)` > `baseline (after)` >
`peak centre (after)`.

```toml
[plan]
name = "sim_multicollect"             # what a run refers to
instrument_family = "sim"             # required
description = "Example two-hop measurement for the simulated lab"
analysis_types = ["unknown", "blank_unknown", "air", "blank_air"]  # run types it applies to

[detectors]
reference = "H1"        # must be a detector in the spectrometer config
# exclude = ["CDD"]     # leave a detector out of this run

[equilibration]         # gas from the extraction line into the spectrometer
inlet = "@valves.inlet"     # '@x' = a name defined by the lab, see below
outlet = "@valves.outlet"
time_s = "@extraction.eqtime"   # seconds the inlet stays open
inlet_delay_s = 3               # outlet closes, wait this long, then inlet opens
# close_inlet = true

[sniff]                 # readings taken while the gas is equilibrating
enabled = true
counts = 10
integration_s = 1

[peak_center]           # centre the peak on the magnet before/after the run
after = true
detector = "H1"
isotope = "Ar40"
config = "default"      # a named table in the lab's peak_center.toml

[baseline]              # measure with the magnet off the peaks
after = true
counts = 10
mass = 34.2             # a mass with no signal on any detector
settle_s = 5
integration_s = 1

[main]                  # the real measurement
cycles = 2              # the hop list is repeated this many times
integration_s = 1

[[main.hops]]           # hop 1: Ar40 on H1 and Ar39 on H2 together
positions = { Ar40 = "H1", Ar39 = "H2" }
counts = 15             # readings per cycle at this hop
settle_s = 2            # wait after the magnet moves

[[main.hops]]           # hop 2: move the magnet to put Ar36 on CDD
positions = { Ar36 = "CDD" }
counts = 15
settle_s = 2
position = { isotope = "Ar36", detector = "CDD" }  # where to put the magnet

[fits]                  # how isotope evolutions are fitted
signal = { default = "linear" }
baseline = { default = "average" }

[conditionals]
include = ["@conditionals.default_unknown"]  # checks that can truncate/stop the run

[parameters]            # what the Measurement dock lets you change per run
expose = ["main.cycles", "main.hops[0].counts", "main.hops[1].counts", "baseline.counts"]
```

Notes on the parts people trip over:

- **Hops and multicollection.** A hop is one magnet position with several
  detectors collecting at once. `positions` maps isotope to detector; every
  detector may appear once per hop. A plan with a single hop is plain
  multicollection; several hops are peak hopping. The magnet goes where
  `position = { isotope, detector }` says; without it the hop must include the
  reference detector and the magnet goes to that isotope on it
  (`reference detector 'H1' not in hop and no 'position'` otherwise). The
  magnet does not move, and does not wait the settle time, when it is already
  in place. One hop for N cycles collects the same as one hop with N times the
  counts.
- **Baseline hops** inside `main` use `baseline = true` and `mass = ...`.
  A hop-level `protect = ["CDD"]` lists detectors to protect (deflect or
  blank) during that hop's move.
- **Detector names, isotope names and the reference** are checked against the
  spectrometer config and its field table. An unknown detector is an error
  (`unknown detector 'X'`); so is an unknown key anywhere in the file
  (`unknown key 'x'`).
- **Aliases** (`"@valves.inlet"`) are resolved from the extraction line's
  `[aliases]` table (and the spectrometer's), so one plan serves any line.
  An alias that is not defined is `unresolved alias '@x'`.
- **Fits** are `average`, `linear`, `parabolic`, `cubic`, `exponential` or
  `custom_poly`, chosen per isotope (`{ Ar40 = "parabolic", default =
  "linear" }`) or as a single name; `error` is `sem` or `sd`; `[fits.outliers]`
  has `enabled`, `iterations`, `std_devs`.
- **Whiff** (`[whiff]`): an early look at the gas, with checks whose action is
  `run_remainder`, `pump` (close inlet, open outlet, end the run) or `abort`.
- **`[parameters] expose`** is a list of paths a run may override from the
  Measurement dock. The dock lists exactly these. Tick **Advanced: override
  any value of the plan** to override any value; overridden values are marked
  and the status line shows the estimated measurement time and the number of
  overrides. **Reset All** discards the overrides.
- **`hook`** at the top level names an escape-hatch script for steps the plan
  cannot express (see [scripting](09-scripting.md)).

### Detector intercalibration (IC)

Each detector has its own gain and efficiency. The factor that puts them on
one scale is the **IC factor**: the same gas measured on two detectors, as a
ratio. In Pychron:

1. Measure it with the `detector_ic` plan (or an air shot with a plan that
   hops the same isotope across detectors). Air runs have a known
   composition, so the ratios of the intercepts are the factors.
2. The experiment record stores the IC factor used for each detector; while a
   run is measuring, conditionals see the corrected value (intercept minus
   baseline, times IC factor). A detector with no stored factor is 1.0.
3. Fit and apply IC factors to unknowns in the Data window (**View >
   Data**, then in the browser's **Plot** menu: **IC factors...**), as
   described in [data analysis](06-data-analysis.md).

Write the plan so that the IC reference gas does not overload an ion counter:
the starter `detector_ic` plan hops only Faradays for exactly that reason.

### Peak centring

Peak centring finds the magnet setting that puts an isotope's peak in the
middle of a detector, and (by default) writes the correction into the field
table so later moves use it. There is no button for it in the spectrometer
window; it runs from a plan (`[peak_center] before/after = true`) or a
script. The result appears in the **Evolutions** dock of the experiment
window under **Peak centers** as `Ar40 on H1: center 5.001234, table
updated`, or `failed: <reason>`. A failed centring is reported and the run
continues; it is not an error.

The settings are named tables in the lab's `peak_center.toml`; a plan picks
one with `config = "default"`:

```toml
[default]
isotope = "Ar40"
detector = "H1"
window = 0.06            # half-width of the scan, in the magnet's native units
step = 0.002             # step size
percent = 80             # peak edges measured at this % of the maximum
min_peak_height = 1.0    # smaller than this is "no peak"
n_tries = 2              # retries, with a wider window if the signal was weak
integration_s = 1.0
baseline_timeout_s = 10  # how long to wait for the signal to drop at the start
update_table = true      # write the result to the field table
propagate = true         # and move the other detectors' columns with it
# also accepted: additional_detectors, center, direction ("increase"/"decrease"),
# settle_s, dac_offset, peak_shift_threshold, test_peak_flat
```

Choose `window` from your real peak width: the example's wide window suits
the simulator's broad peaks, and the program's built-in default is much
narrower. A window that misses the peak fails the centring. An unknown key in
this file is an error, and `elctl exp validate` reports a plan whose
`config` is not in the file.

`update_table = true` writes a **new version** of the field table (a
timestamped file under `tables/<name>/`, with a `current` pointer file naming
the active one). Old versions are kept; nothing is overwritten.

## The spectrometer config

The spectrometer is described by one file, `spectrometer.toml` (named by your
installation, or by `--spectrometer`), plus optional machine-local overrides,
a mass table and field tables. A shipped simulated example
(`configs/examples/spectrometer.sim-integrated.toml`), shortened:

```toml
[system]
name = "sim-integrated"
reference_detector = "H1"        # the detector masses are quoted on; must exist
integration_time_s = 1.0         # starting integration time

[transports.sim]                 # connections, as in extraction_line.toml
kind = "sim"

[drivers.sim]                    # the vendor-box driver
kind = "sim_integrated"
transport = "sim"
roles = ["positioner", "source", "acquirer", "detector_control", "beam_blank"]
channels = ["H2", "H1", "AX", "L1", "L2", "CDD"]

[magnet]
positioner = "sim"               # the driver that moves the magnet
native_axis = "dac"              # dac | field | mass: what the position number means
limits = { min = 0.0, max = 10.0 }   # never move outside this
settle_ms = 500
field_table = "argon"            # tables/argon/: where each isotope sits on each detector
hv_table = "argon_hv"
propagate = false
corrections = { deflection = true, hv = true }
protection = { detectors = ["CDD"], beam_blank_threshold = 0.5 }

[source]
driver = "sim"
nominal_hv = 4500                # required when corrections.hv = true

[acquisition]
acquirers = ["sim"]              # which drivers deliver readings
stale_frame_guard = true
timeout_factor = 2.0             # stall alarm after this many integration periods

[detector_control]
driver = "sim"

[[detectors]]                    # one per detector, left to right
name = "H1"
kind = "faraday"                 # faraday | counter | cdd | atona
channel = "sim:H1"               # "<driver>:<channel>"
units = "fA"                     # cps for counters and CDDs
isotope = "Ar40"                 # starting isotope shown in the table
color = "#ff7f0e"
active = true
deflection = { control = true, correction = [0.0, 0.0012], sign = 1, max = 800, per_volt = 0.0031 }
protection = { threshold = 5e5, on_move = true }
saturation = 4.9e6               # a reading at or above this shows as "saturated"
```

Things worth knowing:

- **Roles.** A *role* is a job the spectrometer needs done: `positioner`
  (magnet), `source`, `acquirer` (readings), `detector_control`
  (deflection, protection) and `beam_blank`. A vendor box (Qtegra, NGX) plays
  several roles from one driver on one connection. Legacy split instruments
  use one driver each (`dac_positioner`, `adc_bank`, `pulse_counter`,
  `serial_hv`); see `configs/examples/spectrometer.sim-legacy.toml`.
- **Field table.** For every isotope, the magnet setting that puts it on each
  detector (`tables/<name>/current` points at the active
  `<timestamp>.toml`; each file has `[[points]]` with `isotope`, `mass` and
  one column per detector). The `argon` table shipped with Pychron is a
  **simulation placeholder** and describes no instrument.
- **Mass table.** `molecular_weights.toml` extends the built-in noble-gas
  masses.
- **Protection and saturation.** `protection.threshold` is the signal at
  which a detector counts as in danger; `on_move = true` makes Pychron
  protect it during magnet moves; `[magnet] protection` lists the detectors
  and a `beam_blank_threshold`. These are only as good as the field table.
- **Source ramping.** `[source] ramp = { HV = 200.0, TrapCurrent = 10.0 }`
  limits how fast a parameter changes **in the simulator**; the Thermo Qtegra
  driver does not implement ramping (see below).
- **Machine-local overrides.** A `spectrometer.local.toml` (or
  `spectrometer.qtegra.local.toml`) beside the config can change
  `host`, `port`, `baud`, `timeout_ms` of a transport, and driver
  `user` / `password`. Nothing else may be overridden. Keep addresses and
  passwords there, not in the shared file. Do not turn on `trace = true` for a
  transport that carries a password: the trace records the login.
- **Checks at load.** The loader reports, with file and line: unknown
  sections or keys, a `reference_detector` that is not a detector, duplicate
  detectors, an acquirer without `channels`, a detector `channel` no driver
  offers or two detectors on one channel, a field table that is missing or has
  no column for an active detector, `deflection` or `protection` without
  `[detector_control]`, `corrections.hv` without `nominal_hv`, and a `ramp`
  naming an unknown parameter.

Run the same check outside the program:

```bash
elctl exp validate experiment.toml --lab <lab dir> --spectrometer spectrometer.toml
elctl doctor --install <name>     # checks an installation, lists leftover placeholders
elctl conditionals-check <file> --spectrometer spectrometer.toml
```

## Simulated, Thermo Qtegra and Isotopx NGX

| | Simulated | Thermo Qtegra (Argus VI, Helix) | Isotopx NGX |
|---|---|---|---|
| Driver kind | `sim_integrated` (vendor-style) or `sim_dac_positioner` + `sim_adc_bank` + `sim_pulse_counter` + `sim_hv_supply` (legacy-style) | `thermo_qtegra` | `isotopx_ngx` (+ `ngx_valves` for valves) |
| Connection | `kind = "sim"` transport | TCP to Qtegra's RemoteControlServer (example port 1069) | TCP to the NGX controller (example port 1099) |
| Position axis | `dac` | `dac` | `mass` |
| Setup profile | `--sim` / "simulation" install | `argus`, `helix` | `ngx` |
| Status in this repository | Complete; what the tests and rehearsals use | **Implemented, never run against an instrument.** Tested only against a simulated wire. | **Implemented, never run against an instrument.** Needs bring-up. |

What that means in practice:

- **Simulated** is safe and complete. `pychron-ui --sim` refuses to load a
  spectrometer config that is not fully simulated (every transport `sim`, and
  every driver a `sim_*` one), so it cannot be used to rehearse against
  hardware by accident. Error: `simulation requested but <file> is not a
  simulated spectrometer: driver 'qtegra' has kind "thermo_qtegra", which is
  not a simulator`. Note that a `thermo_qtegra` driver on a simulated
  transport is refused too. The simulated beam follows the field table, so
  after you edit the table the simulated peaks move with it.
- **Thermo Qtegra.** Read `docs/dev_setup.md`, section "Running against a
  Thermo instrument", and the driver spec's bring-up checklist before
  connecting ([dev setup](../dev_setup.md);
  [driver spec](../superpowers/specs/2026-10-01-qtegra-driver-design.md)). In
  short: the example's field tables, deflection coefficients, `cdd_voltage`,
  `nominal_hv` and protection thresholds are **simulation placeholders**;
  replace them with the instrument's real calibration before any magnet move,
  because detector protection on a move is planned from the field table and a
  wrong table can leave a detector unprotected while a large beam crosses it.
  **Source ramping is not implemented**: HV and trap current change in a
  single step, so step large changes by hand. Put the Qtegra PC's address in
  `spectrometer.qtegra.local.toml`. Set `trace = true` on its transport while
  you bring it up; the wire is written to `traces/qtegra.trace` and is
  overwritten each time the program starts. Opening the spectrometer window
  starts a scan, which sends `SetIntegrationTime` if Qtegra's period differs;
  a setter that gets no reply fails with a timeout. A Qtegra that cannot be
  reached is a load failure at start-up (see the log dock).
- **NGX.** The driver exists (magnet by mass, source, acquisition, and valves
  through the same connection with a `link` transport) but has not met an
  instrument; the spec lists a bring-up checklist and open questions
  (completion mode, login, terminator). The shipped `spectrometer.ngx.toml`
  has placeholder mass table, `nominal_hv` and saturation values. Credentials
  go in `spectrometer.ngx.local.toml`, never in the shared file. The README's
  one-line hardware status still says NGX is "not implemented"; the driver
  is in the program, and the example config's header is the more accurate
  statement.

## Calibration checklist

When `pychron setup` writes an install it also writes a `CALIBRATE.md` in the
install folder listing what is still a placeholder; `elctl doctor` warns about
every file that still says `SIMULATION PLACEHOLDER` or `CONFIRM`. Before you
measure on a real instrument:

1. **Field table** (`tables/`): replace with a calibration for your
   instrument, every isotope on every detector. Do not move the magnet
   before this is done.
2. **Detectors** (`[[detectors]]`): names, kinds and isotopes are defaults
   for the instrument family; confirm them against the real hardware.
3. **Source** (`nominal_hv`), deflections, saturation and protection
   thresholds.
4. **Extraction line** (`extraction_line.toml`, `canvas.toml`): the starter
   line is a five-valve example on simulated transports; describe your
   valves, gauges and controllers (see [the extraction line](02-extraction-line.md)).
5. **Scripts** (`scripts/`): `sim_extract` and `sim_pump` are examples; write
   your own extraction and pumping scripts ([scripting](09-scripting.md)).
6. **Plans**: read the three starter plans; check the baseline `mass`,
   `peak_center` window and equilibration against your line. Run
   `detector_ic` with air to get IC factors.
7. **Peak-centre settings** (`peak_center.toml`): set `window` from the real
   peak width.

To leave simulation once the instrument is ready:

```bash
elctl init --reconfigure --install <name> --set simulation=no
```

(or run `pychron-ui --setup` again). Files you have edited are kept.

## Unverified / not yet implemented

- No peak-centre, deflection, source or baseline controls in the spectrometer
  window (by design; see the spec). I did not find any code that offers them
  there.
- **Plan features not yet supported.** The "measurement plan v2" design
  (segments, positions with per-detector deflection, several gases, baselines
  during extraction, `optics`, per-run field tables) is marked Draft. The plan
  loader today accepts only the sections shown above (`plan`, `hook`,
  `detectors`, `equilibration`, `sniff`, `peak_center`, `baseline`, `main`,
  `fits`, `conditionals`, `whiff`, `parameters`); anything else, such as
  `[[segment]]` or `[[gas]]`, is rejected as an unknown key.
- Whether the spectrometer window and a running queue can both hold the
  acquirer: the scan service refuses to start when the acquisition engine is
  already running elsewhere; I did not verify what an operator sees. Close the
  spectrometer window before starting a queue if you see a start error.
- **Hardware behaviour** of the Qtegra and NGX drivers, source ramping on
  real hardware, and the effect of `af_demag` have not been checked against an
  instrument; this page documents the configuration, not measured behaviour.
- The window's colour cell for a saturated detector depends on the config
  `saturation` value; a detector without `saturation` never shows it.
- Spec text says "Window > Spectrometer"; the real menu is **View >
  Spectrometer**.
