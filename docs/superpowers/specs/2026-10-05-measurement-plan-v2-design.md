# Measurement plan v2: segments, positions, gases

Date: 2026-10-05
Status: Draft
Owner: Jake Ross
Builds on: `2026-09-29-experiment-system-design.md` §4 (the v1 plan schema
and engine), §3 (run lifecycle and overlap), §8 (collector and
`AnalysisRecord`); `2026-10-01-qtegra-driver-design.md` and
`2026-10-03-ngx-driver-design.md` (what each positioner and acquirer can do).

Every protocol fact below is marked **[legacy]** (read from `NMGRL/pychron`
6ccadb4: measurement pyscripts, hop files, `syn_extraction.py`),
**[literature]** (published methods; abstracts and lab pages only, publisher
sites were unreachable, so verify before quoting) or **[decision]** (decided
here). Nothing has been run against an instrument.

## 1. Intent

The v1 plan is one block order: `peak_center.before -> baseline.before ->
equilibrate -> main (cycles x hops) -> baseline.after -> peak_center.after`.
It expresses static multicollection, one isotope hopped across detectors and
"four static plus a hopped fifth", which is most Ar-Ar work. It cannot express
what labs already do with legacy pyscripts:

- settings that change per position: deflection, Faraday/CDD mode, zoom and
  source presets, field table, integration time, a cup kept in the
  configuration but not collected **[legacy: `Ar36:CDD:110`, `active: false`,
  `peak_hop(mftable=)`]**;
- baselines and peak centers anywhere but before and after, baselines at
  several positions, a peak center per detector that writes a per-run field
  table **[legacy: `bs:` hops, `generate_ic_mftable`]**;
- a baseline measured while the same run's extraction is heating, with no
  baseline afterwards **[legacy: NMGRL synchronous extraction]**;
- more than one gas in a run, each with its own inlet, equilibration, time
  zero and fits **[literature: He then Ne then Ar on a Helix SFT; Xe as three
  methods per cycle on a Helix MC Plus]**;
- labels that are not isotopes: `CO2`, `HCl`, `Ar40H`, `Ar40++`, raw masses,
  pseudo-isotopes **[legacy: `argonH.yaml`, `PHHCl`]**.

v2 keeps every v1 file valid and adds one level of structure: a plan is an
ordered list of **segments**, a segment is a list of **positions** cycled N
times, and segments may be grouped under a **gas**. `[main]` with
`[[main.hops]]` desugars to one signal segment.

## 2. What instruments need

Across ARGUS VI, Helix MC Plus, Helix SFT, NGX, Noblesse and MAP 215-50, and
across Ar, He, Ne, Kr and Xe, a measurement is built from the same five
things **[literature, legacy]**:

| Thing | Varies by instrument |
|---|---|
| Position | magnet by isotope or mass (all), raw DAC (legacy MAP, `positioning.dac`), HV (legacy `position_hv`) |
| Detectors collecting there | fixed cups (ARGUS, NGX); a cup is Faraday or CDD by mode (Helix MC Plus); one of two collectors (MAP, SFT) |
| Per-detector settings tied to the position | deflection (ARGUS, Helix), protect or blank (CDD at 2000 V, Noblesse deflector A at -880 V), zoom and source preset per method (Noblesse), HR collector position (Helix) |
| Timing | integration, counts, settle; settle depends on which detectors just saw a large beam (10^13 ohm cups need about 3 s to reach under 100 ppm, NMGRL uses 20 s on them against 6 s elsewhere) |
| Series kind | signal, baseline, sniff, whiff |

Three observations fix the shape of the model:

1. The positioning reference and the collecting detectors are independent.
   `ICcycle.yaml` records Ar40 on H2 while the reference walks H2, H1, AX,
   L1; `argonH.yaml` positions by pseudo-isotope `PHHCl` and collects `HCl`
   on L2(CDD) **[legacy]**.
2. A label needs a mass, not an isotope table entry: molecules, doubly
   charged species and pseudo-isotopes all appear **[legacy, literature]**.
3. Some corrections are measured between samples (Cologne CO2++/CO2+ for Ne)
   rather than in the run **[literature]**, so interference monitors are an
   analysis type as well as a position.

## 3. Schema

