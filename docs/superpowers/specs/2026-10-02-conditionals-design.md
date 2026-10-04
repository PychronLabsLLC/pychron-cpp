# Conditionals: full port

Date: 2026-10-02
Status: Implemented
Depends on: `2026-09-29-experiment-system-design.md` section 7 (grammar and
kinds, already implemented as the first cut) and section 4.2 (MeasurementEngine).
Reference: legacy `pychron/experiment/conditional/*`, `automated_run/*`,
`experiment_executor.py`. A file:line inventory of the legacy behaviour was
taken for this spec; its findings are summarized in section 2.

## 1. Intent

The first cut (wave 11) delivered a safe grammar, an AST evaluator, eight
kinds, TOML loading and level merging. It does not yet cover what labs rely on
in pychron:

- per-conditional `window`, `mapper`, `analysis_types`,
  `abbreviated_count_ratio`, action `resume`;
- the full metric vocabulary (corrected intercepts, `.cur`, `.std_dev`,
  baselines, detector deflection/inactive, gauges, devices, `instant_age`,
  `age`, `kca`, `radiogenic_yield` and the other computed Ar-Ar values,
  detector names like `L2(CDD)`);
- queue modifications (skip next/N/aliquot/to-last-in-aliquot, set extract,
  repeat run, run blank) actually applied to the queue;
- pre-run and post-run checks with state across runs;
- whiff;
- trip provenance in the record (what was evaluated against which values);
- loading from the lab's system / queue / run / plan sources, and an
  importer for the legacy YAML files.

The port keeps pychron's semantics where they are deliberate and fixes the
places where the legacy code is broken or contradicts its own documentation.
Every such decision is listed in section 2 so nothing changes silently.

## 2. Legacy behaviour and decisions

