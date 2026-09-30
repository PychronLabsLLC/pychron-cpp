# Experiment System — Design

Date: 2026-09-29
Status: Approved (design), pending implementation plan
Depends on: `2026-09-29-instrument-control-design.md` (core, transport,
extraction line managers, config and UI conventions) and
`2026-09-29-spectrometer-control-design.md` (position/acquire primitives,
jobs, `SpectrometerState`).
Scope: experiment queues, run lifecycle, measurement definition and
execution, extraction scripting host, conditionals, data collection and
fits, the analysis record and the persister boundary, whole-lab simulation,
CLI and UI. Persistence implementations (DVC, SQL) are a later spec.

## 1. Intent

Pychron's experiment system works but has two structural problems the
rewrite must fix:

1. **Measurement scripting failed as a user surface.** Extraction scripts are
   authored by lab users and are valuable. Measurement scripts have only ever
   been written by the maintainer. The ~30 example measurement scripts in the
   repo share one skeleton and differ only in values: counts, integration
   time, baseline settings, peak-center flags, active detectors, fit names,
   hop tables, equilibration valves and times. Root causes named by the
   owner: users think in measurement concepts not code; scripts couple
   procedure to lab-specific detector and valve names; there is no safe,
   validated editing surface.
2. **Monolithic executor and run.** `experiment_executor.py` (3.7k lines)
   and `automated_run.py` (3.5k lines) mix queue loop, hardware, plots,
   persistence, UI traits and telemetry; run state is a set of flags plus a
   partially adopted state machine; conditionals are `eval` strings; scripts
   dispatch to managers by string; per-run git pushes run inside the
   collection thread.

Decisions (owner):

- Measurement = **declarative plan** from vendor templates, edited by the
  lab manager through a validated parameter surface; no code. A Python
  hook remains as an escape hatch for rare flows.
- Extraction stays scripted in **embedded Python** with the existing
  pyscript vocabulary so lab scripts port near-verbatim.
- Best-effort importer for existing measurement scripts; no importer for
  `.txt` queues.
- Persistence: define `IAnalysisPersister` and the `AnalysisRecord` schema
  here; DVC implementation later.
- This spec stops at the experiment engine over instrument primitives.

## 2. Queue and run model

`libs/experiment/model/`, Qt-free plain data.

```cpp
struct RunIdentity   { std::string identifier; std::optional<int> aliquot; std::string step; AnalysisType type; };
// AnalysisType: unknown | blank_unknown | blank_air | blank_cocktail | blank_extractionline | background
//               | air | cocktail | pause | degas | detector_ic

struct ExtractionSpec{ std::string device; std::optional<Position> position; double value; Unit units;
                       Duration duration, cleanup, pre_cleanup, post_cleanup; std::optional<Pattern> pattern;
                       std::optional<double> beam_diameter, ramp_rate; Duration ramp; std::optional<double> cryo_temp;
                       std::string script; ScriptOptions options; };

struct MeasurementRef{ std::string plan; ParamOverrides overrides; std::optional<std::string> hook; };

struct RunSpec       { RunIdentity id; ExtractionSpec extraction; MeasurementRef measurement;
                       std::optional<std::string> post_equilibration, post_measurement;
                       Overlap overlap; Duration delay_after; std::vector<ConditionalRef> conditionals;
                       std::string comment; std::optional<double> weight; bool skip = false, end_after = false;
                       SampleInfo sample; };

struct QueueSpec     { std::string name, mass_spectrometer, extract_device, tray, load, username, email;
                       Delays delays; std::string queue_conditionals; std::string repository;
                       std::vector<RunSpec> runs; };
```

- Pychron aliases (`e_value`/`extract_value`, `t_o`, `s_opt`, `truncate`)
  are normalized once at the model boundary.
- `AnalysisType` is derived from the identifier prefix through
  `identifiers.toml` (same defaults as pychron: `u b ba bc bu be bg c a pa dg
  ic`), not hard-coded.