### 3.1 Levels

```
plan
  gas*            implicit single gas for Ar-Ar: inlet, outlet, equilibration,
                  source_dark_phase, time_zero, fits defaults, segments
    segment*      kind, when, cycles, integration_s, time_zero, fits, conditionals,
                  positions   -- or a step: peak_center, set, blank_beam, park, delay, time_zero
      position*   collect[], at, kind, counts, settle_s, integration_s, optics, table
```

### 3.2 Position **[decision]**

| Key | Meaning |
|---|---|
| `collect` | List of `{ label, detector, active = true, deflection, protect }`, or the shorthand map `{ Ar40 = "H1", Ar36 = "CDD" }`. `active = false` keeps the cup in the configuration (deflected or protected) without collecting. A detector appears at most once per position. A label may appear on several detectors across positions, and in the list form on several detectors in one position. |
| `label` | An isotope (`Ar40`), a molecule or interference (`CO2`, `HCl`, `Ar40H`, `Ar40++`), a pseudo-isotope from the spectrometer's mass table (`PHHCl`), or an entry positioned by `at.mass` (`mass35`). |
| `at` | Positioning target, independent of `collect`: `{ label, detector }`, `{ mass, detector }`, `{ dac }` or `{ hv, detector }`. Default: the reference detector's label in `collect`; if the reference detector is not in `collect`, `at` is required (the v1 `position` rule). |
| `kind` | `signal` (default) or `baseline`. A baseline position inside a signal segment is the legacy `bs:` hop. |
| `counts` | Readings per cycle at this position. Required. |
| `settle_s` | Wait after the move. The engine uses the larger of this and the spectrometer's `settle_after_large_beam_s` for any detector whose last reading exceeded its `large_beam_threshold`. The move and settle are skipped when the position, optics, table and deflections are unchanged. |
| `integration_s` | Defaults to the segment's. |
| `optics` | Named preset from `spectrometer.toml [optics.<name>]`: Qtegra sub-cup configuration, Noblesse zoom and source tune, Helix HR collector position. Part of the position's identity in the record so IC factors match the optics they were measured under. |
| `table` | Named field table (legacy `mftable=`), including a per-run table written by an earlier `peak_center` step. |

### 3.3 Segment **[decision]**

| Key | Meaning |
|---|---|
| `kind` | `signal`, `baseline`, `sniff`, `whiff`, or a step: `peak_center`, `set`, `blank_beam`, `park`, `delay`, `time_zero`. |
| `when` | `during_extraction`, `before_inlet`, `during_equilibration`, `after_inlet` (default), `after_main`, `after_run`. Segments with the same `when` run in file order. `after_main` means after the last `after_inlet` signal segment of the gas. |
| `cycles` | Repeat count, default 1. Live-editable like counts. |
| `integration_s` | Segment default for its positions. |
| `time_zero` | `"on_inlet_close"`, `"on_first_count"` or `{ offset_s = N }`. Allowed on the first `after_inlet` signal segment of a gas, or as a `time_zero` step anywhere (legacy `set_time_zero` after an IC block). |
| `positions` | §3.2. Required for `signal`, `baseline`, `sniff`, `whiff`. |
| `fits` | Overrides keyed `label@detector`, each `{ fit, error, outliers }`. Bare labels and `default` apply to every detector of that label. |
| `conditionals` | `truncations` scoped to the segment. |
| `checks` | `whiff` only: `[{ check, action = run_remainder | pump | abort }]`, as v1. |

Steps carry one table named after the kind:

- `peak_center = { label, detector | detectors[], extra_detectors[], config, write_table }`. With `detectors[]` the job runs once per detector; `write_table` stores the result as a per-run field table under that name (legacy `generate_ic_mftable`).
- `set = { deflection = { CDD = 2000 }, gain = { H1 = 1.0 }, hv, cdd_voltage, source = { ... }, integration_s }`. Each sub-key maps to one facade call.
- `blank_beam = true | false`.
- `park = { mass | label, detector }` (legacy `warm_cdd`).
- `delay = { seconds }`.
- `time_zero = "now" | { offset_s }`.

### 3.4 Gas **[decision]**