| # | Legacy behaviour | Decision |
|---|---|---|
| L1 | Check strings are rewritten by ordered regexes and `eval`ed; spaces inside an operand break parsing; `(`/`)` cannot group. | Keep the AST grammar (section 3). Whitespace-insensitive, real grouping, no `eval`. The importer rewrites legacy strings. |
| L2 | Gating: evaluated when `cnt - start > 0` and `(cnt - start) % frequency == 0`, so the first check is at `start + frequency`. `cnt` is the count *about to be* acquired (n-1 points exist). | Same arithmetic on the number of readings collected: evaluated after reading `n` when `n > start` and `(n - start) % frequency == 0`. The C++ engine evaluates after a reading, so it sees one point more than pychron did at the same count; documented. |
| L3 | `ntrips` consecutive true evaluations; a false evaluation resets the count; after firing, trips reset and the conditional can fire again. | Same. A conditional fires once per run, except an `action` with `resume = true`, which re-arms and needs another `ntrips`. Re-firing a truncation/termination/cancelation is meaningless; re-firing a modification inserted duplicate runs (legacy bug). |
| L4 | Per count, categories in order modification, truncation, action, termination, cancelation (+ equilibration during sniff); the first tripped conditional acts and evaluation stops. | Same: at most one trip per reading. |
| L5 | Defaults: `start` 50 from YAML, 0 from the script API; `frequency` 1 from YAML, 10 from the API. | Native TOML: `start = 0`, `frequency = 1`. The importer writes legacy defaults explicitly (`start = 50` when absent). |
| L6 | `analysis_types` filter, lowercased, `blank` matches every `blank_*`. | Same. |
| L7 | `window`: the plain value becomes the mean of the last N raw points; functions use the last N points. | Same, as a load-time AST transform: bare isotope `X` becomes `average(X, window=N)`; series functions without their own `window=` get N. |
| L8 | `mapper` (`x+1000`): every fetched value is transformed by `eval`. | Same semantics without `eval`: the mapper is parsed as an arithmetic expression in `x` and substituted into every metric and function node of the check at load time. |
| L9 | Plain `Ar40` is the fully corrected intercept (baseline, blank, IC); `average`/`min`/`max`/`.cur` use raw points. | Same split: scalar `Ar40` = live IC-corrected intercept (fit per plan, baseline mean subtracted, icfactor applied; blanks are not known in-run). Series functions use raw points. |
| L10 | `.std_dev`/`.sd`/`.stddev` never worked (eval AttributeError). | Implemented as the intercept's error. |
| L11 | `$VAR` interpolation never worked (wrong method name). | Implemented: `$NAME` from script options, then plan parameters, then run variables. |
| L12 | `between(...)` only worked with plain keys and min/max; bounds limited to one decimal. | `between(value, lo, hi)` takes any value expressions. |
| L13 | `not` only before plain/function tokens; attribute-vs-attribute comparisons and keys with `_` failed. | Full boolean algebra, any comparison, any metric name. |
| L14 | A missing value drops the operand (often a SyntaxError); an unknown plain key evaluates to 0; eval exceptions kill the thread. | Unavailable metric -> the check is not evaluated for this reading (no trip, trip counter reset), recorded once per conditional in the run's conditional errors. Static validation (section 7.3) catches unknown names up front. |
| L15 | Termination is documented as "end this run, continue the queue" but escalates to cancelling the experiment. | Documented intent: termination ends the run; data are saved; the queue continues. |
| L16 | Cancelation cancels the run without saving and cancels the queue. | Same: `MeasurementOutcome::Cancelled` with `cancel_queue = true`. |
| L17 | Truncation sets the script's `abbreviated_count_ratio` from the conditional (default 1.0); later `multicollect`/`baselines` multiply counts by it; `truncate quick` forces 0.25. | Same. The engine's count scale after a truncation is the conditional's `abbreviated_count_ratio`; a user truncate uses 1.0, quick 0.25. |
| L18 | Action: a pyscript snippet `exec`ed; `resume = false` ends the current measurement block (not marked truncated). | Enum actions (`truncate[:quick]`, `terminate`, `cancel`, `set_param`, `run_hook`, `notify`). `run_hook NAME` replaces snippets (the importer emits a hook stub). `resume = false` ends the current collection block; true continues. |
| L19 | Equilibration trips crashed (missing fields); intended to end equilibration. | An equilibration trip closes the inlet early (ends equilibration). |
| L20 | Modifications: skip next / N / aliquot / to-last-in-aliquot, set extract (broken), repeat run, run blank; `use_truncation`/`use_termination`; always overwrote the ratio; re-fired while true. | All seven implemented over `ExperimentQueue` (section 6), set extract with absolute or percent step lists; `truncate`/`terminate` flags; `abbreviated_count_ratio` applied only with `truncate`; fires once. |
| L21 | Pre-run terminations: queue then system files, one spectrometer reading, checked before extraction and before measurement; trip cancels the queue. Reloaded every check, so `ntrips` never accumulates; trips not persisted. | Same timing and effect, through `RunChecks` (section 6.2), which keeps state across runs so `ntrips` counts consecutive runs; trips are returned for the executor to log and persist. |
| L22 | Post-run terminations/actions: evaluated after save on the finished run's values; `post_run_actions` in files broke run start; "repeat" used missing fields. | Same timing; values from the saved `AnalysisRecord`. `post_run` kind with any queue action (`repeat`, `run_blank`, `skip_*`, `set_extract`) or `cancel` (default). |
| L23 | Levels SYSTEM/QUEUE/RUN label only; lists ordered run, queue, system, script. | Merge order system -> queue -> plan -> run (later levels add or replace by name, `disable` removes); level and source location recorded per conditional. |
| L24 | Run-level YAML required an `attr` key, got `level=None`; peak hop re-added run conditionals (duplicates). | No `attr` key (the metric comes from the check); level RUN; no duplication. |
| L25 | Persisted: `conditionals` (to_dict list with a per-process `hash_id`) and `tripped_conditional` (rewritten string + value context). | `AnalysisRecord.conditionals` v2: every installed conditional with level, location and parameters; every trip with kind, reading, time, value, action and the value of every metric the check referenced. Stable `id` = sha256 of the canonical definition. |
| L26 | Whiff: N counts, then the first tripped temporary conditional's free-form `action` string is the result returned to the script. | Plan `[whiff]` with `counts` and ordered `[[whiff.checks]]` with `run_remainder` / `pump` / `abort`, evaluated once after the whiff readings, during equilibration. `pump` closes the inlet and ends the measurement as Terminated with no main data; `abort` aborts. The result is recorded and passed to the hook's `on_whiff_result`. |
| L27 | Conditionals evaluated during baselines too unless the script cleared them. | Evaluated on main signal collections only (the documented practice). |
| L28 | Legacy inline run value `"<check>,<start>"` -> one truncation with ratio 0.5. | Importer only. |
| L29 | Dead code: `StatefullConditional`, `value`, `active`, results counter, `delay_after_conditional`. | Dropped. |

## 3. Grammar

```
expr     := or_expr
or_expr  := and_expr ('or' and_expr)*
and_expr := not_expr ('and' not_expr)*
not_expr := 'not' not_expr | cmp
cmp      := sum (op sum)?                  op in < <= > >= == !=
sum      := term (('+'|'-') term)*
term     := unary (('*'|'/') unary)*
unary    := '-' unary | atom
atom     := number | metric | func '(' args ')' | '$' NAME | '(' expr ')'
metric   := ISO | ISO '/' ISO
          | ISO '.' ('cur'|'current'|'bs'|'bs_corrected'|'ic_corrected'|'intercept'|'std_dev'|'sd'|'stddev')
          | DET '.' ('deflection'|'inactive'|'intensity')
          | COMPUTED
          | 'device.' NAME | 'gauge.' NAME '.pressure' | 'param.' NAME
func     := min | max | average | slope | std | rsd | count      (series; window=N)
          | between(v, lo, hi) | abs(v) | elapsed()
COMPUTED := age | instant_age | kca | cak | kcl | clk | radiogenic_yield
          | rad40 | rad40_percent | atm40 | k39 | ca37 | ca39 | ca36 | cl36
DET      := NAME | NAME '(' NAME ')'      e.g. CDD, L2(CDD)
```