- Aliquot and step are assigned at run start by `AliquotAllocator` through
  `IAnalysisPersister::next_aliquot(identifier)`; user-fixed aliquots are
  validated up front. There is one source of truth.
- Queue file `experiment.toml`: `[queue]` table plus `[[runs]]`.
  Schema-validated, diff-friendly. `skip = true` replaces commenting out.
- `ExperimentQueue` holds ordered runs plus edit operations (move, copy,
  repeat block, randomize, group by extraction, toggle skip/end-after).
  Frequency runs (blanks or airs every N unknowns, before/after) are
  expanded at edit time into explicit runs, never at execution time.
- Validation (`elctl exp validate` and on load): referenced plan, scripts
  and conditionals exist; extraction fields legal for the analysis type
  (the factory's per-type stripping rules become schema rules); positions
  parse; special identifiers are well formed; per-run duration estimates
  and total ETA are computed.
- `RunFactory` reduces to pure functions: defaults per
  `(analysis_type, extract_device)` from `defaults.toml`, block templates,
  position-range expansion, increment helpers.
- The queue text, schema version and creator are persisted at queue start.

Dropped: `.txt` tab format with YAML header, runtime frequency-run
generation, `human_error_checker` (replaced by schema and type rules),
Traits in the model.

## 3. Run lifecycle, executor, overlap

### 3.1 Run state machine (`libs/experiment/run/`)

```
Pending -> Preparing -> Extracting -> Equilibrating -> Measuring -> PostMeasuring -> Saving -> Success
                 |          |            |             |             |            \-> Failed(save_error)
                 \----------+------------+-------------+-------------+-> Failed | Cancelled | Aborted
                                                       \-> Truncated -> PostMeasuring -> Saving -> Success(truncated)
```

- Transitions only through `Run::advance(Event)`; each emits
  `RunStateChanged{run_id, from, to, reason, ts}`.
- Phases are objects: `IPhase { Result<PhaseOutcome> run(RunContext&,
  CancelToken&); Duration estimate(const RunSpec&); }`.
  `PreparePhase` (allocate aliquot, load plan and scripts, install
  conditionals, `begin_run` persist), `ExtractPhase` (script host),
  `EquilibratePhase` (valve sequence from the plan, eqtime, triggers the
  post-equilibration script and the overlap signal), `MeasurePhase`
  (MeasurementEngine), `PostMeasurePhase` (script; signals pump-time
  start), `SavePhase` (assemble `AnalysisRecord`, persist).
- `RunContext` carries typed handles: `ExtractionLine&`, `Spectrometer&`,
  `IExtractionDevice&`, `IAnalysisPersister&`, `SignalBus&`, `Clock&`, the
  run spec, and mutable `RunData`.
- `CancelToken` modes: `Cancel` runs post-equilibration and post-measurement
  then ends Cancelled without saving; `Abort` stops immediately;
  `Truncate{quick}` ends the current collection block and scales remaining
  counts (quick: 0.25); the run saves as `Success(truncated)`.
- A `PostMeasurePhase` failure still proceeds to `SavePhase`. A `SavePhase`
  failure ends `Failed(save_error)` with the record kept in the local spool.

### 3.2 Executor (`libs/experiment/executor/`)

```
Idle -> Preparing(queue) -> Running -> [StoppingAtBoundary | Cancelling | Aborting] -> Finalizing -> Idle
```

Per run: pluggable `IPreRunCheck`s (managers healthy, disk, persister
reachable, system pre-run conditionals) -> delay policy (`delay_after`, else
per-type delay, else between-runs delay; skipped when the previous run is
still measuring) -> build `Run` -> execute serially or overlapped -> post-run
checks -> queue conditional actions (repeat, skip N, cancel). Stop finishes
the current run; cancel and abort delegate to the active run's token;
`end_after` is honoured by the loop. Scheduled start and stop are Scheduler
one-shots. The executor persists `executor_state.json` on every transition
and offers resume from the next run after a restart; a partially measured
run is never re-run.

### 3.3 Overlap

- Allowed iff the current run is `unknown`, not last, and
  `overlap.delay > 0`.
- `EquilibratePhase` publishes `OverlapReady{run_id}` when the inlet
  closes; the executor waits for it plus `overlap.delay`, then starts the
  next run's Prepare/Extract in a second run slot.
- Hardware exclusivity through `Resource`s acquired by phases:
  `ExtractionDevice`, `ExtractionLine.inlet_group`, `Spectrometer`. The
  second run blocks on a resource, never on an ad-hoc event.
- `overlap.min_pump_time`: `PostMeasurePhase` publishes `PumpTimeStarted`;
  the next run's `EquilibratePhase` waits until the elapsed pump time
  reaches the minimum before opening the inlet. Visible in the UI as a
  named wait.
- At most two runs in flight.

Dropped: the monolithic executor with UI traits and telemetry,
`_sync_compatibility_state`, thread-per-run with shared events,
confirmation dialogs inside the executor.

## 4. MeasurementPlan schema and engine

### 4.1 Plan (data, not code)

```toml
[plan]
name = "argus_multicollect"
instrument_family = "thermo_argus"
description = "Standard 6-collector static measurement"

[detectors]
reference = "H1"                   # default magnet-positioning detector for every hop
exclude = []                       # UI toggle: drop these detectors from every hop

[equilibration]
inlet  = "@valves.inlet"          # '@' = alias from extraction_line.toml [aliases]
outlet = "@valves.outlet"
time_s = 15                        # or "@extraction.eqtime"
inlet_delay_s = 3
close_inlet = true

[sniff]        enabled = true   counts = "@equilibration.time_s"  integration_s = 1
[peak_center]  before = false   after = true   detector = "H1"  isotope = "Ar40"  config = "default"
[baseline]     before = false   after = true   counts = 120  mass = 34.2  detector = "H1"  settle_s = 15  integration_s = 1

# A measurement is `cycles` repetitions of an ordered list of hops.
# Multicollect is the degenerate case: one hop, one cycle.
[main]
cycles = 1
integration_s = 1
time_zero = "on_inlet_close"      # on_inlet_close | on_first_count | offset_s = N

[[main.hops]]
positions = { Ar40 = "H1", Ar39 = "AX", Ar38 = "L1", Ar37 = "L2", Ar36 = "CDD" }
counts = 400
settle_s = 3

# Peak hop: same schema, more hops and cycles.
# [main] cycles = 20
# [[main.hops]] positions = { Ar40 = "H1", Ar36 = "CDD" }  counts = 10  settle_s = 3  protect = ["CDD"]
# [[main.hops]] positions = { Ar39 = "CDD" }               counts = 10  settle_s = 3
#               position = { isotope = "Ar39", detector = "CDD" }   # reference detector not in this hop
# [[main.hops]] positions = { Ar36 = "CDD" } counts = 10 settle_s = 3 baseline = true  mass = 34.2

[fits]
signal   = { default = "linear", Ar40 = "parabolic" }
baseline = { default = "average" }
error    = "sem"   outliers = { enabled = true, iterations = 1, std_devs = 2 }

[conditionals]
include = ["@conditionals.default_unknown"]
truncations = [{ check = "Ar40 > 8e5", start = 20 }]

[whiff]  enabled = false

[parameters]
expose = [{ path = "main.hops[0].counts", label = "Counts" }, "main.integration_s", "main.cycles",
          "baseline.counts", "baseline.settle_s", "peak_center.before", "peak_center.after",
          "detectors.exclude"]
```

Hop rules:

- `positions` maps isotope -> detector for that hop; those detectors are
  active while the hop collects (minus `detectors.exclude`).
- The magnet positions the isotope assigned to `detectors.reference` in the
  hop. If the reference detector is not in the hop, `position = { isotope,
  detector }` is required; a hop with neither is a validation error.
- `counts` per hop per cycle; `settle_s` after the move; `protect` detectors
  are protected for the move; `baseline = true` marks a baseline hop (series
  kind `baseline`), optionally at `mass` instead of an isotope position.
- If a hop's target position equals the current position (single-hop
  measurements after the first cycle), the move and settle are skipped, so
  one hop x N cycles is identical to one hop with N x counts.