`[[gas]]` has `name`, `inlet`, `outlet`, `equilibration` (`time_s`,
`inlet_delay_s`, `close_inlet`), optional `source_dark_phase = true` (hold
the source below ionisation during inlet and switch on at time zero, Thermo
EST **[literature]**), `time_zero`, `fits` defaults, and `[[gas.segment]]`.
Gases run in file order. A plan without `[[gas]]` has one implicit gas built
from the top-level `[equilibration]`, `[fits]` and segments. A detector's
deflection, integration and optics are snapshotted per gas, and per position
where a position overrides them.

### 3.5 Baseline table

`[baseline] before / after / counts / mass / detector / settle_s /
integration_s` stay as sugar (§4.1). New:

```toml
[baseline.modifier.CDD]
variable = "Ar40+Ar39"          # expression over this run's intercepts
model = "baseline_model.csv"    # WLS of baseline vs variable, or an expression in x
error = "counting_statistics"   # optional
```

applied at reduction, stored in the record **[legacy: `baseline_modifiers`]**.

### 3.6 Spectrometer config additions **[decision]**

In `spectrometer.toml`, not in plans:

- logical detectors with `mode`, so `H2` and `H2(CDD)` are two detectors
  sharing one collector, with `mode_switch_s`;
- per detector `large_beam_threshold` and `settle_after_large_beam_s`;
- `[optics.<name>]` presets, opaque to the plan, applied by the driver;
- a mass table with molecules, doubly charged species and pseudo-isotopes
  (`PHHCl = 36.46`, `Ar40++ = 19.98`, `CO2 = 43.99`);
- `[interferences]` relations for reduction metadata: `Ar40++ corrects Ne20`,
  `CO2 corrects Ne22`, `HD corrects He3`.

### 3.7 Validation (additions to v1)

- a detector at most once per position; `active = false` entries count;
- every label resolves to a mass through the mass table, or the position has
  `at.mass`;
- `at` resolves; `optics` and `table` names exist, a `table` written by a
  `peak_center` step is defined before any position that uses it;
- `deflection` only on detectors whose driver supports it; `optics` only on
  instrument families that declare presets;
- a `during_extraction` segment only in plans whose analysis types have an
  extraction, and no `signal` segment there;
- `time_zero` at most once per gas unless as a step;
- `[main]` and `[[segment]]` may not both appear at top level; `[[gas]]` and
  top-level segments may not both appear;
- unknown keys at any level remain errors.

## 4. Desugaring and compatibility

### 4.1 v1 to v2 **[decision]**

The parser rewrites v1 tables into segments before validation, so every
existing fixture and template parses and the engine has one input form:

| v1 | v2 |
|---|---|
| `[main]` + `[[main.hops]]` | one `signal` segment, `when = "after_inlet"`, `cycles`, `integration_s`, `time_zero`; each hop a position (`positions` map becomes `collect`; `protect` becomes `protect = true` on those entries, adding `active = false` entries for protected detectors not in the map; `baseline = true, mass` becomes `kind = "baseline", at = { mass, detector }`) |
| `[baseline] before` | a `baseline` segment at `before_inlet`, one position collecting every detector that appears in any signal position, `at = { mass, detector }` |
| `[baseline] after` | the same at `after_main` |
| `[peak_center] before / after` | `peak_center` steps at `before_inlet` / `after_run` |
| `[sniff]` | a `sniff` segment at `during_equilibration` on the first signal position's detectors |
| `[whiff]` | a `whiff` segment at `after_inlet`, before the signal segment |
| `[fits] signal.Ar40` | `Ar40` applies to every detector of Ar40; `[fits] baseline.H1` applies to every baseline series on H1 |
| `[detectors] exclude` | removed from every `collect`; a position left empty is an error |
| `parameters.expose` paths | `main.hops[i].counts` keeps working through a path alias table; new plans use `segment[i].positions[j].counts` |

Property test: a v1 plan and its hand-written v2 equivalent produce the same
resolved plan, the same duration and the same engine trace in the simulator.

### 4.2 Legacy import **[decision]**

Extend the converter pattern of `tools/pychron_conditionals_import.py`:

- `hops.txt` tuples and hops YAML -> positions (`Iso:Det:Defl` -> entry with
  `deflection`; `bs:Mass:Det` -> `kind = "baseline"`; `active`, `protect`,
  `positioning.dac` -> `at = { dac }`);