- `ISO/ISO` written with two bare names is an isotope ratio metric (usable as
  a series in `slope(Ar40/Ar39)`); any other `/` is arithmetic. As scalars
  they are the same number.
- `.current` = `.cur`; `.sd`/`.stddev` = `.std_dev`.
- A series-only metric (`X.bs`) compared without a reducer is a parse error.
- Canonical text (`to_string`) re-parses to the same AST.

## 4. Conditional model and files

```cpp
struct Conditional {
  std::string name;               // unique in a merged set
  ConditionalKind kind;
  std::string check; shared_ptr<const Expr> expr;   // after window/mapper transforms
  int start = 0, frequency = 1, ntrips = 1;
  std::optional<int> window;      // L7
  std::string mapper;             // L8, e.g. "x + 1000"
  std::vector<std::string> analysis_types;          // L6; empty = all
  double abbreviated_count_ratio = 1.0;             // truncation, equilibration, modification
  ActionSpec action;  bool resume = false;
  ConditionalLevel level;  std::string location;    // provenance
  std::string id() const;          // sha256 of the canonical definition
};
```

`ActionSpec` adds `skip_next`, `skip_n N`, `skip_to_last_in_aliquot`,
`set_extract STEPS` (`"1,2,3"` absolute increments or `"10%,20%"`), and the
modification flags `truncate`/`terminate`.

TOML (one file per source):

```toml
disable = ["system:gauge_high"]

[[truncations]]
name = "big"
check = "Ar40 > 8e5"
start = 20
abbreviated_count_ratio = 0.5

[[terminations]]
check = "average(Ar36, window=10) < 0"
start = 30
ntrips = 3
analysis_types = ["unknown", "blank"]

[[actions]]
check = "slope(Ar40) > 100"
action = "run_hook warn_operator"
resume = true

[[modifications]]
check = "Ar40 < 1e3"
action = "skip_aliquot"
truncate = true
abbreviated_count_ratio = 0.25

[[equilibrations]]
check = "Ar40.cur > 5e5"

[[pre_run]]
check = "CDD.inactive or gauge.spec.pressure > 5e-9"

[[post_run]]
check = "Ar40 < $MIN_INTENSITY"
action = "run_blank"
analysis_types = ["air", "cocktail"]
```

Sources and levels (`ConditionalLibrary`):

| Level | Source |
|---|---|
| system | `<lab>/conditionals/system.toml` |
| queue | `<lab>/conditionals/<QueueSpec.queue_conditionals>.toml` |
| plan | `[conditionals]` of the plan: `include = ["@conditionals.NAME"]` (files in `<lab>/conditionals/`) plus inline `truncations` |
| run | each `RunSpec.conditionals[i].name` -> `<lab>/conditionals/<name>.toml` |

`ConditionalLibrary::for_run(queue, run, plan)` returns the merged set with
level and location stamped on every conditional.

## 5. Metrics

`MetricContext` implementations, chained (first that answers wins):

1. **Collector** (in-run): raw series, `.cur`, `.bs`, the live intercept per
   isotope fitted with the plan's fit (`.intercept`, `.std_dev`),
   `.bs_corrected`, `.ic_corrected` / bare value, ratios, `DET.intensity`,
   `elapsed()`; computed values through an `IComputedMetrics` provider fed the
   live corrected intercepts.
2. **InstrumentMetrics** (adapter): `DET.deflection`, `DET.inactive` from
   `Spectrometer::detector_state`; `gauge.NAME.pressure` from the extraction
   line's latest snapshot (or a fresh read); `device.NAME` through a
   lab-supplied value reader (motors, resources).
3. **RecordMetrics** (post-run): the same names over a finished
   `AnalysisRecord` (intercepts, baselines, icfactors, computed values).

Computed Ar-Ar values (`libs/reduction/arar`, on the shared kernels of the
reduction spec 8.1): from corrected 36-40 intercepts and `ArArConstants`
(decay constants, atmospheric 40/36, production ratios including `K3739`,
`K3839`, `Ca3837` and `Cl3638`, J, 37/39 decay factors, K/Ca factor):

