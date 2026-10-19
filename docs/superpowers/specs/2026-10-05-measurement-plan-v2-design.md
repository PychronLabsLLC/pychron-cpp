# Measurement plan v2: segments, positions, gases

Date: 2026-10-05
Status: Agreed 2026-10-15 (owner settled section 10; staged gas release folded in as section 11, item 6b)
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
| `collect` | List of `{ label, detector, active = true, deflection, protect }`, or the shorthand map `{ Ar40 = "H1", Ar36 = "CDD" }`. Shorthand read in key order (toml++ iterates a table sorted), not order written; list form keeps order written. `active = false` keeps the cup in the configuration (deflected or protected) without collecting. A detector appears at most once per position. A label may appear on several detectors across positions, and in the list form on several detectors in one position. |
| `label` | An isotope (`Ar40`), a molecule or interference (`CO2`, `HCl`, `Ar40H`, `Ar40++`), a pseudo-isotope from the spectrometer's mass table (`PHHCl`), or an entry positioned by `at.mass` (`mass35`). |
| `at` | Positioning target, independent of `collect`: `{ label, detector }`, `{ mass, detector }`, `{ dac }` or `{ hv, detector }`. Default: the reference detector's label in `collect`; if the reference detector is not in `collect`, `at` is required (the v1 `position` rule). |
| `kind` | `signal` (default) or `baseline`. A baseline position inside a signal segment is the legacy `bs:` hop. |
| `counts` | Readings per cycle at this position. Required. |
| `settle_s` | Wait after the move. The engine uses the larger of this and the spectrometer's `settle_after_large_beam_s` for any detector whose last signal reading exceeded its
`large_beam_threshold`. That includes the move out of a baseline position:
a baseline reading does not clear the detector's large-beam state (owner,
2026-10-15). Threshold and settle are per detector, not per amplifier
type. The move and settle are skipped when the position, optics, table and deflections are unchanged. |
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
from the top-level `[equilibration]`, `[fits]` and segments. Equilibration
values (`inlet`, `outlet`, `time_s`, `inlet_delay_s`, `close_inlet`) a gas
does not give are top-level `[equilibration]`'s; gas `fits` overrides plan's
`[fits]` key by key, none: plan's; `[main]` and `[[gas]]` may not both
appear (decided 2026-10-18, Task 2). A detector's
deflection, integration and optics are snapshotted per gas, and per position
where a position overrides them.

### 3.5 Baseline table

`[baseline] before / after / counts / mass / detector / settle_s /
integration_s` stay as sugar (§4.1). Sugar for `[main]` only: in a plan of
`[[segment]]` or `[[gas]]`, `[baseline] before`/`after`, `[peak_center]
before`/`after`, `[sniff] enabled` and `[whiff] enabled` refused (`only with
[main]; in a plan of segments write a <kind> segment`); segment written out,
so its place among others is the file's (decided 2026-10-18, Task 2). New:

```toml
[baseline.modifier.CDD]
variable = "Ar40+Ar39"          # expression over this run's intercepts
model = "baseline_model.csv"    # WLS of baseline vs variable, or an expression in x
error = "counting_statistics"   # optional
```