- An isotope may appear in several hops on different detectors; series stay
  keyed by (isotope, detector, kind).
- `expose` entries are dotted paths (indexed paths allowed) or
  `{ path, label }` tables for the run-editor form.

Rules:

- Only keys listed in `parameters.expose` are editable in the run editor and
  settable through `RunSpec.measurement.overrides` (schema-enforced) unless
  the run is marked `advanced`.
- Instrument coupling lives in aliases (`extraction_line.toml [aliases]`)
  and detector names validated against `spectrometer.toml`; one template
  serves every lab with the same instrument family.
- Duration estimate is arithmetic over the plan (sum over cycles and hops of
  settle + counts x integration, minus skipped settles); no execution.

### 4.2 MeasurementEngine (`libs/experiment/measurement/`)

Fixed block sequence; each block is a small class with `run(ctx, token)`:

```
[peak_center.before] -> [baseline.before] -> [position first hop]
-> [equilibrate || sniff] -> [time zero] -> [main: cycles x hops] -> [baseline.after]
-> [peak_center.after] -> [done]
```

- Blocks use only spectrometer primitives (`position`, `acquire` stream,
  `protect`, `submit_job(PeakCenterSpec)`) and extraction-line open/close.
- The engine owns equilibrate/sniff timing; `OverlapReady` is published when
  the inlet closes.