- the `mx` docstring of a measurement script -> `[equilibration]`,
  baseline and peak-center segments, counts and integration;
- a script that calls `multicollect` or `peak_hop` more than once -> several
  segments; `set_time_zero`, `set_integration_time`, `set_deflection`,
  `generate_ic_mftable` between calls -> steps;
- `syn_extraction/<name>.yaml` -> a `baseline` segment at
  `during_extraction` plus `[baseline.modifier]`;
- anything not mapped -> a hook stub with the original command in a comment,
  and a line in the conversion report.

## 5. Engine

### 5.1 Phases **[decision]**

The engine stops being a fixed block order. `resolve(plan)` yields a flat
list of segments per gas, grouped by `when`. The executor calls
`engine.run_phase(when, ctx, token)` at the matching point of the run:

```
Extract ───────────────────────────────┐ during_extraction (spectrometer claimed at run start)
Measure: before_inlet
         open inlet ─ during_equilibration ─ close inlet / time zero
         after_inlet  (signal, whiff first)
         after_main
         after_run
```

For a multi-gas plan the inlet block repeats per gas; `before_inlet`
segments of gas N run after `after_main` of gas N-1.

### 5.2 Position execution

Per position, in order: apply `table`; apply `optics`; set deflections and
protect/blank for entries that carry them (and `ProtectPolicy::Auto` for the
move, as today); position by label, mass, DAC or HV; wait
`max(settle_s, large-beam settle)`; set integration if it differs; collect
`counts` readings on the active entries; restore deflections that this
position changed if the next position does not set them (legacy reset rule
**[legacy: `peak_hop_collector.py`]**). Move, optics, table and settle are
skipped when nothing changed, which preserves the v1 property that one
position times N cycles equals one position with N times counts.

### 5.3 Ports

`measurement/ports.hpp` gains `apply_optics(name)`, `with_table(name)`,
`set_deflection(det, v)`, `set_integration(s)`, `move_native(dac)`,
`position_hv(v, det)`, `blank_beam(bool)`, `set_source_output(bool)`,
`set_gain`, `set_hv`, `set_param`, and `peak_center` with `write_table`.
Every one maps to a call that already exists on the spectrometer facade
except `apply_optics` and `set_source_output`, which the Qtegra and Noblesse
drivers add. The simulator applies deflection and optics to the simulated
beams so tests can observe them; a driver without a capability reports it at
plan load (`doctor` and `exp validate`), not at run time.

### 5.4 `during_extraction` and the executor **[decision]**

A run whose plan has a `during_extraction` segment claims the spectrometer
resource when its extraction starts, not when its measurement starts, and
releases it as today. The executor runs the extraction script and
`run_phase(during_extraction)` concurrently under one cancel token; the
measurement phase must finish before `before_inlet` begins, and the executor
waits for it if the extraction is shorter. Such a run cannot overlap with
the previous run's measurement; the existing resource model expresses this
once the claim point moves. The run record marks the phase so the reduction
knows the baseline predates the gas.

### 5.5 Hooks

Hooks keep their three points and gain `peak_center`, `set_deflection`,
`set_integration`, `with_table` and `apply_optics`, so the escape hatch is at
least as capable as the plan.

## 6. Collector, record, reduction

### 6.1 Keys **[decision]**

- `SeriesKey` becomes `(label, detector, kind, gas, segment index, position
  index)`; `kind` is an open string set (`signal`, `baseline`, `sniff`,
  `whiff`, `peak_center`), not a closed enum.
- `Results.intercepts` is keyed `label@detector` always (no collision-only
  suffix). `Collector::intercept("Ar40")` keeps returning the most recently
  measured detector so existing conditionals work; `Ar40@H1` selects one.
- `Results.baselines` is keyed `(detector, segment index, position index)`
  and records `when`, the magnet position and the deflection in force.
- `time_zero` per gas; `SpectrometerRec` per gas, with per-position
  deflection, integration and optics on the series that used them.
- `Results.baseline_modifiers` is added so ingest's `baseline_modifiers_json`
  has a producer.