```
k39  = (Ar39 df39 - Ca3937 Ar37 df37) / (1 - K3739 Ca3937)     (K3739 = 0: Ar39 df39 - ca39)
ca37 = Ar37 df37 - K3739 k39;   ca36 = (36/37)Ca * ca37;   ca39 = (39/37)Ca * ca37
                      (clamp: ca37 = 0 when allow_negative_ca_correction is false and ca37 <= 0)
atm36 = Ar36 - ca36;   atm40 = atm36 * (40/36)atm
rad40 = Ar40 - atm40 - (40/39)K * k39;   rad40_percent = radiogenic_yield = 100 rad40 / Ar40
F = rad40 / k39;   age = ln(1 + J F) / lambda   (Ma)
kca = kca_factor * k39 / ca37;   cak = 1 / kca
```

With the chlorine inputs set (`ArArConstants::chlorine`: `cl3638`,
`lambda_cl36`, `decay_days`, `atm4038`, `cl_k_factor`) and `Ar38` present,
the atmospheric step is the chlorine-aware E12 and `cl36`, `kcl`, `clk` are
also computed (`kcl = k39 / cl38 * cl_k_factor`, `clk = 1 / kcl`). A per-analysis
or constants fixed `K3739` selects the fixed mode of the reduction spec E10.
`MetricCatalog::chlorine` tells static validation that these keys are
available to static validation.

`instant_age` is `age` from the latest raw points rather than intercepts.
`kcl`/`clk`/`cl36` need the chlorine inputs above; when `MetricCatalog::chlorine`
is false (the default) static validation reports them as unavailable.

## 6. Runtime

### 6.1 In-run (MeasurementEngine)

- Main signal readings: modification, truncation, action, termination,
  cancelation; sniff readings: equilibration. First trip wins (L4).
- Truncation: end main; scale later collections by the conditional's ratio
  (L17).
- Action: perform; `resume = false` ends the current collection block
  without marking the run truncated (L18).
- Termination: outcome Terminated, later blocks skipped, data kept (L15).
- Cancelation: outcome Cancelled, `cancel_queue = true` (L16).
- Equilibration: close the inlet now (L19).
- Modification: reported in `MeasurementResult::modifications` with its
  flags; `truncate` / `terminate` take effect in-run (L20).
- Whiff block (L26).

### 6.2 Between runs

- `apply_modification(ExperimentQueue&, current_row, const ActionSpec&,
  const BlankFactory&)` performs a queue action and reports what changed.
- `RunChecks` holds the merged pre-run and post-run sets for a queue and keeps
  trip state across runs. `pre_run(run, ctx)` and `post_run(run, record)`
  return the trips; the executor cancels the queue on a pre-run trip and
  applies post-run actions.

### 6.3 Record

`record::Conditionals` v2 (schema version bump):

```
installed[]: { id, name, kind, level, location, check, start, frequency, ntrips, window,
               mapper, analysis_types[], abbreviated_count_ratio, action, resume }
tripped[]:   { id, name, kind, reading, t, value, count, action, context{metric: value} }
errors[]:    { name, message }
```

## 7. Tooling

1. `tools/pychron_conditionals_import.py`: legacy YAML (system, queue, run
   files) -> TOML. Handles `teststr`/`comp`/`check` precedence, legacy
   defaults, `.current`, `CTRL.GAUGE.pressure` -> `gauge.GAUGE.pressure`
   (flagged), spaces, `window`, `mapper`, `analysis_types`,
   `abbreviated_count_ratio`, `resume`, action snippets -> `run_hook` + stub
   (flagged), modification names and flags, `pre_run_terminations` /
   `post_run_terminations` / `post_run_actions`, `equilibrations`. A report
   lists every rewrite and anything not converted. pytest fixtures are the
   legacy docs/test YAML.
2. `elctl exp conditionals check FILE`: parse, transform, statically validate
   against the lab's isotopes/detectors/gauges/devices; print the canonical
   form.
3. Static validation API: `validate(set, MetricCatalog)` -> diagnostics.

## 8. Testing

- Grammar: every row of the feature table, round-trip, errors.
- Gating: start/frequency/ntrips/resume against the legacy arithmetic; first
  trip wins; analysis_types incl. `blank`.
- Transforms: window and mapper produce the expected AST.
- Metrics: collector intercepts/corrections, instrument adapter, record
  metrics, Ar-Ar computed values against a hand-computed fixture.
- Engine: ratio scaling, action resume/break, cancel_queue, equilibration,
  whiff run_remainder/pump/abort, provenance in the result.
- Queue actions: each of the seven on a small queue, including set extract
  absolute and percent.
- RunChecks: ntrips across runs, pre/post flows.
- Record: v2 round trip; v1 rejected.
- Importer: legacy fixtures -> expected TOML and report.

## 9. Out of scope

Blank subtraction in-run,
the executor itself (it consumes 6.2). UI editors were out of scope here and
are specified in `2026-10-03-conditionals-editor-design.md`.