- Collection blocks drive the `Collector` (section 8).
- One `HopRunner` executes every collection: per hop, protect set ->
  `position(iso on det)` (skipped with its settle when already there) ->
  settle -> `acquire(counts)` with the hop's detector -> isotope map ->
  unprotect. `main` runs it `cycles` times over the hop list; sniff and the
  before/after baselines are single-hop runs of the same runner with kind
  `sniff` / `baseline`. A named field table is selected with `with_table`.
  There is no separate multicollect code path.
- Truncate ends the current block at the next reading; remaining collection
  blocks scale counts by the ratio.
- Typed progress events (`BlockStarted/Finished`, `CountsProgress{i, n}`).

### 4.3 Escape hatch

`hook = "scripts/measurement_hooks/<name>.py"`: Python called at named
points (`before_main`, `after_main`, `on_whiff_result`) with a typed
`MeasurementAPI` (position, acquire, open/close, add_conditional, truncate,
log). A hook cannot replace the block sequence.

Dropped: `MeasurementPyScript` as user surface, `mx` docstring YAML,
`set_fits("Ar40H1:parabolic")` strings, `define_detectors` ordering
conventions, `hops.txt` tuples, per-vendor measurement script subclasses.

## 5. Templates, parameter surface, importer

### 5.1 Template library

```
templates/
  thermo_argus/  multicollect.toml  peak_hop_cdd.toml  detector_ic.toml  air.toml  blank.toml  degas.toml
  thermo_helix/  ...
  isotopx_ngx/   multicollect.toml  peak_hop.toml
  generic/       single_collector_hop.toml        # legacy split systems
```

- Each template declares `instrument_family`, the `analysis_types` it fits,
  and `parameters.expose`.
- `defaults.toml` per lab maps `(analysis_type, extract_device)` to
  `{template, overrides, extraction script}`.
- Lab templates live in `<lab>/templates/`; resolution is lab then shipped.
  Editing a shipped template forks it into the lab directory.

### 5.2 Parameter surface

- The run editor form is generated from `parameters.expose` plus field
  types, ranges, defaults and help from the schema; `elctl exp set-param`
  uses the same schema.
- Advanced: a structured, validated tree editor over the full plan; saving
  creates a lab template.
- Per-run overrides display as a diff against the template.
- Every edit re-validates against the live configs and recomputes duration.