This is `AnalysisRecord` v3; the v2 reader stays and v2 records load with a
single implicit gas and segment.

### 6.2 Reduction

- An intercept for one label on several detectors feeds the IC-factor
  machinery directly: `reference_fit.hpp` gains a same-run source, and the
  IC factor is stored with the `optics` it was measured under.
- With several baselines per detector the reduction picks by rule: the
  baseline position whose `at` matches, else the nearest in time; the rule
  is recorded on the baseline value.
- `[interferences]` drives the `Ar40++` and `CO2` corrections for Ne and
  the hydride and `HCl` monitors for Ar as reduction metadata; the
  calculations themselves are a separate spec.

### 6.3 Conditionals

Metric names gain an optional detector qualifier (`Ar40@H1.cur`); bare names
keep the v1 meaning. `cycles` joins `counts` as a live-editable target.

## 7. Worked plans

Full plans minus `[plan]`, `[equilibration]`, `[conditionals]`.

### 7.1 NMGRL ARGUS static multicollection with synchronous-extraction baseline **[legacy]**

```toml
[detectors]
reference = "H1"

[[segment]]
kind = "baseline"
when = "during_extraction"
integration_s = 1
[[segment.positions]]
collect = { Ar40 = "H1", Ar39 = "AX", Ar38 = "L1", Ar37 = "L2", Ar36 = "CDD" }
at = { mass = 34.2, detector = "H1" }
counts = 120
settle_s = 20

[[segment]]
kind = "signal"
integration_s = 1
time_zero = "on_inlet_close"
[[segment.positions]]
collect = { Ar40 = "H1", Ar39 = "AX", Ar38 = "L1", Ar37 = "L2", Ar36 = "CDD" }
counts = 400

[[segment]]
kind = "peak_center"
when = "after_run"
peak_center = { label = "Ar40", detector = "H1", extra_detectors = ["AX", "L2", "CDD"] }

[baseline.modifier.CDD]
variable = "Ar40+Ar39"
model = "baseline_model.csv"
error = "counting_statistics"
```

### 7.2 Four static, hop a fifth, Cl monitor at mass 35, per-detector deflection **[legacy: `hops with mass 35.txt`]**

```toml
[[segment]]
kind = "signal"
cycles = 20
integration_s = 1
[[segment.positions]]
collect = [
  { label = "Ar40", detector = "H1" }, { label = "Ar41", detector = "H2" },
  { label = "Ar38", detector = "L1" }, { label = "Ar37", detector = "L2" },
  { label = "Ar36", detector = "CDD", deflection = 110 } ]
counts = 15
settle_s = 6
[[segment.positions]]
collect = { Ar39 = "CDD" }
counts = 15
settle_s = 5
[[segment.positions]]
collect = { mass35 = "CDD" }
at = { mass = 35.0, detector = "CDD" }
counts = 8
settle_s = 3

[[segment]]
kind = "baseline"
when = "after_main"
[[segment.positions]]
collect = { Ar40 = "H1", Ar41 = "H2", Ar38 = "L1", Ar37 = "L2", Ar36 = "CDD" }
at = { mass = 34.2, detector = "H1" }
counts = 120
settle_s = 20
```

### 7.3 Detector intercalibration with a per-run field table **[legacy: `jan_ic_peak_hop3.py`, `ic_hops.txt`]**

```toml
[[segment]]
kind = "set"
when = "before_inlet"
set = { deflection = { CDD = 2000 } }

[[segment]]
kind = "peak_center"
when = "before_inlet"
peak_center = { label = "Ar40", detectors = ["H1", "AX", "L2"], write_table = "ic_run" }

[[segment]]
kind = "signal"
cycles = 6
integration_s = 1
table = "ic_run"
[[segment.positions]]
collect = { Ar40 = "H2" }
counts = 10
settle_s = 20            # 10^13 ohm cup
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 10
settle_s = 6
[[segment.positions]]
collect = { Ar40 = "AX" }
counts = 10
settle_s = 6
[[segment.positions]]
collect = { Ar40 = "L1" }
counts = 10
settle_s = 20
[[segment.positions]]
collect = { Ar40 = "L2" }
counts = 10
settle_s = 6

[[segment]]
kind = "time_zero"
time_zero = "now"

[[segment]]
kind = "baseline"
when = "after_main"
[[segment.positions]]
collect = [ { label = "Ar40", detector = "H2" }, { label = "Ar40", detector = "H1" },
            { label = "Ar40", detector = "AX" }, { label = "Ar40", detector = "L1" },
            { label = "Ar40", detector = "L2" } ]
at = { mass = 39.5, detector = "H1" }
counts = 60
settle_s = 20
```