applied at reduction, stored in the record **[legacy: `baseline_modifiers`]**. As built (Task 21):
variable reads intercept names only (`Ar40`: label's first detector;
`Ar40@H1`), model `x` only, checked at load. CSV model: weighted
least-squares line through `x, baseline[, error]` points (1/error²; equal
weights and scatter's variance without errors), value and standard error at
run's variable; CSV read from plan library dir by run. Expression model:
value, error 0. `error` is an error model's name, kept for reduction.

### 3.6 Spectrometer config additions **[decision]**

In `spectrometer.toml`, not in plans:

- logical detectors with `mode`, so `H2` and `H2(CDD)` are two detectors
  sharing one collector, with `mode_switch_s`;
- per detector `large_beam_threshold` and `settle_after_large_beam_s`;

  Decided 2026-10-18, Task 9: keys under `[[detectors]]`: `large_beam_threshold`
  (> 0, unset: never), `settle_after_large_beam_s`, `collector` (empty: own
  collector), `mode`, `mode_switch_s` (each >= 0; negative is config error
  naming detector). Engine gets them as `EngineContext::timing`
  (`detector_timing()`, `adapters.hpp`). State on engine for whole
  measurement, across segments and gases:
  - large beam: every non-baseline reading (signal, sniff, whiff) sets or
    clears each detector's state against its threshold, all detectors of
    the reading, collected or not; baseline reading leaves it;
  - position settles `max(settle_s, settle_after_large_beam_s` of each of
    its entries' detectors, `active = false` included, in large-beam state`)`;
  - mode: engine remembers detector last used on each collector. Entry on
    other detector of same collector is mode change: settle at least larger
    `mode_switch_s` of the two, and settle taken even when move skipped.
    First use of a collector in a measurement: mode unknown, no wait;
  - hook's `position_magnet` (settle 0) untouched.
- `[optics.<name>]` presets, opaque to the plan, applied by the driver;
- a mass table with molecules, doubly charged species and pseudo-isotopes
  (`PHHCl = 36.46`, `Ar40++ = 19.98`, `CO2 = 43.99`);
- `[interferences]` relations for reduction metadata: `Ar40++ corrects Ne20`,
  `CO2 corrects Ne22`, `HD corrects He3`.

### 3.7 Validation (additions to v1)

- a detector at most once per position; `active = false` entries count;
- an `active = false` entry carries a `label` like any other (mass table
  check); no bare `{ detector, protect = true }` form (owner, 2026-10-15);
- every label resolves to a mass through the mass table, or the position has
  `at.mass`;
- `at` resolves; `optics` and `table` names exist, a `table` written by a
  `peak_center` step is defined before any position that uses it;
- `at` default checked only when plan names `[detectors] reference`; none
  named: spectrometer's reference positions it, not checked at load. Entry
  on a mode of reference (`H2(CDD)` for `H2`) counts as reference
  (decided 2026-10-18, Task 3). None named, at run (decided 2026-10-18,
  Task 10): position collecting on spectrometer's `reference_detector` (or
  a mode of it) is positioned by that entry; other position by its first
  active entry, as legacy hop's first `isotope:detector`
  (`plan::position_target`, `EngineContext::reference`). Worked plan 7.2
  runs so: position 2 is `Ar39@CDD`;
- v1 file: only label masses of its signal hops and peak center isotope
  checked; inactive entry desugar adds for a protected detector with no
  configured isotope is not, so v1 file valid today stays valid (decided
  2026-10-18, Task 3);
- `deflection` only on detectors whose driver supports it; `optics` only on
  instrument families that declare presets;
- a `during_extraction` segment only in plans whose analysis types have an
  extraction, and no `signal` or `sniff` segment there (owner, 2026-10-15:
  no leak watch while heating);
- `time_zero` at most once per gas unless as a step;
- `[main]` and `[[segment]]` may not both appear at top level; `[[gas]]` and
  top-level segments may not both appear;
- no two gases collect same `label@detector` with same `kind`: store keeps
  one series of a kind per isotope per detector, so such a run would measure
  and then not save (against 11.6). Refused at load: `gas '<a>' and gas
  '<b>' both collect <kind> <label>@<detector>: the store keeps one`. Other
  kind, or same label on other detector, allowed;
- unknown keys at any level remain errors.

## 4. Desugaring and compatibility

### 4.1 v1 to v2 **[decision]**

The parser rewrites v1 tables into segments before validation, so every
existing fixture and template parses and the engine has one input form:

| v1 | v2 |
|---|---|
| `[main]` + `[[main.hops]]` | one `signal` segment, `when = "after_inlet"`, `cycles`, `integration_s`, `time_zero`; each hop a position (`positions` map becomes `collect`; `protect` becomes `protect = true` on those entries, adding `active = false` entries for protected detectors not in the map; `baseline = true, mass` becomes `kind = "baseline", at = { mass, detector }`) |
| `[baseline] before` | a `baseline` segment at `before_inlet`, one position collecting every detector that appears in any signal position, `at = { mass, detector }`. No `mass`: no `at`, §3.2 default positions it (v1 engine collected where magnet was; decided 2026-10-18, Task 6) |
| `[baseline] after` | the same at `after_main` |
| `[peak_center] before / after` | `peak_center` steps at `before_inlet` / `after_run` |
| `[peak_center] min_signal` | `min_signal` of the `after_run` step: step skipped when mean of last 10 signal readings of its isotope on its detector is below it; a step with no such readings (`before_inlet`) runs |
| `[sniff]` | a `sniff` segment at `during_equilibration` on the first signal position's detectors |
| `[whiff]` | a `whiff` segment at `after_inlet`, before the signal segment. Sniff and whiff read where magnet is: no move, no settle; `at` on them refused until phases land (Task 6) |
| `[fits] signal.Ar40` | `Ar40` applies to every detector of Ar40; `[fits] baseline.H1` applies to every baseline series on H1 |
| `[detectors] exclude` | removed from every `collect`; entry that protects its detector stays as `active = false, protect = true`, so cup stays protected during move, as v1 `protect` did (decided 2026-10-18, Task 6); a position left with nothing to collect is an error |
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

As built (Task 19): `during_equilibration` segments other than sniff run
once inlet is open (after whiff), in file order; inlet closes on time,
checked after each reading and before each position; segment then read to
its end. Sniff starts after them when there are any. `before_main` hook
after equilibration, `after_main` hook after `after_inlet` phase; first
`after_inlet` signal segment is main, others (any kind but whiff) in file
order around it.

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

Decided 2026-10-18, Task 8:
- next position is cyclic within segment; at segment end every deflection
  engine still holds is restored, so none outlives its segment (baseline
  after main sees detector as configured). Cleanup restores too, on any exit;
- deflection restored to value read (`deflection(det)`) before engine first
  changed it; a restore counts as a change: next position settles even when
  its move is skipped;
- table: position's, else segment's, else `""` (configured current);
  selected only when it differs from one in effect. New table or new optics
  forces move again (same label sits at another DAC);
- `optics` empty means none asked: preset in effect stays;
- integration: acquisition is started for every collection anyway; trace
  `integration` only where it differs from previous collection's (first
  collection of measurement has nothing to differ from);
- protect around move, as v1: unprotected after settle, before collect, also
  for `active = false` entry;
- sniff, whiff read in place: no per-position settings; their deflection,
  table, `integration_s`, inactive entry stay refused by gate. `optics`
  refused everywhere until phase 8.

Decided 2026-10-19, Tasks 16-17:
- `peak_center` with `write_table`: job puts center into run's own copy of
  current table (made at first such job, that detector's column only, never
  saved), forgotten when run ends. Failed job: positions naming that table
  run on current table, run notes `table <name> not written: <message>`
  once.
- `set` calls in fixed order (sub-keys are maps): deflection by detector,
  gain, source, hv, cdd_voltage, integration_s. Its deflection stands until
  run ends (cleanup restores), position's own deflection of that detector
  goes back to it; `integration_s` holds for gas's later collections whose
  position names none. Beam blanked by `blank_beam` unblanked on every exit.
  `time_zero` step sets time zero; gas's policy does not set it again.

### 5.3 Ports

`measurement/ports.hpp` gains `apply_optics(name)`, `with_table(name)`,
`set_deflection(det, v)`, `set_integration(s)`, `move_native(dac)`,
`position_hv(v, det)`, `blank_beam(bool)`, `set_source_output(bool)`,
`set_gain`, `set_hv`, `set_param`, and `peak_center` with `write_table`.
Every one maps to a call on the spectrometer facade except `apply_optics`
and `set_source_output`, which the Qtegra and Noblesse drivers add. Facade
gained `blank_beam(bool)` and `read_deflection(det)` (current deflection, for
the restore rule) for this. Integration stays an argument of starting
acquisition: setting it is the engine restarting acquisition. The simulator applies deflection and optics to the simulated
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