### 5.3 Importer `tools/pychron_measurement_import.py`

1. Parse the `mx` docstring YAML, module constants, and the `main()` AST
   for the known skeleton (`activate_detectors`, `peak_center`, `baselines`,
   `position_magnet`, `equilibrate`, `sniff`, `set_fits`,
   `set_baseline_fits`, `multicollect|peak_hop`, `warm_cdd` gosub, `whiff`).
2. Classify: standard (values only) -> template overrides; near-standard ->
   plan plus notes; exotic -> plan for the recognizable part plus a hook
   stub containing the unmatched code as comments, flagged.
3. Convert `multicollect(ncounts)` + `activate_detectors`/`define_detectors`
   -> one `[[main.hops]]` with `cycles = 1`; `peak_hop(ncycles, hops)` and
   `hops.txt|.yaml` -> `[[main.hops]]` + `cycles`; fits yaml -> `[fits]`; and
   check detector literals against `lab_profile.toml`.
4. Emit a per-script report; nothing is dropped silently.

Success metric: the ~30 example argus/helix scripts yield at most 4 exotic
cases; the importer ships with them as fixtures.

## 6. Extraction scripting host

`libs/scripting/`, Qt-free. Embedded CPython 3.12 through pybind11, one
sub-interpreter per script execution, GIL held only during script calls.
Build option `PYCHRON_SCRIPTING` (default on; off installs a stub host that
rejects scripted runs).

Vocabulary preserved: `open close lock unlock is_open is_closed`,
`extract end_extract ramp fire_laser enable disable prepare warmup`,
`move_to_position set_x set_y set_z set_xy execute_pattern set_tray`,
`dump_sample drop_sample`, `load_pipette extract_pipette`,
`set_motor get_value set_cryo get_cryo_temp`, `begin_heating_interval`,
`snapshot video_*`, `get_pressure get_manometer_pressure`,
`waitfor wake pause sleep delay`, `acquire wait release set_resource
get_resource_value`, `info`, `gosub`, `begin_interval complete_interval`,
`set_pid_parameters`, `get_device` (read-only handle).

Underneath:

- Every command binds to a typed C++ interface: `IValveService`,
  `IExtractionDevice` (+ optional `ILaserDevice`, `IFurnaceDevice`, `IStage`,
  `IPatternRunner`, `IPipetteService`, `ICryo`, `IMotorService`, `IImaging`,
  `IPressureService`). A missing capability is a `NotSupportedError` found by
  the static check.
- Static check: compile plus symbol-table walk against the vocabulary and
  the run's capabilities; catches unknown commands, arity, unavailable
  device features, unresolvable valve names. Runs on save and in
  `elctl exp validate`.
- Duration estimate: an `estimate()` pass with a `DurationAccumulator`
  host; gosubs recursed; unbounded loops flagged. No hardware mocks.
- Cancellation: `CancelToken` checked in every blocking command; `Cancel`
  raises `ScriptCancelled` so `finally:` blocks run; `Abort` interrupts and
  the host refuses further hardware calls.
- Sandbox: restricted builtins, import allowlist, file access only through
  host helpers, watchdog limits. Guards against accidents, not a hostile
  author.
- Context: pychron's `set_default_context` names injected as read-only
  globals; `script_options` injected as `opt.<key>`; `ex`/`mx` removed.
- Metadata such as `eqtime` moves to a `#! pychron: key=value` header line.
- Same host for extraction, post_equilibration, post_measurement (adds
  `get_intensity`, `signal_pump_time_start()`), and measurement hooks.
- Script text and SHA recorded in the `AnalysisRecord`.

Layout: `scripts/extraction/`, `scripts/post_equilibration/`,
`scripts/post_measurement/`, `scripts/measurement_hooks/`, `scripts/lib/`.

Dropped: `exec` in an open namespace, `_manager_action` string dispatch,
decorator-based dry-run re-execution, vendor-specific script subclasses.

## 7. Conditionals