The record holds `Ar40@H2`, `Ar40@H1`, ... as separate intercepts.

### 7.4 Helix: argon with hydride and HCl monitors, an inactive protected cup **[legacy: `argonH.yaml`]**

```toml
[detectors]
reference = "H2"

[[segment]]
kind = "signal"
cycles = 10
[[segment.positions]]
collect = { Ar40 = "H2" }
counts = 10
settle_s = 3
[[segment.positions]]
collect = [
  { label = "Ar40", detector = "H2(CDD)", active = false, protect = true, deflection = 3250 },
  { label = "Ar39", detector = "H1(CDD)", deflection = 200 },
  { label = "Ar38", detector = "AX(CDD)" },
  { label = "Ar37", detector = "L1(CDD)", deflection = 600 },
  { label = "Ar36", detector = "L2(CDD)" } ]
at = { label = "Ar38", detector = "AX(CDD)" }
counts = 10
settle_s = 3
[[segment.positions]]
collect = { Ar40H = "H2(CDD)", Ar39H = "H1(CDD)", Ar38H = "AX(CDD)", Ar37H = "L1(CDD)", Ar36H = "L2(CDD)" }
counts = 5
settle_s = 3
[[segment.positions]]
collect = { HCl = "L2(CDD)" }
at = { label = "PHHCl", detector = "L2(CDD)" }
counts = 5
settle_s = 3
```

### 7.5 Noblesse IC/Faraday split with per-method optics **[literature]**

```toml
[[segment]]
kind = "signal"
cycles = 15
[[segment.positions]]
collect = { Ar40 = "F0", Ar38 = "IC1", Ar36 = "IC2" }
optics = "ar_config_a"        # zoom, source tune, deflector A = -880 V
counts = 10
settle_s = 5
integration_s = 1
[[segment.positions]]
collect = { Ar39 = "IC0", Ar37 = "IC1" }
optics = "ar_config_b"
counts = 10
settle_s = 5
integration_s = 2
```

### 7.6 Helix SFT suite: He, then Ne, then Ar **[literature]**

```toml
[[gas]]
name = "He"
inlet = "@valves.he_inlet"
outlet = "@valves.outlet"
equilibration = { time_s = 20 }
time_zero = "on_inlet_close"
[[gas.segment]]
kind = "signal"
integration_s = 1
[[gas.segment.positions]]
collect = { He4 = "Faraday", He3 = "SEM" }
counts = 200
[[gas.segment]]
kind = "baseline"
when = "after_main"
[[gas.segment.positions]]
collect = { He4 = "Faraday", He3 = "SEM" }
at = { mass = 3.5, detector = "SEM" }
counts = 30

[[gas]]
name = "Ne"
inlet = "@valves.ne_inlet"
outlet = "@valves.outlet"
equilibration = { time_s = 30 }
[[gas.segment]]
kind = "signal"
cycles = 10
integration_s = 2
[[gas.segment.positions]]
collect = { Ne20 = "SEM" }
counts = 5
settle_s = 4
[[gas.segment.positions]]
collect = { Ne21 = "SEM" }
counts = 8
settle_s = 4
[[gas.segment.positions]]
collect = { Ne22 = "SEM" }
counts = 5
settle_s = 4
[[gas.segment.positions]]
collect = { Ar40 = "SEM" }     # Ar40++ correction on Ne20
counts = 3
settle_s = 4
[[gas.segment.positions]]
collect = { CO2 = "SEM" }      # mass 44, CO2++ on Ne22
counts = 3
settle_s = 4

[[gas]]
name = "Ar"
inlet = "@valves.ar_inlet"
outlet = "@valves.outlet"
[[gas.segment]]
kind = "signal"
cycles = 8
[[gas.segment.positions]]
collect = { Ar40 = "Faraday" }
counts = 5
[[gas.segment.positions]]
collect = { Ar39 = "SEM" }
counts = 5
[[gas.segment.positions]]
collect = { Ar38 = "SEM" }
counts = 5
[[gas.segment.positions]]
collect = { Ar37 = "SEM" }
counts = 8
[[gas.segment.positions]]
collect = { Ar36 = "SEM" }
counts = 8
```