As built (Task 20): `Run::extract` claims spectrometer before extraction
line, builds engine, runs `run_phase(during_extraction)` on a second thread
beside script. Phase has own token: run's cancel or abort forwarded to it,
and a failed extraction cancels it (run then fails as extraction failure,
not cancelled). Spectrometer released in `extract` when extraction fails or
run is stopped; otherwise `measure` uses that claim and releases it as
today. A run whose plan has such a phase and no extraction script fails at
prepare. `PlanResolvers::has_extraction` (lab's plan library): a type
`rules_for` gives an extraction.

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
- `time_zero` per gas; `SpectrometerRec` per gas holds a table of the
  states used in that gas (deflections, integration, optics), each distinct
  state once; each series refers to its state by index. Record size then
  grows with distinct states, not with series. (Owner left to the author,
  2026-10-15; chosen over the state copied onto every series.)
- `Results.baseline_modifiers` is added so ingest's `baseline_modifiers_json`
  has a producer.

This is `AnalysisRecord` v3; the v2 reader stays and v2 records load with a
single implicit gas and segment. v2 record read is written back, and its sha
computed, in v2 layout. Record builder finalizes as v2 a record v2 can say all
of (no gas but implicit one, every series at segment and position 0, one
baseline position a detector, no baseline choice or modifier): ordinary run
keeps bytes and stored items it had before v3, as `instrument.simulated` and
`provenance.status` did (`RecordIngest.AnOrdinaryRunConvertsAsItAlwaysDid`).

### 6.2 Reduction

- An intercept for one label on several detectors feeds the IC-factor
  machinery directly: `reference_fit.hpp` gains a same-run source, and the
  IC factor is stored with the `optics` it was measured under.
- With several baselines per detector the reduction picks by rule: the
  baseline position whose `at` matches, else the nearest in time; when two
  baselines bracket the signal and neither is decisive, their average
  (owner, 2026-10-15), errors propagated as a mean of two. The rule used
  (`at`, `nearest`, `bracketing_average`) is recorded on the baseline
  value. As built (`measurement::fit_results`): candidates are detector's
  baseline positions in signal's gas (all of detector's when none there);
  those whose `at` equals signal's first position's target if any, else
  all; two candidates bracketing signal (latest ending before its first
  point, earliest starting after its last) give average, error
  `sqrt(e1² + e2²) / 2`, rule `bracketing_average`; else nearest by mean
  time, rule `at` when candidates matched, else `nearest`. Store keeps
  detector's choice for its first signal (by label); every position and
  choice in row's `extra` when detector has more than one position.
- `[interferences]` drives the `Ar40++` and `CO2` corrections for Ne and
  the hydride and `HCl` monitors for Ar as reduction metadata; the
  calculations themselves are a separate spec.

### 6.3 Conditionals

Metric names gain an optional detector qualifier (`Ar40@H1.cur`, either side
of a ratio: `Ar40@H1/Ar39@CDD`); bare names keep the v1 meaning. `cycles` joins `counts` as a live-editable target.

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
   6b. Staged gas release (section 11): `prepare` and `pump_out` on a gas.
7. `optics` presets and `[interferences]` in the spectrometer config;
   Qtegra and Noblesse driver support.
8. Legacy converter and conversion report.

## 10. Decisions (owner, 2026-10-15)

- `active = false` entries keep a `label`; no bare form (3.7).
- Large-beam settle applies after a baseline position too; threshold per
  detector (3.2 `settle_s`).
- Two bracketing baselines with nothing decisive: their average (6.2).
- No `sniff` segment `during_extraction` (3.7).
- Record: per-gas table of states, series refer to it (6.1).

## 11. Staged gas release (item 6b)

Folded in 2026-10-15 from `2026-10-13-staged-gas-release-design.md` (owner).
Task T027.

### 11.1 Problem

Owner's case: extract gas from sample; freeze noble gases on cryotrap; warm
cryo to release one species, equilibrate with mass spectrometer, measure;
warm more, release next species, measure it on the quadrupole; and so on.
One run, several gas fractions; between fractions the extraction line does
work (cryo setpoint, wait for temperature, getter exposure, pump out the
spectrometer) that depends on which fraction comes next.

Section 3.4 gives several gases in one run, each with own inlet and time
zero. Not given there: what the line does between gases (gas names an
inlet and an outlet valve, nothing else; release has no place but a Python
hook), and a gas measured on another spectrometer.