Grammar preserved, `eval` removed. `libs/experiment/conditionals/` parses
each check once into an AST evaluated against a typed `MetricContext`.

```
expr     := or_expr
or_expr  := and_expr ('or' and_expr)*
and_expr := not_expr ('and' not_expr)*
not_expr := 'not' not_expr | cmp
cmp      := value (op value)?            op in < <= > >= == !=
value    := number | metric | func '(' args ')' | '$' NAME | '(' expr ')'
metric   := ISO | ISO '/' ISO | ISO '.' ('cur'|'bs'|'bs_corrected'|'ic_corrected')
          | DET '.' ('deflection'|'inactive'|'intensity') | 'age' | 'kca' | 'radiogenic_yield'
          | 'device.' NAME | 'gauge.' NAME '.pressure' | 'param.' NAME
func     := min | max | average | slope | std | rsd | between | count | elapsed
```

- Metrics come from the `Collector` series, the spectrometer snapshot, the
  extraction line, and on-demand isotope-group computations from the
  reduction library.
- `window` applies to series functions; `$NAME` resolves from
  `script_options` then plan parameters.
- Comparing a series to a scalar without a reducer is a validation error.

Kinds and fields (TOML):

```toml
[[truncations]]   check = "Ar40 > 8e5"                      start = 20  frequency = 5
[[terminations]]  check = "average(Ar36, window=10) < 0"    start = 30  ntrips = 3
[[cancelations]]  check = "gauge.ion_pump.pressure > 1e-6"
[[actions]]       check = "slope(Ar40) > 100"  start = 10  action = "truncate:quick"  resume = false
[[modifications]] check = "Ar40 < 1e3"  action = "skip_aliquot"   # skip_n | skip_aliquot | repeat | run_blank | set_extract=<v>
[[equilibrations]] check = "slope(Ar40) < 50"  start = 5
[[pre_run]]       check = "gauge.spec.pressure < 5e-9"
[[post_run]]      check = "Ar40 > 1e6"  action = "run_blank"
```

- `action` is an enum: `truncate[:quick]`, `terminate`, `cancel`,
  `set_param NAME=v`, `run_hook NAME`, `notify`. No script snippets.
- Levels merge in order system -> queue -> plan -> run; later levels add; a
  run may `disable = ["name"]` a named upstream conditional.
- Evaluation after every reading in pychron's order: modification ->
  truncation -> action -> termination -> cancelation -> equilibration. Trips
  are typed `Trip{name, kind, value, count, ts}` events and recorded.
- Whiff is a plan block: sniff N counts, then an ordered list of
  `{check, action in run_remainder | pump | abort}`.

Dropped: `eval`, regex-classified test strings, script-snippet actions,
duplicated load paths.

## 8. Collection, fits, `AnalysisRecord`, persister boundary

### 8.1 Collector (`libs/experiment/collect/`)

One class. Input: `IntensityStream` plus
`CollectionSpec{kind in signal|baseline|sniff|whiff, counts, time_zero,
detectors -> isotopes}`. Per reading: append to `Series[iso, det, kind]`,
evaluate conditionals, publish `SeriesUpdated`. Peak hop = the engine
calling the collector once per hop. `set_target(counts)` supports user count
changes. Output is `RunData` (series, trips, timing), plain data.

### 8.2 Fits (`libs/reduction/fits/`, pure)

`FitSpec{kind in average|linear|parabolic|cubic|exponential|custom_poly(n),
error in sem|sd, outliers{enabled, iterations, std_devs}}`.
`fit(series, spec) -> Intercept{value, error, n_used, filtered_idx, residual_sd}`.
Algorithms ported with regression fixtures against pychron outputs on real
series. The live intercept shown in the UI and used by conditionals is the
same function on the running series.

### 8.3 `AnalysisRecord` v1 (`libs/experiment/record/`)