### 7.7 Helix MC Plus xenon: three positions per cycle **[literature]**

```toml
[[segment]]
kind = "signal"
cycles = 12
integration_s = 4
[[segment.positions]]
collect = { Xe136 = "H2", Xe134 = "H1", Xe132 = "AX", Xe131 = "L1", Xe130 = "L2" }
counts = 5
settle_s = 8
[[segment.positions]]
collect = { Xe132 = "H2", Xe131 = "H1", Xe130 = "AX", Xe129 = "L1", Xe128 = "L2" }
counts = 5
settle_s = 8
[[segment.positions]]
collect = { Xe129 = "H1", Xe128 = "AX", Xe126 = "L1", Xe124 = "L2" }
counts = 8
settle_s = 8

[[segment]]
kind = "baseline"
when = "before_inlet"
[[segment.positions]]
collect = { Xe136 = "H2", Xe134 = "H1", Xe132 = "AX", Xe131 = "L1", Xe130 = "L2" }
at = { mass = 133.5, detector = "AX" }
counts = 30
```

`Xe132@AX` and `Xe132@H2` stay apart in the record; combining them is the
reduction's job and is what the IC-factor machinery needs.

## 8. Testing

- Every v1 fixture under `tests/experiment/fixtures/plans/valid/` parses
  unchanged and resolves to the documented segments (golden resolved-plan
  JSON per fixture).
- The seven worked plans above become fixtures; each runs in the simulator
  with a `ManualClock` and asserts the engine trace (positions, deflections,
  optics, settles, counts, kinds) and the record keys.
- Equivalence: one position times N cycles equals one position with N times
  counts (kept); a v1 plan equals its v2 spelling.
- `during_extraction`: an integration test with a simulated laser and
  spectrometer asserts the baseline series ends before `before_inlet`, the
  spectrometer is claimed for the run's whole life, and the next run's
  extraction overlap still works for plans without that phase.
- Large-beam settle: a position after a saturating Ar40 on a 10^13 ohm cup
  waits the configured settle even when `settle_s` is shorter.
- Converter: each legacy example under `docs/user_guide/operation/scripts/
  examples/{argus,helix}/measurement/` in `NMGRL/pychron` converts, and the
  conversion report lists what became a hook stub.

## 9. Order of work

Each item is a spec unit of its own.

1. Segment and position structs; parser desugaring of v1; validation; v1
   fixtures pass unchanged; resolved-plan goldens.
2. Per-position settings on the engine and ports (deflection, protect and
   active, integration, table, DAC and HV positioning); simulator support.
3. `AnalysisRecord` v3 and collector keys; fits by `label@detector`;
   conditional metric qualifier.
4. Steps: `peak_center` with `write_table`, `set`, `blank_beam`, `park`,
   `delay`, `time_zero`.
5. `when` phases; executor change for `during_extraction`; baseline
   modifier in the record.
6. Gases: per-gas inlet, time zero, snapshot; `source_dark_phase`.
7. `optics` presets and `[interferences]` in the spectrometer config;
   Qtegra and Noblesse driver support.
8. Legacy converter and conversion report.

## 10. Open decisions

- Whether `active = false` entries should also be allowed to carry a
  `label` that is not positioned (today they need one for the mass table
  check); a bare `{ detector, protect = true }` form may be clearer.
- Whether the large-beam settle should also apply after a baseline position
  (the beam was off-peak, so probably not) and whether the threshold is per
  amplifier type rather than per detector.
- Reduction rule for choosing among several baselines per detector when
  neither `at` matches nor time is decisive (two baselines bracket the
  signal): bracketing average, or both kept and the reducer chooses.
- Whether a `during_extraction` segment may also be `sniff` kind to watch
  for leaks while heating.
- Record size: per-position deflection and optics on every series is
  verbose; a per-gas table of states with a reference per series is the
  alternative.