Built today (v1): one extraction script, then one plan with one inlet.
`cryo_temp` is a run variable handed to the extraction script
(`docs/user/04-experiments.md:160`); script vocabulary has `set_cryo`,
`get_cryo_temp` (experiment spec; host implementation checked by the plan).

Goal: owner's case on one spectrometer as one plan file, no Python,
validated before the queue starts, every fraction's data in one analysis
record under its gas.

### 11.2 `prepare` on a gas

`[[gas]]` gains optional `prepare`: an extraction script (`scripts/extraction`,
same resolution as a run's `extraction.script`) run after the previous gas's
last segment and pump-out, before this gas's inlet opens.

```toml
[[gas]]
name = "He"
prepare = { script = "cryo:release", variables = { cryo_temp = 35, hold_s = 300 } }
inlet = "@valves.inlet"
...
[[gas]]
name = "Ne"
prepare = { script = "cryo:release", variables = { cryo_temp = 75, hold_s = 300 } }
```

- A script, not a declarative `release = { ... }` table (owner, 2026-10-15):
  the steps differ by line. Same script kind, host and cancel token as the
  run's extraction script: valves, waits, `set_cryo`, under the line's
  clock rules.
- `variables`: table handed to the script, over the run's own extraction
  variables. Plan parameters (`parameters.expose`) reach them, so a run in
  a queue can change a release temperature without a new plan.
- First gas's `prepare` runs after the run's extraction script (extract and
  trap), before first inlet. A plan whose gases have no `prepare` behaves as
  without this section.
- Failure of a `prepare`: run ends as an extraction failure does today;
  data of gases already measured is kept and saved.

### 11.3 `pump_out` on a gas

Optional `pump_out = { valve = "@valves.ion_pump", time_s = 120 }`: after
the gas's last segment, open, wait, close, before the next gas's `prepare`.
Absent: outlet handling as 3.4. Declarative because every multi-gas method
needs it.

### 11.4 Run phases

No new run states (owner, 2026-10-15). `prepare` and `pump_out` run inside
Measuring as engine blocks (`Block::Prepare`, `Block::PumpOut`, published
with the gas's name and named on the timeline), so executor, overlap and
resources see one measurement phase holding both the spectrometer and the
extraction line for the whole run.

- Consequence: such a run cannot overlap with the next run's extraction
  (line busy until the last gas). Queue validation says so when a queue
  asks for overlap with such a plan, rather than failing at run time.

### 11.5 Record

Per gas (6.1): the `prepare` script name, the variables it ran with, start
and end time; `pump_out` valve and time.

### 11.6 Rules

- A `prepare` script is an extraction script: no measurement host calls.
- Nothing here changes a plan without `prepare` or `pump_out`.
- Time: scripts and waits on the line's clock; no real sleeps (AGENTS.md,
  Time).
- A gas already measured is saved whatever happens to a later gas.

### 11.7 Out of scope

- A gas measured on another spectrometer. The quadrupole is on another
  install; the two installs coordinate (owner, 2026-10-15). How two installs
  hand a run's gas between them is its own spec, after a Quadera driver spec
  (legacy: `pychron/spectrometer/pfeiffer/spectrometer/quadera.py`,
  `hardware/quadera_spectrometer_controller.py`,
  `pyscripts/measurement/quadera_measurement_pyscript.py`). No
  `spectrometer` key on a gas in this spec.
- Reduction of non-argon gases (He, Ne, Kr, Xe ages or abundances).
- Conditionals that choose the next release temperature from the fraction
  just measured.

### 11.8 Tests

- Plan parse: `prepare` and `pump_out` accepted on a gas; unknown keys and
  unknown script refused at validation with the gas's name.
- Engine, fakes: two gases, each with `prepare`: order is prepare, inlet,
  segments, pump out, prepare, inlet, segments; variables reach the script;
  a failing second `prepare` ends the run failed with the first gas's
  series in the result.
- Simulated lab: cryo warmed in two steps releases two fractions; each
  gas's signal appears only after its release.
- Queue validation refuses overlap with a staged plan.

## Open questions

None.