```
identity     uuid, identifier, aliquot, step, analysis_type, timestamp, run_index, queue_uuid
sample       sample, project, material, irradiation/level/position, PI, note
instrument   mass_spectrometer, extract_device, laboratory, analyst, software{version, git sha}
extraction   ExtractionSpec + actuals{value, duration, cleanup, positions, beam_diam, pattern, pid_params,
             response/output/setpoint series, cryo series, snapshot refs, grain polygons, pipette counts,
             manometer pressure}
measurement  plan{template, version, effective_plan_toml, overrides}, hook{name, sha}?,
             scripts{extraction, post_eq, post_meas: name + sha + text ref}
spectrometer SpectrometerState snapshot (content hash), integration actual, deflections, gains,
             source params, field table version
data         series[iso, det, kind] as float32 (t, v[, sigma]), time_zero, count totals
results      intercepts{iso: Intercept + FitSpec}, baselines{det: value, error, spec},
             blanks_ref, icfactors{det: 1.0 default}, whiff result
conditionals installed[], tripped[]
events       run state transitions, alarms, peak-center results, environment samples
provenance   schema_version, record sha, persister refs
```

Built incrementally by `RecordBuilder`, finalized in `SavePhase`;
deterministic serialization. Derived values (age, K/Ca, yield) are computed
by reduction from the record, not stored.

### 8.4 Persister boundary

```cpp
struct IAnalysisPersister {
  virtual Result<int>  next_aliquot(const std::string& identifier) = 0;
  virtual Result<void> begin_run(const RunIdentity&, const QueueSpec&) = 0;
  virtual Result<void> save_extraction(const AnalysisRecord&) = 0;
  virtual Result<void> save_analysis(const AnalysisRecord&) = 0;
  virtual Result<void> save_artifact(Uuid, std::string name, Bytes) = 0;
  virtual Result<void> flush() = 0;
};
```

Implementations follow `2026-09-29-persistence-adr.md` (ADR-0002: database
authoritative, git as asynchronous mirror): `DbPersister` (SQLite for a
single instrument or offline, PostgreSQL for a multi-instrument lab; one
local transaction per save; raw series stored content-addressed by hash)
and `FilePersister` (records directory; also the recovery spool). The git
publisher and importer are background services fed from the database and
are never persisters on the acquisition path. Spool-first: `SavePhase`
always writes to the local spool, then hands the record to the configured
persister on the Scheduler. Persister failure never blocks the next run; the
UI shows pending records and `elctl exp flush` retries. Unspooled records
are re-sent on startup. `AnalysisRecord` is the unit both the DB schema
and the mirrored JSON layout are derived from.

Dropped: `PersistenceSpec`, HDF5/Excel persisters, multiple git commits per
run inside the run thread, plot updates from the collector.

## 9. Simulation and testing

Whole-lab sim = `SimSystem` (extraction line) + `BeamModel` (spectrometer)
+ `SimExtractionDevice` (power -> temperature -> gas release into the line's
sample volume). Gas flows into the BeamModel when the inlet opens, so runs
produce realistic evolutions. Deterministic clock and RNG; a 40-minute run
executes in about a second with `ManualClock`.

The scripting host in tests is the real embedded CPython; test scripts are
ports of the documented example extraction scripts, plus one fixture per
static-check error class.

```
tests/experiment/
  model/          queue round-trip; identifiers; per-type field rules; frequency expansion
  run/            every legal and illegal transition; cancel/abort/truncate per phase;
                  post-measure failure still saves; save failure -> Failed(save_error) + spool
  executor/       delay policy; end_after; stop-at-boundary; queue conditional actions;
                  overlap gating, min_pump_time, resource exclusivity; resume from executor_state.json
  measurement/    plan schema (one failing fixture per rule); alias resolution; duration arithmetic;
                  block order; single-hop == multicollect equivalence (one hop x N cycles vs N x counts);
                  multi-hop ordering, move/settle skipping, baseline hops; equilibrate/sniff timing; truncate scaling; hooks
  scripting/      vocabulary binding; static-check errors; estimate within 1% of sim duration;
                  cancel raises and finally runs; abort halts; sub-interpreter isolation
  conditionals/   parser round-trip on conditionals.rst examples; evaluator; order; ntrips/frequency/start;
                  level merge and disable
  collect/        series keying incl. peak hop; set_target; SeriesUpdated cadence
  fits/           regression fixtures from pychron (tests/data/series/*.csv)
  record/         serialization determinism; schema version bump; builder completeness
  persist/        spool semantics; retry; recovery
  importer/       (Python) example scripts -> expected classification and overrides
tests/integration/experiment/
  6-run sim queue (blank, air, 3 unknowns with overlap, detector_ic): all Success, complete records,
  intercepts within tolerance of BeamModel truth, planted conditional trip, cancel mid-measure leaves
  the line safe
```

- Property-style: random cancel/abort/truncate injection -> no script-opened
  valve left open, extraction device disabled, detectors unprotected, record
  complete or absent.
- Golden-record test: same seed and queue -> byte-identical record set.
- Conformance suite for `IExtractionDevice` implementations.

## 10. Milestones, CLI, UI

### 10.1 Definition of done

A 6-run sim queue runs unattended from the UI and from `elctl`, using a
shipped Argus template with per-run overrides, a ported extraction script
and inline conditionals; records land in the spool and `FilePersister`; the
same queue runs on a real Argus and extraction line at bring-up.

### 10.2 Stages

| Stage | Units | Depends on |
|---|---|---|
| E1 model | `exp_model`, `queue_file`, `run_factory` | instrument-control `core` |
| E2 measurement data | `plan_schema`, `conditionals`, `fits`, `analysis_record` | E1 |
| E3 scripting | `scripting_host`, `extraction_device_ifaces` (+ `SimExtractionDevice`, conformance) | E1, extraction-line `switch_manager` |
| E4 engine | `collector`, `measurement_engine`, `run_state_machine`, `persister_boundary` | E2, E3, spectrometer S3 |
| E5 executor | `executor`, `whole_lab_sim`, `integration_queue_tests` | E4 |
| E6 tooling | `measurement_importer`, `template_library`, `elctl exp-*` | E5 |
| E7 UI | `exp_ui` | E6 |
| E8 bring-up | real queue on Argus (manual) | E7 |

### 10.3 `elctl exp`

`validate`, `estimate`, `plan show [--overrides]`, `plan-diff`,
`script check`, `run <queue> [--from N] [--dry-run]`, `status`,
`stop | cancel | abort | truncate [--quick]`, `flush`, `import-measurement`,
`sim queue <queue>`.

### 10.4 UI (`apps/pychron-ui`, via `CoreBridge`)

- Queue editor: run table with per-type field enabling, inline validation,
  reorder, repeat/copy/skip/end-after, frequency insert, ETA; run-factory
  side panel.
- Measurement panel: template picker filtered by instrument family and
  analysis type; generated parameter form; override diff badges; Advanced
  structured editor.
- Script editor: highlighting, inline static-check diagnostics, gosub
  navigation, autocomplete from the host command table, live estimate.
- Executor pane: queue progress, phase timeline, overlap lanes, explained
  waits, stop/cancel/abort/truncate with UI-side confirmation, conditionals
  view, pending-persist counter.
- Evolutions: live series with current intercept and fit overlay, baseline
  and sniff tabs, peak-center results; shares the spectrometer plotting
  widget.
- Notifications: email/webhook subscriber on failure and queue end.

### 10.5 Deferred

`DbPersister` and the git publisher/importer (persistence spec per ADR-0002); age display in evolutions; dashboard/labspy;
multi-queue scheduling; visual valve programmer; UV-specific extraction
columns (as a device-specific `extra` table).

## 11. Open decisions carried into the plan

Non-blocking: pybind11 vs nanobind; sub-interpreter vs fresh module dict
per run if sub-interpreters prove fragile with pybind11; `Resource` API
shape shared with the extraction-line facade; whether `fits` lives in
`libs/reduction` now or `libs/experiment` until the reduction spec exists.
