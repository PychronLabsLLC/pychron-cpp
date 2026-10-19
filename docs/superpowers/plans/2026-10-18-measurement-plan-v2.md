# Measurement Plan v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A measurement plan becomes gases of segments of positions, so one plan file can express per-position detector settings, baselines and peak centers anywhere, a baseline taken while the sample heats, several gases in one run, and the owner's staged cryotrap release (row T027), all validated before the queue starts and recorded in one `AnalysisRecord`.

**Architecture:** The plan parser rewrites every v1 table into v2 segments before validation, so the engine has one input form. The engine stops being a fixed block order: it walks each gas's segments grouped by `when`, runs one position executor for every collection, and runs steps (`peak_center`, `set`, ...) and the staged-release blocks (`prepare`, `pump_out`) between them. The collector keys series by `(label, detector, kind, gas, segment, position)`, and `AnalysisRecord` v3 carries per-gas time zero, a per-gas table of spectrometer states and every baseline position, while v2 records keep loading with their sha.

**Tech Stack:** C++20, toml++, GoogleTest, the `libs/experiment` measurement engine and plan loader, `libs/systems` spectrometer facade and config, `libs/sim` simulated lab, `libs/scripting` script host, Python 3 + pytest for the legacy converter.

**Spec:** `docs/superpowers/specs/2026-10-05-measurement-plan-v2-design.md` (agreed 2026-10-15). Read it first; section numbers below (§) are its sections. Section 11 is the staged gas release, item 6b of its order of work.

## How this plan is cut

The spec's §9 lists eight items, each "a spec unit of its own". This plan keeps that order as phases. Each phase ends with the tree green and can land on `develop` alone (its own branch, named for the phase, cut from `develop` per `AGENTS.md`). Row T027 (staged release) is Phase 7; it needs Phases 1, 2, 3 and 6 first, because a `prepare` lives on a `[[gas]]` and its data is recorded per gas.

| Phase | Spec item | Tasks |
|---|---|---|
| 1 Schema | 1 | 1-5 |
| 2 Positions | 2 | 6-10 |
| 3 Record v3 and keys | 3 | 11-15 |
| 4 Steps and hooks | 4 | 16-18 |
| 5 Phases (`when`) | 5 | 19-21 |
| 6 Gases | 6 | 22-23 |
| 7 Staged gas release (T027) | 6b | 24-28 |
| 8 Optics, interferences, drivers | 7 | 29-31 |
| 9 Legacy converter | 8 | 32 |
| 10 Branch close | | 33 |

Until a phase lands, the plan loader refuses the keys whose behaviour that phase builds (Task 5, `unbuilt_features`): a plan that validates always runs as written. Each phase deletes its own entries from that list.

## What the code says

Read from `develop` `08987e39`.

- Plan types: `libs/experiment/include/pychron/experiment/plan/plan.hpp` (`MeasurementPlan` with v1 sections `sniff`, `peak_center`, `baseline`, `main`, `whiff`; helpers `hop_target`, `baseline_target`, `active_detectors`, `signal_fit`, `baseline_fit`).
- Loader pipeline (`plan_loader.hpp`): `parse_plan_template` → `render_effective_plan` (applies `parameters.expose` overrides to the TOML text) → `resolve_plan` (typed parse and every schema rule, `src/plan/plan_parse.cpp:472`) → `load_plan`. Errors: one `Problems` per resolve (`src/plan/plan_toml.hpp:20`), each line `<source>: <path>: <msg>`, `ErrorKind::Config`. Unknown keys refused by `Section::keys` (`plan_parse.cpp:106`); top-level allowed list at `plan_parse.cpp:486`.
- Overrides are applied to the TOML before the v1 tables are read, so `main.hops[i].counts` keeps working for a v1 file without any alias. The §4.1 "path alias table" is needed only where a path addresses the *resolved* plan (live edits, Task 15).
- Detector check: `check_detectors(plan, ISpectrometerCatalog, Problems)` (`plan_parse.cpp:452`), catalog interface `plan_loader.hpp:33` (`has_detector`), real one `SpectrometerCatalog` (`measurement/adapters.hpp:92`) from `cfg::SpectrometerConfig::detectors`.
- Engine: `measurement/engine.hpp`, one `HopRunner`-style path (`move_to`, `collect`), `Block` enum of seven values published as `BlockStarted`/`BlockFinished`; consumers of `BlockStarted`: `apps/pychron-ui/src/executor_pane.cpp`, `experiment_bridge.{hpp,cpp}`, `tests/experiment/test_executor.cpp`.
- Ports (`measurement/ports.hpp`): `ISpectrometerPort` has `position`, `protect`, `start_acquisition`, `next_reading`, `stop_acquisition` only. The facade (`libs/systems/include/pychron/systems/spectrometer/spectrometer.hpp:165`) already has `move_native`, `position_hv`, `with_table`, `update_table`, `save_table`, `set_deflection`, `set_gain`, `set_cdd_voltage`, `set_hv`, `protect`; it has no integration setter (integration goes with `start_acquisition`), no optics and no source-output call.
- Users of v1 plan fields outside the plan code: `engine.cpp`, `duration.cpp`, `run/run.cpp`, `tests/setup/test_profiles.cpp`, `tests/experiment/test_run_factory.cpp`, `tests/integration/test_measurement_sim.cpp`.
- Collector: `SeriesKey{isotope, detector, SeriesKind}` with a closed `SeriesKind` (`collect/collector.hpp:39`). Intercepts keyed by isotope, `Ar36:CDD` only on collision (`measurement/results.hpp:25`); `record/names.hpp` is the one place mapping record names to analysis/store names (`libs/experiment/AGENTS.md`).
- Record: `kRecordSchemaVersion = 2` (`record/types.hpp:23`); `tests/data/records/v2.{toml,json}` hold the v2 sha. Store `baseline_value` has primary key `(revision_uuid, detector)` and an `extra` JSON column and `modifier_value`/`modifier_error` (`libs/persistence/migrations/sqlite/0001_init.sql:472-491`); ingest has `baseline_modifiers_json` with no producer (`libs/persistence/src/ingest.cpp:63`).
- Run: `Run::extract` (`run/run.cpp:426`) acquires and releases the extraction resource around the extraction script (`:430`, `:438`); `Run::measure` (`:455`) acquires the spectrometer (`:458`) and builds the engine (`:499`). Extraction scripts run through `scripting::IScriptHost::run(script, env, token)`; scripts resolve with `scripting::IScriptResolver::resolve(name, kind)`; run variables reach a script through `Run::script_context()` (`:276`).
- Executor overlap: `Executor::overlaps` (`executor/executor.cpp:273`), waits on `overlap_ready` set by `RunHooks::on_overlap_ready`. Queue validation (`model/queue_validation.hpp:66`, `check_queue`) has no overlap rule yet.
- Cryo: `set_cryo`, `get_cryo_temp` exist in the script vocabulary (`libs/scripting/src/vocabulary.cpp:50`) and host (`host_state.cpp:501`). The simulator has Lakeshore cryostats (`libs/sim/include/pychron/sim/sim_system.hpp:324`) but no trap that holds gas by temperature.
- Drivers: Qtegra (`libs/devices/src/spectrometer/thermo_qtegra.cpp`) and NGX exist. There is no Nu Noblesse driver in the tree (spectrometer-control spec puts it after Qtegra and NGX).
- Converter precedent: `tools/pychron_conditionals_import.py`, tests `tools/tests/test_pychron_conditionals_import.py` (pytest, fixtures under `tools/tests/fixtures/`).
- Test targets: `pychron_experiment_tests`, `pychron_experiment_store_tests`, `pychron_integration_tests`, `pychron_systems_tests`, `pychron_sim_tests`, `pychron_scripting_tests`, `pychron_processing_tests`, `pychron_processing_records_tests`, `pychron_devices_tests`, `pychron_setup_tests`, `pychron_install_e2e_tests`. Plan fixtures: `tests/experiment/fixtures/plans/{valid,invalid}`; every invalid fixture starts with `# expect: <text>` and is run by `PlanSchema.EveryInvalidFixtureFailsWithItsRule`.

## Decided here

Settled from the code and `AGENTS.md`; not owner questions.

- **Store keeps one baseline per detector.** A run with several baseline positions on one detector stores, as that detector's `baseline_value.value/error`, the baseline chosen (§6.2 rule) for the detector's first signal series in record order; every baseline position and every per-series choice go into the row's `extra` JSON (`{"positions": [...], "choices": {"Ar40@H1": {...}}}`). No migration (`libs/persistence/AGENTS.md`: a migration costs every lab a step). The records path reads the per-series choice exactly.
- **Store names unchanged.** Record v3 keys intercepts `label@detector` always; `record/names.hpp` maps `Ar40@H1` to the analysis name `Ar40` when Ar40 has one detector and `H1:Ar40` when it has several, as today. `RecordIngest.AnOrdinaryRunConvertsAsItAlwaysDid` is not edited.
- **v2 records keep their sha.** A record read with `schema_version = 2` is written back, and its sha computed, by the v2 writer; only new records are v3. `tests/data/records/v2.*` do not change; `tests/data/records/v3.*` are added and checked on Windows CI (root `AGENTS.md`: a writer of a stored format is checked against a fixture written elsewhere).
- **Noblesse optics wait for a Noblesse driver.** §5.3: a driver without a capability reports it at plan load. Phase 8 adds optics to Qtegra; a Noblesse plan using `optics` is refused at validation until that driver exists.
- **Simulated engine trace.** The engine records what it did as `MeasurementResult::trace` (Task 6), so the §8 trace assertions and the v1-equals-v2 property test compare one structure, not fake journals.
- **Converter fixtures are written here**, in the legacy formats, one per legacy example §8 names; `NMGRL/pychron` is not fetched by a test.
- **A failed `write_table` peak center** leaves positions that name that table on the spectrometer's current table, with a run note; the run goes on (legacy `generate_ic_mftable` behaviour: the IC data is still worth having).

## Global Constraints

- Every v1 plan file valid today stays valid and resolves to the documented segments (§4.1); `[main]` and `[[segment]]` may not both appear at top level; `[[gas]]` and top-level segments may not both appear; unknown keys at any level remain errors.
- Errors are `pychron::Result<T>` values; time goes through `pychron::Clock`; engine waits go through `EngineOptions::sleep` / the cancel token, never a real sleep (root `AGENTS.md`, Time).
- `libs/experiment` gains no dependency on `reduction` beyond what it has, none on `processing`, and never links `libs/persistence` (`libs/experiment/AGENTS.md`).
- Record: every new `AnalysisRecord` field gets a row in `tests/experiment/test_record_ingest.cpp` or a place in the e2e spec's not-stored list; `to_ingest` stays pure; ids stay `Uuid::v5(<record uuid>, label)` with today's labels.
- A thread added or shared (Task 20) is run under `-DPYCHRON_SANITIZE=thread` before it lands.
- Read the component file before touching its paths: `libs/experiment/AGENTS.md`, `libs/sim/AGENTS.md` (Task 28), `libs/devices/AGENTS.md` (Task 31: every new command gets a row in `test_repeat_tables.cpp`), `libs/processing/AGENTS.md` (Task 30), `libs/persistence/AGENTS.md` (Task 13), `libs/setup/AGENTS.md` (anything under `profiles/` or `configs/examples`).
- A `sim.toml` key is read and range-checked in `sim_config.cpp`, listed commented out with its default in `configs/examples/sim.toml`, and given a row in `docs/simulator.md`.
- Per task: build only the touched target, run its binary filtered; `python3 tools/quality_check.py` exit 0; `python3 tools/compiler_sweep.py --changed` (with `--build-dir build/dev-ui` for `apps/pychron-ui`); `ctest --preset dev-quick` when the change reaches other components. Full `cmake --build --preset dev[-ui]`, `ctest --preset dev[-ui]` and a whole-tree sweep run once, in Task 33 (and at the end of each phase that lands alone).
- Commits: Conventional Commits, component as scope (`feat(experiment): ...`, `feat(systems): ...`, `feat(sim): ...`, `feat(tools): ...`); code, comments and commit messages in normal prose.

## Review Focus

Inputs the spec implies but no worked plan exercises, most likely first. Each has its test in the owning task.

1. A peak center step with `write_table` fails (`ok = false`) and later positions name that table: positions run on the current table, the run notes it, the run is not failed (Task 16, `PeakCenterStep.FailedWriteTableFallsBackToCurrentTable`).
2. Cancel or abort while a gas's `prepare` script runs: the script is cancelled through the shared token, no inlet is open, the run ends Cancelled/Aborted and the gases already measured are in the result and saved (Task 25, `StagedRelease.CancelDuringPrepareKeepsEarlierGases`; Task 26).
3. The extraction fails while the `during_extraction` baseline is collecting: the baseline phase stops, the spectrometer is released, the run fails as an extraction failure does today (Task 20, `DuringExtraction.ExtractionFailureStopsTheBaselinePhase`).
4. Large-beam state crosses segment and gas boundaries: a He signal saturating `Faraday` makes the first Ne position on `Faraday` wait `settle_after_large_beam_s` (Task 9, `LargeBeamSettle.StateCarriesAcrossSegments`).
5. A live edit of `cycles` or `counts` during a multi-position, multi-segment plan changes the running segment only, not later segments (Task 15, `LiveEdit.CyclesChangeOnlyTheRunningSegment`).

---

## Phase 1: Schema (§3, §4.1; spec item 1)

### File structure

| File | Responsibility |
|---|---|
| `libs/experiment/include/pychron/experiment/plan/plan.hpp` | v2 types (`Gas`, `Segment`, `Position`, `CollectEntry`, `At`, step tables, `When`), helpers |
| `libs/experiment/src/plan/plan_parse.cpp` | v2 parse, v1 desugar, §3.7 rules |
| `libs/experiment/src/plan/plan_desugar.cpp` (new) | v1 → v2 rewrite (§4.1 table), one function per v1 table |
| `libs/experiment/include/pychron/experiment/plan/plan_json.hpp`, `src/plan/plan_json.cpp` (new) | Deterministic JSON of a resolved plan for goldens |
| `libs/experiment/include/pychron/experiment/plan/plan_loader.hpp` | `ISpectrometerCatalog` gains label masses, capabilities, optics and table names |
| `libs/experiment/src/measurement/adapters.cpp` | `SpectrometerCatalog` answers the new catalog calls |
| `tests/experiment/test_plan_v2.cpp` (new) | v2 parse, desugar, validation, goldens, equivalence |
| `tests/experiment/fixtures/plans/valid/v2_*.toml`, `.../invalid/v2_*.toml`, `.../resolved/*.json` (new) | Worked plans 7.1-7.7, one invalid fixture per new rule, goldens |

### Task 1: v2 types and the v1 desugar

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/plan/plan.hpp`
- Create: `libs/experiment/src/plan/plan_desugar.cpp`
- Modify: `libs/experiment/src/plan/plan_parse.cpp`, `libs/experiment/CMakeLists.txt`, `libs/experiment/include/pychron/experiment/plan/plan_loader.hpp` and `libs/experiment/src/measurement/adapters.cpp` (`isotope_of`)
- Test: `tests/experiment/test_plan_v2.cpp` (add to `tests/experiment/CMakeLists.txt`); `tests/experiment/test_plan_schema.cpp` (its fake catalog answers `isotope_of`)

**Interfaces:**
- Produces (namespace `pychron::experiment::plan`):
  - `enum class When { DuringExtraction, BeforeInlet, DuringEquilibration, AfterInlet, AfterMain, AfterRun };` `std::string_view to_string(When)`, `std::optional<When> parse_when(std::string_view)`; strings `during_extraction`, `before_inlet`, `during_equilibration`, `after_inlet`, `after_main`, `after_run`.
  - `enum class SegmentKind { Signal, Baseline, Sniff, Whiff, PeakCenter, Set, BlankBeam, Park, Delay, TimeZero };` with `to_string`/`parse_segment_kind` (`peak_center`, `blank_beam`, `time_zero` spelled as in §3.3); `bool is_step(SegmentKind)`.
  - `enum class PositionKind { Signal, Baseline };`
  - `struct CollectEntry { std::string label, detector; bool active = true; std::optional<double> deflection; bool protect = false; };`
  - `struct AtLabel { std::string label, detector; }; struct AtMass { double mass = 0; std::string detector; }; struct AtDac { double dac = 0; }; struct AtHv { double hv = 0; std::string detector; }; using At = std::variant<AtLabel, AtMass, AtDac, AtHv>;`
  - `struct Position { std::vector<CollectEntry> collect; std::optional<At> at; PositionKind kind = PositionKind::Signal; int counts = 0; double settle_s = 0; std::optional<double> integration_s; std::string optics, table; };`
  - `TimeZeroKind` gains `Now` (step only).
  - `struct FitOverride { std::optional<reduction::FitKind> fit; std::optional<reduction::ErrorType> error; std::optional<reduction::OutlierSpec> outliers; };` `using FitOverrides = std::map<std::string, FitOverride>;` (keys `label@detector`, `label`, `default`)
  - Step tables: `struct PeakCenterStep { std::string label; std::vector<std::string> detectors; std::vector<std::string> extra_detectors; std::string config = "default"; std::string write_table; double min_signal = 0; };` `struct SetStep { std::map<std::string, double> deflection, gain, source; std::optional<double> hv, cdd_voltage, integration_s; };` `struct ParkStep { std::string label; std::optional<double> mass; std::string detector; };`
  - `struct Segment { SegmentKind kind = SegmentKind::Signal; When when = When::AfterInlet; int cycles = 1; double integration_s = 1; std::optional<TimeZero> time_zero; std::string table; std::vector<Position> positions; FitOverrides fits; std::vector<Truncation> truncations; std::vector<WhiffCheck> checks; std::optional<PeakCenterStep> peak_center; std::optional<SetStep> set; std::optional<bool> blank_beam; std::optional<ParkStep> park; std::optional<double> delay_s; };` (a `time_zero` step uses `time_zero` with kind `Now` or `Offset`; segment-level `table` is the default for its positions, as in worked plan 7.3)
  - `struct Gas { std::string name; Equilibration equilibration; bool source_dark_phase = false; std::optional<TimeZero> time_zero; std::optional<Fits> fits; std::vector<Segment> segments; bool implicit = false; };`
  - `MeasurementPlan` gains `std::vector<Gas> gases;` (always filled after resolve; v1 fields stay filled for v1 files until Task 6 removes them).

- [x] **Step 1: Write failing desugar tests**, one per §4.1 row, each resolving inline v1 TOML with `resolve_plan` and asserting on `plan.gases`:
  - `PlanDesugar.MainBecomesOneSignalSegment`: `[main] cycles=3, integration_s=2` with two hops → `gases.size()==1`, `gases[0].implicit`, one segment `kind==Signal`, `when==AfterInlet`, `cycles==3`, `integration_s==2`, two positions, `positions[0].collect` equals the hop's `positions` map as entries in map order.
  - `PlanDesugar.ProtectBecomesEntriesAndInactiveEntries`: hop `positions={Ar40="H1"}`, `protect=["H1","CDD"]` → entry H1 `protect==true, active==true`; added entry `{label=<see rule>, detector="CDD", active=false, protect=true}`. Label rule for the added entry (§3.7 requires one that resolves to a mass): the label the plan collects on that detector in another hop; else the detector's configured `isotope` (Task 1 adds `virtual std::optional<std::string> isotope_of(std::string_view detector) const = 0;` to `ISpectrometerCatalog`, answered from `DetectorConfig::isotope`); else, with no catalog, the detector name (the mass check needs a catalog and is skipped without one). Assert the first case here and the second with the test catalog.
  - `PlanDesugar.BaselineHopBecomesBaselinePosition`: `baseline=true, mass=34.2` on reference H1 → `kind==PositionKind::Baseline`, `at==AtMass{34.2,"H1"}`.
  - `PlanDesugar.BaselineBeforeAfterBecomeSegments`: `[baseline] before=true, after=true, counts=30, mass=34.2` → a `Baseline` segment at `BeforeInlet` and one at `AfterMain`, each one position collecting every detector of any signal position, `at==AtMass{34.2, reference}`.
  - `PlanDesugar.PeakCenterBecomesSteps`: `[peak_center] before=true, after=true, isotope="Ar40", min_signal=5` → `PeakCenter` step at `BeforeInlet` (`min_signal==0`) and at `AfterRun` (`min_signal==5`).
  - `PlanDesugar.SniffAndWhiffBecomeSegments`: `[sniff]` → `Sniff` at `DuringEquilibration` on first signal position's detectors; `[whiff]` → `Whiff` at `AfterInlet` placed before the signal segment, `checks` copied.
  - `PlanDesugar.ExcludeRemovesFromEveryCollect`: `[detectors] exclude=["CDD"]` → no entry with detector CDD anywhere.
- [x] **Step 2: Run, verify they fail** — `cmake --build --preset dev --target pychron_experiment_tests && build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='PlanDesugar.*'` → compile errors on `gases`.
- [x] **Step 3: Add the types to `plan.hpp`** (each with defaulted `operator==`) **and `Result<std::vector<Gas>> desugar_v1(const MeasurementPlan& v1, const ISpectrometerCatalog* catalog)` in `plan_desugar.cpp`** (internal header `src/plan/plan_desugar.hpp`), called from `resolve_plan` after the v1 typed parse and before the rules.
- [x] **Step 4: Run** the filter above and the whole binary's `PlanSchema.*:PlanOverrides.*:PlanDuration.*:PlanLibrary.*:PlanParameters.*` → all pass (v1 behaviour untouched).
- [x] **Step 5: Commit** `feat(experiment): v2 plan types and the v1 desugar`.

### Task 2: Native v2 parse

**Files:**
- Modify: `libs/experiment/src/plan/plan_parse.cpp`
- Test: `tests/experiment/test_plan_v2.cpp`; fixtures `tests/experiment/fixtures/plans/valid/v2_71_argus_syn_baseline.toml` … `v2_77_helix_xenon.toml` (worked plans §7.1-7.7 verbatim, each given the `[plan]` header `name`, `instrument_family`, `analysis_types = ["unknown"]` and `[equilibration]` the fixture `multicollect.toml` uses)

**Interfaces:**
- Consumes: Task 1 types.
- Produces: `resolve_plan` accepts top-level `[[segment]]` (with `[[segment.positions]]`) and `[[gas]]` (with `[[gas.segment]]`, `[[gas.segment.positions]]`); allowed top-level keys add `segment`, `gas`. `collect` accepts the list form and the shorthand map (map order = TOML key order as toml++ iterates; documented as such). `at` accepts exactly one of the four forms.

- [x] **Step 1: Write failing tests:**
  - `PlanV2Parse.EveryWorkedPlanResolves`: loop over the seven `v2_7*.toml`, `load_plan` with the test catalog (Task 3 extends it; until then `kResolvers` with no spectrometer) → `ok()`.
  - `PlanV2Parse.CollectShorthandEqualsListForm`: same position written both ways → equal `Position`.
  - `PlanV2Parse.AtForms`: `{label,detector}`, `{mass,detector}`, `{dac}`, `{hv,detector}` → the matching `At` alternative with values.
  - `PlanV2Parse.StepTables`: one segment of each step kind from §3.3 → each table's fields; `peak_center.detectors=["H1","AX","L2"]`, `write_table="ic_run"`; `set.deflection.CDD==2000`; `delay.seconds` → `delay_s`; `time_zero="now"` → `TimeZeroKind::Now`.
  - `PlanV2Parse.GasesInFileOrder`: worked plan 7.6 → `gases` names `He, Ne, Ar`, `implicit==false`, He `time_zero` OnInletClose, Ne `equilibration.time_s==30`.
  - `PlanV2Parse.UnknownKeysAtEveryLevel`: unknown key in `[[gas]]`, `[[segment]]`, `[[segment.positions]]`, a `collect` entry, `at`, each step table → error containing `unknown key '<k>'` and its path (`segment[0].positions[1]`).
- [x] **Step 2: Run** `--gtest_filter='PlanV2Parse.*'` → FAIL.
- [x] **Step 3: Implement** the readers in `plan_parse.cpp` with the existing `Section::keys` and `Problems` (paths `segment[i]`, `gas[g].segment[i].positions[j].collect[k]`).
- [x] **Step 4: Run** → PASS; plus `PlanSchema.*` → PASS.
- [x] **Step 5: Commit** `feat(experiment): parse v2 segments, positions and gases`.

Done notes: fixture 7.1 leaves out its `[baseline.modifier.CDD]` table until Task 21 reads it (Task 21 puts it back). Also built here, since the parse needs them to be unambiguous: the two conflict errors Task 3's fixtures `v2_main_and_segment.toml` and `v2_gas_and_segment.toml` expect (same messages) and `[main] and [[gas]] cannot both appear`, the refusal of v1 block sugar in a plan of segments (spec §3.5), and shape errors (`at` forms, step table on its own kind only, collection keys refused on a step, `gas '<n>' named twice`; a gas's `fits` overrides the plan's key by key and a gas without one carries the plan's), tested by `PlanV2Parse.ShapeErrors`. v2 detectors are not checked against the catalog yet (Task 3), and `[detectors] exclude` is not yet applied to v2 positions (Task 3, `v2_exclude_empties_position.toml`).


### Task 3: Validation additions (§3.7) and the catalog

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/plan/plan_loader.hpp`, `libs/experiment/src/plan/plan_parse.cpp`, `libs/experiment/include/pychron/experiment/measurement/adapters.hpp`, `libs/experiment/src/measurement/adapters.cpp`
- Test: `tests/experiment/test_plan_v2.cpp`, `tests/experiment/test_plan_schema.cpp` (its `FakeSpectrometer` catalog gains the new calls); new invalid fixtures `tests/experiment/fixtures/plans/invalid/v2_*.toml`

**Interfaces:**
- Produces: `ISpectrometerCatalog` gains (pure virtual):
  - `virtual std::optional<double> mass_of(std::string_view label) const = 0;` (molecular weights table, which already takes molecules and pseudo-isotopes: `PHHCl`, `Ar40++`, `CO2` are lab entries in `molecular_weights.toml`)
  - `virtual bool supports_deflection(std::string_view detector) const = 0;` (config `Deflection::control`)
  - `virtual bool has_optics(std::string_view name) const = 0;` (false until Phase 8)
  - `virtual bool has_table(std::string_view name) const = 0;` (named field tables loaded with the spectrometer)
  - `virtual bool positions_by_dac() const = 0; virtual bool positions_by_hv() const = 0;`
- `PlanResolvers` gains `std::function<bool(std::string_view analysis_type)> has_extraction;` (null: every type has one; the factory's analysis-type table answers in `Run`/`check_queue`).

- [x] **Step 1: Write one invalid fixture per rule**, each starting `# expect: <text>`:
  - `v2_detector_twice.toml` — `# expect: detector 'H1' appears twice in position`
  - `v2_inactive_without_label.toml` — `{ detector = "CDD", active = false, protect = true }` — `# expect: collect entry needs a label`
  - `v2_label_without_mass.toml` — label `Foo99` — `# expect: label 'Foo99' has no mass`
  - `v2_at_unresolved.toml` — reference not in collect, no `at` — `# expect: needs 'at'`
  - `v2_unknown_optics.toml` — `# expect: unknown optics 'nope'`
  - `v2_unknown_table.toml` — `# expect: unknown table 'nope'`
  - `v2_table_before_written.toml` — position uses `ic_run` before the `peak_center` step that writes it — `# expect: table 'ic_run' is used before it is written`
  - `v2_deflection_unsupported.toml` — `# expect: detector 'L2' has no deflection control`
  - `v2_during_extraction_signal.toml` — `# expect: no signal or sniff segment during_extraction`
  - `v2_during_extraction_no_extraction.toml` — `analysis_types=["air"]` with `has_extraction` false for air — `# expect: during_extraction needs an analysis type with an extraction`
  - `v2_two_time_zeros.toml` — `# expect: time_zero set twice in gas`
  - `v2_main_and_segment.toml` — `# expect: [main] and [[segment]] cannot both appear`
  - `v2_gas_and_segment.toml` — `# expect: [[gas]] and top-level [[segment]] cannot both appear`
  - `v2_exclude_empties_position.toml` — `# expect: position has no detector left after exclude`
  - `v2_time_zero_not_first_signal.toml` — `time_zero` on the second `after_inlet` signal segment — `# expect: time_zero belongs on the first after_inlet signal segment`
- [x] **Step 2: Run** `--gtest_filter='PlanSchema.EveryInvalidFixtureFailsWithItsRule'` → FAIL on each new file.
- [x] **Step 3: Implement** the rules in `plan_parse.cpp` (`check_v2(const MeasurementPlan&, const PlanResolvers&, Problems&)`, run on `plan.gases`, so v1 files go through the same rules after desugar), the catalog calls in `SpectrometerCatalog` (it now also holds `MolecularWeights` and table names: constructor `SpectrometerCatalog(const cfg::SpectrometerConfig&, const MolecularWeights&, std::vector<std::string> table_names)`), and update its two construction sites (grep `SpectrometerCatalog(`).
- [x] **Step 4: Run** the invalid-fixture test, `PlanV2Parse.*`, `PlanSchema.*` and `pychron_integration_tests --gtest_filter='MeasurementSim.ShippedConfigsResolveTheExamplePlan'` → PASS.
- [x] **Step 5: Commit** `feat(experiment): validate v2 plans against the spectrometer`.

Done notes: `SpectrometerCatalog` takes `spectrometer::cfg::MolecularWeights` (the label -> mass map `SpectrometerData::weights` holds, defaults merged with the lab's file), not the `spectrometer::MolecularWeights` class; `measurement::table_names(const SpectrometerData&)` gives its table names, at both construction sites. `positions_by_dac` is true when the magnet has a positioner whose native axis is `dac`, `positions_by_hv` when a source driver is configured; `has_optics` is false. Rules settled here (spec §3.7 updated): the `at` default is checked only when the plan names `[detectors] reference` (with none, the spectrometer's reference positions it, which Task 6's `position_target` answers), and an entry on one of the reference's modes (`H2(CDD)` for `H2`, worked plan 7.4) counts as the reference. A v1 file is held to the label-mass rule only, on its signal hops (`main.hops[i].positions.<label>`) and its peak center isotope: its reader already holds it to the reference, exclude and detector rules, and the inactive entry the desugar adds for a protected detector with no configured isotope is labelled with the detector's name, which has no mass, so checking it would refuse a v1 file valid today (`PlanV2Rules.AV1ProtectedDetectorWithNoIsotopeStaysValid`). The time-zero rules report both "set twice" and "belongs on the first after_inlet signal segment" when both hold. Without a catalog only the plan-internal rules run (a table used before the step that writes it is still refused). `PlanResolvers::has_extraction` is read by the rule but nobody sets it yet: no analysis-type table says which types extract; Task 20, which runs `during_extraction`, sets it where `Run` and `check_queue` resolve plans; a plan with no `analysis_types` names no type to ask about, so there the run's own type is the one to check. Worked plans now resolve against the test catalog (`kWorkedResolvers`), and `PlanV2Rules.*` covers what the fixtures do not.

### Task 4: Resolved-plan goldens and the v1 = v2 equivalence

**Files:**
- Create: `libs/experiment/include/pychron/experiment/plan/plan_json.hpp`, `libs/experiment/src/plan/plan_json.cpp`
- Create: `tests/experiment/fixtures/plans/resolved/<fixture stem>.json` for every file in `valid/`; `tests/experiment/fixtures/plans/valid/v2_spelling_of_multicollect.toml`, `v2_spelling_of_peak_hop_cdd.toml`
- Test: `tests/experiment/test_plan_v2.cpp`

**Interfaces:**
- Produces: `std::string to_json(const MeasurementPlan& plan);` — `nlohmann::json` with sorted keys, 2-space indent, trailing newline; serializes `info`, `detectors`, `gases` (every field, enums as their strings), `fits`, `conditionals`, `expose`, `hook`; not the v1 fields.

- [x] **Step 1: Write failing tests:**
  - `PlanGoldens.EveryValidFixtureMatchesItsGolden`: for each `valid/*.toml`, `to_json(load(...).plan)` equals `resolved/<stem>.json` byte for byte; when env `PYCHRON_UPDATE_GOLDENS=1` the test writes the file instead and fails with "golden rewritten: review the diff".
  - `PlanGoldens.V1EqualsItsV2Spelling`: `multicollect.toml` and `v2_spelling_of_multicollect.toml` resolve to equal `gases` (and the same for `peak_hop_cdd`).
- [x] **Step 2: Run** `--gtest_filter='PlanGoldens.*'` → FAIL.
- [x] **Step 3: Implement `to_json`**, write the two v2 spellings by hand from the §4.1 table, generate goldens with `PYCHRON_UPDATE_GOLDENS=1`, and read each golden against its fixture before committing.
- [x] **Step 4: Run** → PASS.
- [x] **Step 5: Commit** `test(experiment): resolved-plan goldens for every plan fixture`.

### Task 5: Duration over gases, and the unbuilt-feature gate

**Files:**
- Modify: `libs/experiment/src/plan/duration.cpp`, `libs/experiment/src/plan/plan_parse.cpp`
- Test: `tests/experiment/test_plan_schema.cpp` (`PlanDuration`), `tests/experiment/test_plan_v2.cpp`

**Interfaces:**
- Produces: `estimate_duration(const MeasurementPlan&, const DurationOptions&)` walks `plan.gases` (per gas: equilibration `inlet_delay_s + time_s`; per segment `cycles x Σ positions (settle_s + counts x integration)`; `delay` steps their seconds; `peak_center` steps `options.peak_center` each, times detectors). `main_duration` = the gas's `after_inlet` signal segments.
- `std::vector<std::string> unbuilt_features(const MeasurementPlan& plan);` (internal, `plan_parse.cpp`): one string per v2 feature the engine does not run yet; `resolve_plan` turns each into `<source>: <path>: <feature>: not run by this build yet`. Initial list: per-position `deflection`/`optics`/`table`/`integration_s`, `at` forms other than `AtLabel`/`AtMass`, `active=false` without `protect`, steps (`set`, `blank_beam`, `park`, `delay`, `time_zero`, `peak_center` with `detectors[]`, `extra_detectors` on a step not at before/after or `write_table`), any `when` other than the v1 placements, more than one gas, `source_dark_phase`, segment `fits`, segment `conditionals`. Task 24 adds `prepare` and `pump_out` when it creates those keys; Task 25 removes them. A v1 file never trips it.

- [x] **Step 1: Write failing tests:** `PlanDuration.V1DurationsUnchanged` (every existing `PlanDuration` expectation, now computed from `gases`); `PlanDuration.WorkedPlan72` (20 cycles x [(6+15) + (5+15) + (3+8)] s = 1040 s, plus the after_main baseline 20 + 120 = 140 s, plus equilibration); `PlanV2Gate.UnbuiltFeatureIsRefusedWithItsPath` (worked plan 7.4 → error mentions `segment[0].positions[1].collect[0]` and `deflection: not run by this build yet`).
- [x] **Step 2: Run** `--gtest_filter='PlanDuration.*:PlanV2Gate.*'` → FAIL.
- [x] **Step 3: Implement** both. `PlanV2Parse.EveryWorkedPlanResolves` and `PlanGoldens.*` call an internal `resolve_plan_ungated` (declared in `src/plan/plan_parse.hpp`, visible to tests through the test target's include path) so the schema stays tested while the gate stands.
- [x] **Step 4: Run** the whole `pychron_experiment_tests` binary → PASS.
- [x] **Step 5: Commit** `feat(experiment): plan duration over gases; refuse v2 features not yet run`.

Done notes: the estimate now counts the equilibration as `inlet_delay_s + time_s`, as this task's interface says and as the engine waits (it opens the inlet `inlet_delay_s` after the block starts); the v1 estimate left the delay out, so `multicollect.toml` is 591 s, not 588 s (`PlanDuration.FullSequence`, `PlanLibrary.ResolvesPlansForQueueValidation`), and every other v1 expectation is unchanged. Sniff segments and a whiff at `after_inlet` count inside that window (the v1 engine takes the whiff right after the inlet opens), adding time only when their readings take longer than it. A position with no `at` and no `[detectors] reference` in its collect list is identified by its collect list (so worked plan 7.2 settles at every position, and one such position times N cycles still settles once); `during_extraction` segments add nothing (they run with the extraction); `after_run` segments of every gas come after the last gas. The gate runs only on a plan that otherwise resolves, so an invalid fixture's message is its rule's alone. Paths: `segment[i]...` for top-level segments, `gas[g].segment[i]...` in a `[[gas]]` plan; the gate also names a segment-level `table` and a peak center outside before_inlet/after_run. Worked plan 7.7 trips nothing (`PlanV2Gate.WorkedPlansNeedingUnbuiltFeatures`); the two v2 spellings of v1 files pass the gate (`PlanV2Gate.AV1FileNeverTripsIt`). `resolve_plan_ungated` and `unbuilt_features` are in `src/plan/plan_parse.hpp`, namespace `detail`; `pychron_experiment_tests` has `libs/experiment/src` on its include path, and every schema test in `test_plan_v2.cpp` resolves ungated.

---

## Phase 2: Positions on the engine and ports (§3.2, §3.6, §5.2, §5.3; spec item 2)

### Task 6: The engine runs segments; v1 fields leave `MeasurementPlan`

Wider than two files: main thread or a general agent, not a cavecrew builder.

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/measurement/engine.hpp`, `libs/experiment/src/measurement/engine.cpp`, `libs/experiment/include/pychron/experiment/plan/plan.hpp`, `libs/experiment/src/plan/plan.cpp`, `libs/experiment/src/plan/duration.cpp`, `libs/experiment/src/plan/plan_parse.cpp`, `libs/experiment/src/plan/plan_desugar.cpp`, `libs/experiment/src/run/run.cpp`
- Modify tests: `tests/experiment/test_measurement_engine.cpp`, `tests/experiment/test_plan_schema.cpp`, `tests/experiment/test_run_factory.cpp`, `tests/setup/test_profiles.cpp`, `tests/integration/test_measurement_sim.cpp`

**Interfaces:**
- Produces:
  - `MeasurementPlan` loses `sniff`, `peak_center`, `baseline`, `main`, `whiff`; the v1 readers fill a private `V1Tables` in `plan_parse.cpp` that only `desugar_v1` reads. `hop_target`, `baseline_target`, `active_detectors` are replaced by `std::optional<At> position_target(const MeasurementPlan&, const Position&)` (the §3.2 `at` default rule) and `std::vector<CollectEntry> collecting(const Position&)` (active entries, file order).
  - `Block` becomes `enum class Block { PeakCenterBefore, BaselineBefore, PositionFirstHop, Equilibrate, Main, BaselineAfter, PeakCenterAfter, Segment, Step, Prepare, PumpOut };` A desugared v1 plan publishes exactly the v1 sequence (mapping: `peak_center` step at `before_inlet` → `PeakCenterBefore`, at `after_run` → `PeakCenterAfter`; baseline segment at `before_inlet` → `BaselineBefore`, at `after_main` → `BaselineAfter`; the first `after_inlet` signal segment → `Main`); every other segment publishes `Segment` and every other step `Step`. `to_string`: `"segment"`, `"step"`, `"prepare"`, `"pump_out"` for the new values.
  - `BlockStarted`/`BlockFinished` gain `std::string gas; int segment = -1;` (index within the gas; -1 for `Equilibrate`, `PositionFirstHop`).
  - `struct TraceEntry { std::string gas; int segment = -1; int position = -1; std::string what; std::string detail; double t = 0; friend bool operator==(const TraceEntry&, const TraceEntry&) = default; };` — `what` one of `move`, `settle`, `table`, `optics`, `deflect`, `protect`, `integration`, `collect`, `step`, `prepare`, `pump_out`; `t` seconds since the measurement epoch. `MeasurementResult` gains `std::vector<TraceEntry> trace;`.
  - One position executor: `Result<collect::CollectStatus> run_position(const plan::Gas&, std::size_t segment, std::size_t position, const plan::Position&, int counts)` replacing `move_to` + the hop loop.

- [x] **Step 1: Write the failing equivalence tests** in `test_measurement_engine.cpp`: `EngineV2.V1AndV2SpellingTraceEqual` (load `multicollect.toml` and `v2_spelling_of_multicollect.toml`, run each against `FakeSpectrometer` on a `ManualClock`, compare `result.trace`, `result.blocks`, the collected series and the final clock time); same for `peak_hop_cdd`.
- [ ] **Step 2: Run** `--gtest_filter='EngineV2.*'` → FAIL (no `trace`).
- [x] **Step 3: Rewrite `run_blocks`** to walk `plan.gases[0]` by phase in the v1 order (Phase 5 generalizes), every collection through `run_position`; port every v1 field use listed in "What the code says" to `gases`; tests that built `MeasurementPlan` by hand build it through `resolve_plan` from TOML (add a `plan_from(std::string_view toml)` helper in each test file that needs it).
- [x] **Step 4: Run** the whole `pychron_experiment_tests`, then `pychron_setup_tests`, `pychron_integration_tests --gtest_filter='MeasurementSim*'` → every existing test passes unchanged in its assertions (`EngineTest.RunsTheBlockSequenceInOrder`, `EngineTest.OneHopTimesNCyclesEqualsOneHopWithNTimesCounts` included); then `ctest --preset dev-quick`.
- [x] **Step 5: Commit** `refactor(experiment): the engine runs plan segments`.

Done notes: Step 2 was not observed failing: the equivalence tests were written in the same pass as the engine, so they ran green the first time. What the engine runs of a gas (spec 5.1 order, first gas only): `before_inlet` segments and steps in file order, the main segment's first position (`PositionFirstHop`), the equilibration with the gas's first sniff segment and first whiff segment, `before_main` hook, main (the first `after_inlet` signal segment), the gas's other `after_inlet` signal segments in file order (block `Segment`, counts scaled after a truncation, conditionals evaluated on their signal readings, no counts check of their own), `after_main` hook, `after_main`, `after_run`. Block names: the first segment of a v1 kind and placement takes the v1 block (`PeakCenterBefore`, `BaselineBefore`, `BaselineAfter`, `PeakCenterAfter`), the main segment `Main`; every other `Segment` or `Step`. Collections are counted under "main", "baseline.before", "baseline.after", "sniff", "whiff", or "segment[i]" (`<gas>.segment[i]` in a named gas). Time zero is the main segment's, else the gas's, else on_inlet_close. A baseline segment collects per detector with no label (series key `("", det, baseline)`, as v1); a baseline position inside a signal segment keeps its label. Sniff and whiff read where the magnet is (no move, as `estimate_duration` counts them); the v1 engine's whiff did the same. `MeasurementResult::trace` records `protect`, `move`, `settle`, `collect` and `step` (the other `what` values come with Task 8 and Phase 7); `t` is collector seconds. `plan::HopTarget` stays as the port's argument until Task 7; the engine turns `AtLabel`/`AtMass` into it and refuses `AtDac`/`AtHv` (gated). The gate gained what the engine does not run yet: a sniff or whiff segment with several positions, with `cycles`, or with an `at`; a second sniff or whiff segment; a whiff after the main segment; a gas with no `after_inlet` signal segment. Decided here from the old engine (spec 4.1 updated): an excluded detector that a hop protects stays in the position as `active = false, protect = true` (the v1 engine protected it; the v2 `[detectors] exclude` keeps such an entry the same way), and a hop positioned on an excluded reference gets that target as an explicit `at`. Departures from the v1 engine, accepted: a v1 `[baseline]` without `mass` is positioned by the section 3.2 default (the reference's label) instead of collecting wherever the magnet was; a hop protecting several detectors protects them in `collect` order, not `protect` order. `MeasurementPlan` keeps `equilibration` (the implicit gas copies it); the metrics exporter pre-creates its block counter for the four new `Block` values; the queue check reads every `peak_center` step's `config`, `Run`'s safe state unprotects every entry with `protect`.

### Task 7: Ports and adapters for per-position settings

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/measurement/ports.hpp`, `libs/experiment/include/pychron/experiment/measurement/adapters.hpp`, `libs/experiment/src/measurement/adapters.cpp`
- Test: `tests/experiment/test_measurement_engine.cpp` (`FakeSpectrometer` implements the new calls and journals them), `tests/integration/test_measurement_sim.cpp`

**Interfaces:**
- Produces on `ISpectrometerPort` (pure virtual):
  - `virtual Result<void> position(const plan::At& target) = 0;` (replaces the `HopTarget` overload; `AtDac` → `move_native`, `AtHv` → `position_hv`)
  - `virtual Result<double> set_deflection(const std::string& detector, double value) = 0;` `virtual Result<double> deflection(const std::string& detector) = 0;` (current value, for the restore rule)
  - `virtual Result<void> with_table(const std::string& name) = 0;` (empty name: the configured current table)
  - `virtual Result<void> set_gain(const std::string& detector, double gain) = 0;` `virtual Result<void> set_hv(double volts) = 0;` `virtual Result<void> set_cdd_voltage(double volts) = 0;` `virtual Result<void> set_param(const std::string& name, double value) = 0;` `virtual Result<void> blank_beam(bool on) = 0;`
  - `virtual Result<void> apply_optics(const std::string& name) = 0;` `virtual Result<void> set_source_output(bool on) = 0;` (adapter answers `ErrorKind::Config` "not supported by this spectrometer" until Phase 8)
  - Integration stays an argument of `start_acquisition` (the facade has no setter): "set integration" in §5.2 is the engine restarting acquisition with the new integration.
- `SpectrometerPort` maps each to the facade call named in "What the code says"; `with_table` keeps the returned `TableScope` alive until the next `with_table` call or the port's destruction.

- [x] **Step 1: Write failing tests:** `SpectrometerPortSim.DacAndHvPositioning` and `SpectrometerPortSim.DeflectionReachesTheBeam` in `test_measurement_sim.cpp` (real facade on the simulated drivers: `position(AtDac{x})` leaves `magnet_native()==x`; `set_deflection("CDD",110)` then `deflection("CDD")==110` and the simulated peak on CDD shifts, read through `BeamModel::intensity`).
- [x] **Step 2: Run** `pychron_integration_tests --gtest_filter='SpectrometerPortSim.*'` → FAIL.
- [x] **Step 3: Implement** the port calls, the adapter, the fake.
- [x] **Step 4: Run** → PASS; `pychron_experiment_tests` → PASS.
- [x] **Step 5: Commit** `feat(experiment): spectrometer port calls for per-position settings`.

Done notes: Step 2 was observed failing only as a build failure (the port had none of the calls); the tests were then run green with the adapter. The facade lacked two calls the port needs, so `Spectrometer` gained `blank_beam(bool)` (through the bound `IBeamBlank`, Config when none) and `read_deflection(det)` (reads the detector control back and records it: `DetectorState::deflection` is empty until something sets it, so the port's `deflection()` could not answer from the state alone); both tested in `tests/systems/test_spectrometer.cpp`. `set_cdd_voltage(volts)` sets every detector of kind `cdd` (Config when there is none); `set_param(name)` takes a registry name (`trap_current`, `custom:<name>`) or the source's vendor name. `AtHv` is set as a native HV value on its detector (`position_hv` with `NativeUnits`), not through the HV table. `plan::HopTarget` is gone: the engine hands `At` to the port; the loader's gate still refuses `at = { dac }` and `at = { hv }` until Task 8. Tests: `SpectrometerPortSim.{DacAndHvPositioning,DeflectionReachesTheBeam,SettingsReachTheFacade,TheTableScopeEndsWithThePort}`; the engine fake journals `move_native <v>`, `position_hv <v>@<det>`, `deflect <det>=<v>`, `table`, `gain`, `hv`, `cdd_voltage`, `param`, `blank`, `optics`, `source_output`.

### Task 8: Position execution (§5.2)

**Files:**
- Modify: `libs/experiment/src/measurement/engine.cpp`, `libs/experiment/src/plan/plan_parse.cpp` (gate entries for deflection, protect/active, `at` dac/hv, position integration and table leave `unbuilt_features`)
- Test: `tests/experiment/test_measurement_engine.cpp`

**Interfaces:**
- Consumes: Task 7 port calls, Task 6 `TraceEntry`.
- Produces: per position, in this order: `with_table` (position `table`, else segment `table`, else ""), `apply_optics`, deflections and protect for entries carrying them (and `ProtectPolicy::Auto` for the move as today), `position(at)`, settle, restart acquisition when integration differs, collect `counts` on active entries, then restore each deflection this position changed whose detector the next position (cyclically, within the segment) does not set. Table, optics, move and settle are skipped when position target, optics, table and deflections equal the previous position's.

- [x] **Step 1: Write failing tests** (plans inline as TOML, `ManualClock`, assertions on `result.trace`):
  - `PositionExec.WorkedPlan72DeflectsCddOnlyAtTheFirstPosition`: trace for cycle 1 is `deflect CDD=110`, `move Ar40@H1`, `settle 6`, `collect 15`, `deflect CDD=<previous value>` (restore before position 2, which does not set CDD), `move Ar39@CDD`, `settle 5`, `collect 15`, `move mass 35.0@CDD`, `settle 3`, `collect 8`.
  - `PositionExec.InactiveEntryIsProtectedNotCollected`: worked plan 7.4 position 2 → `protect H2(CDD) on`, `deflect H2(CDD)=3250`; no series for `Ar40` on `H2(CDD)`.
  - `PositionExec.IntegrationChangeRestartsAcquisition`: worked plan 7.5 without optics → `integration 1` before position 1 and `integration 2` before position 2, every cycle.
  - `PositionExec.UnchangedPositionSkipsMoveAndSettle`: one position, 3 cycles → one `move`, one `settle` in the trace (keeps `EngineTest.OneHopTimesNCyclesEqualsOneHopWithNTimesCounts`).
  - `PositionExec.DacAndHvTargets`: `at={dac=1.234}` → fake journal `move_native 1.234`; `at={hv=4500, detector="H1"}` → `position_hv`.
- [x] **Step 2: Run** `--gtest_filter='PositionExec.*'` → FAIL.
- [x] **Step 3: Implement** in `run_position`; drop the corresponding gate entries.
- [x] **Step 4: Run** the binary → PASS.
- [x] **Step 5: Commit** `feat(experiment): per-position deflection, protect, integration, table and positioning`.

Done notes: Step 2 was not observed failing: the tests were written in the same pass as `run_position`, and against the old gate the inline plans would not have resolved. Trace text is the engine's `describe`: `move mass 35@CDD`, not `35.0`. `PositionExec.WorkedPlan72...` names `[detectors] reference = "H1"` and gives position 2 an explicit `at`: worked plan 7.2 as written names no reference, and `position_target` gives positions 1 and 2 no target then (Task 10, which runs 7.2 through the simulator, has to settle what positions them: spec 3.2 says "the spectrometer's reference"). `IntegrationChangeRestartsAcquisition` expects no `integration` entry before the first collection (nothing to differ from), then one at every change. Decided here (spec 5.2 updated): every deflection still held is restored at the segment's end and on cleanup; a restore makes the next position settle; a new table or new optics forces the move; an empty `optics` leaves the preset in effect; protect stays around the move, as v1. Added beyond the task list: `PositionExec.TheTableIsSelectedPerPosition`. Gate: deflection, inactive entries, `at = { dac }` / `{ hv }`, position `integration_s`, position and segment `table` leave it, except on sniff and whiff segments (read in place, so none of these is applied there); `optics` stays until phase 8; `write_table` stays until Task 16, so a position table is one of the spectrometer's named tables. Worked plans 7.2 and 7.4 now pass the gate.

### Task 9: Large-beam settle and mode detectors in the spectrometer config

**Files:**
- Modify: `libs/systems/include/pychron/systems/spectrometer/config.hpp`, `libs/systems/src/spectrometer/config_loader.cpp`, `libs/experiment/include/pychron/experiment/measurement/engine.hpp`, `libs/experiment/src/measurement/engine.cpp`, `libs/experiment/include/pychron/experiment/measurement/adapters.hpp`, `libs/experiment/src/measurement/adapters.cpp`, `libs/experiment/src/run/run.cpp`
- Test: `tests/systems/test_spectrometer_config.cpp` (or the file that tests `parse_config`; find it with `grep -l parse_config tests/systems`), `tests/experiment/test_measurement_engine.cpp`

**Interfaces:**
- Produces:
  - `DetectorConfig` gains `std::optional<double> large_beam_threshold; double settle_after_large_beam_s = 0; std::string collector; std::string mode; double mode_switch_s = 0;` (TOML keys of the same names under `[[detectors]]`; `collector` empty means the detector is its own collector; two detectors sharing a `collector` are the §3.6 logical pair `H2` / `H2(CDD)`).
  - `struct DetectorTiming { std::optional<double> large_beam_threshold; double settle_after_large_beam_s = 0; std::string collector; double mode_switch_s = 0; };` in `engine.hpp`; `EngineContext` gains `std::map<std::string, DetectorTiming> timing;`; `std::map<std::string, DetectorTiming> detector_timing(const spectrometer::cfg::SpectrometerConfig&)` in `adapters.hpp`; `Run::measure` fills it.
  - Engine rule: settle = `max(settle_s, settle_after_large_beam_s of every detector in the new position whose last signal reading exceeded its large_beam_threshold, mode_switch_s when a collector changes mode)`. A baseline reading does not clear large-beam state (owner, 2026-10-15). State lives on the engine for the whole run, across segments and gases.

- [x] **Step 1: Write failing tests:**
  - `SpectrometerConfig.LargeBeamAndModeKeys`: a config with `large_beam_threshold = 5.0`, `settle_after_large_beam_s = 20`, and `H2(CDD)` with `collector = "H2"`, `mode = "CDD"`, `mode_switch_s = 2` parses to those values; a negative `settle_after_large_beam_s` is a config error naming the detector.
  - `LargeBeamSettle.WaitsTheConfiguredSettle`: Ar40 on H2 reads 10 (threshold 5, settle 20); next position with `settle_s = 3` → trace `settle 20`.
  - `LargeBeamSettle.BaselineDoesNotClearIt`: signal saturates H2, then a baseline position on H2, then a signal position → both moves settle 20.
  - `LargeBeamSettle.StateCarriesAcrossSegments` (Review Focus 4): two signal segments, first saturates H2, second's first position `settle_s=3` → `settle 20`.
  - `LargeBeamSettle.ModeSwitchWaits`: position on `H2` then on `H2(CDD)` → settle `max(settle_s, 2)`.
- [x] **Step 2: Run** `pychron_systems_tests --gtest_filter='SpectrometerConfig.LargeBeamAndModeKeys'` and `pychron_experiment_tests --gtest_filter='LargeBeamSettle.*'` → FAIL.
- [x] **Step 3: Implement**; add the new keys, commented out with defaults, to the spectrometer example config under `configs/examples` that documents `[[detectors]]` (read `libs/setup/AGENTS.md` first).
- [x] **Step 4: Run** both → PASS; `ctest --preset dev-quick`.
- [x] **Step 5: Commit** `feat(systems): per-detector large-beam settle and logical detector modes`.

### Task 10: Worked plans 7.2 and 7.4 through the simulator

**Files:**
- Test: `tests/integration/test_measurement_sim.cpp`; fixture spectrometer config for a Helix with `H2(CDD)`-style logical detectors at `tests/integration/fixtures/helix_v2/` (new, small)

- [x] **Step 1: Write the tests:** `MeasurementSimV2.WorkedPlan72` (ARGUS example configs, real facades, `VirtualTimeTest`: record keys after `fit_results` contain `Ar36`, `Ar39` on CDD and `mass35` on CDD; trace equals the `PositionExec.WorkedPlan72...` sequence) and `MeasurementSimV2.WorkedPlan74` (Helix fixture, at `tests/integration/fixtures/helix_v2/`, not written yet: `Ar40H`, `HCl` series exist; `H2(CDD)` has no series; the simulated deflection on H1(CDD) is 200 while position 2 collects).
- [x] **Step 2: Run** `pychron_integration_tests --gtest_filter='MeasurementSimV2.*'` → `WorkedPlan74` FAILS: its spectrometer config does not exist yet (the config loader's file-not-found error). `WorkedPlan72` runs on configs that exist and on Tasks 6-9's code, so it may pass here; if it fails, the failure names a gap in Tasks 6-9, fixed at its cause before going on.
- [x] **Step 3: Write the Helix fixture:** `spectrometer.toml` with `H1`, `H2` and the logical `H1(CDD)`, `H2(CDD)` (`mode`, `mode_switch_s`), `deflection` control on `H1(CDD)`; `molecular_weights.toml` with `Ar40H` and `PHHCl = 36.46`. Run again → `WorkedPlan74` either PASSES or fails on a real gap (the simulator not applying deflection, a catalog mass); fix that at its cause → PASS. A test that passes before the fixture exists is wrong: check that `WorkedPlan74` really loads `helix_v2/`.
- [x] **Step 4: Commit** `test(experiment): worked plans 7.2 and 7.4 in the simulator`.

Done notes: Step 2 as planned: `WorkedPlan74` failed on `helix_v2/spectrometer.toml: cannot read config file`. `WorkedPlan72` named the gap Task 8 left: the plan names no reference, so position 2 (`Ar39` on CDD, no `at`) had no target and was read wherever position 1 left the magnet. Settled from spec 3.7 ("spectrometer's reference positions it") and the legacy hop rule (spec 3.7 updated): `plan::position_target` takes the spectrometer's reference when the plan names none; a position not collecting on it is positioned by its first active entry. The engine gets it as `EngineContext::reference`, filled by `Run::measure` from `RunServices::reference_detector` (`LabSession` reads `[system] reference_detector`). Unit test `PlanSchema.SpectrometerReferencePositionsAPlanThatNamesNone`. The Helix fixture has `H2`, `H1` and five CDD modes (`H2(CDD)`, `H1(CDD)` sharing their Faradays' collectors, `mode_switch_s = 2`; `AX(CDD)`, `L1(CDD)`, `L2(CDD)` CDD only): 7.4 deflects H2(CDD) and L1(CDD) too, and validation refuses a deflection on a detector without control, so all three have it (`max = 4000` for 3250); the field table `helix` has a column for every detector (the config check wants one per active detector); the hydrides `Ar36H`..`Ar40H` and `HCl` join `Ar40H` and `PHHCl` in `molecular_weights.toml`, since every label is checked. "`H2(CDD)` has no series" is read as no `Ar40` series on it: position 3 collects `Ar40H` there. The deflection is read from the `BeamModel` at each Ar39 reading on H1(CDD). Real-time cost: a line scan wakes the whole simulated lab each simulated second, so `MeasurementSimV2` sets the line's scan to 60 s (`line_scan_interval_ms()`, no gauge is read) and has a 90 s dead-man (7.2 is some 1200 simulated seconds); measured 3.4 s idle and 24 to 27 s with the machine at load 46.

---

## Phase 3: Record v3 and collector keys (§6.1, §6.2 baseline rule, §6.3; spec item 3)

### Task 11: Series keys

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/collect/collector.hpp`, `libs/experiment/src/collect/collector.cpp`, `libs/experiment/src/measurement/engine.cpp`, `libs/experiment/src/measurement/results.cpp`
- Test: `tests/experiment/test_collector.cpp`

**Interfaces:**
- Produces:
  - `struct SeriesKey { std::string label; std::string detector; std::string kind; std::string gas; int segment = 0; int position = 0; friend auto operator<=>(const SeriesKey&, const SeriesKey&) = default; };` — `kind` an open string (`"signal"`, `"baseline"`, `"sniff"`, `"whiff"`, `"peak_center"`); `inline constexpr std::string_view kSignal = "signal", kBaseline = "baseline", kSniff = "sniff", kWhiff = "whiff";` replace `SeriesKind`.
  - `to_string(SeriesKey)` → `"Ar40@H1:signal:<gas>:<segment>.<position>"`.
  - `Channel` field `isotope` is renamed `label`; `CollectionSpec` gains `std::string gas; int segment = 0; int position = 0;` and its `kind` becomes `std::string`.
  - `std::optional<reduction::Intercept> Collector::intercept(const std::string& name) const;` — `"Ar40"` → most recently measured detector's series (today's rule); `"Ar40@H1"` → that detector. Several series of one label on one detector (several positions) are fitted as one series in time order.

- [x] **Step 1: Write failing tests:** `Collector.KeysCarryGasSegmentPosition`; `Collector.InterceptByLabelPicksMostRecentDetector`; `Collector.InterceptByQualifiedName` (`Ar40@H2` vs `Ar40@H1` from worked plan 7.3 shape); `Collector.OneLabelOneDetectorTwoPositionsFitsAsOneSeries`.
- [x] **Step 2: Run** `--gtest_filter='Collector.*'` → FAIL.
- [x] **Step 3: Implement**; every existing `SeriesKind::X` use becomes the string constant.
- [x] **Step 4: Run** the whole binary → PASS.
- [x] **Step 5: Commit** `refactor(experiment): series keyed by label, detector, kind, gas, segment and position`.

Built as well (2026-10-18): `merge_series(parts)` (points of several series as one, in time order), `merged_series(RunData, label, detector, kind)` (every series of a label on a detector wherever in the plan it was read; the tests' lookups and the peak-center `min_signal` check use it) and `Collector::series_of(name, kind)`. Until Task 13, `to_record_data` and `fit_results` merge the series of one label, detector and kind, so a record keeps its v2 shape and order; the evolutions view draws them as one line. The tests are in the `CollectorTest` fixture (`--gtest_filter='Collector*'`).

### Task 12: `AnalysisRecord` v3 types and serialization

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/record/types.hpp`, `libs/experiment/src/record/serialize.cpp`
- Create: `tests/data/records/v3.toml`, `tests/data/records/v3.json`
- Test: `tests/experiment/test_analysis_record.cpp`

**Interfaces:**
- Produces:
  - `kRecordSchemaVersion = 3`; `inline constexpr int kRecordSchemaV2 = 2;`
  - `struct SpectrometerState { std::map<std::string, double> deflections; double integration_time = 0; std::string optics, table; friend bool operator==(const SpectrometerState&, const SpectrometerState&) = default; };`
  - `struct PrepareRec { std::string script; std::map<std::string, std::string> variables; std::string started, finished; };` `struct PumpOutRec { std::string valve; double time_s = 0; };` (filled in Phase 7; serialized only when present)
  - `struct GasRec { std::string name; double time_zero = 0; std::vector<SpectrometerState> states; std::optional<PrepareRec> prepare; std::optional<PumpOutRec> pump_out; };`
  - `DataSeries` gains `std::string gas; int segment = 0, position = 0, state = 0;` (`state` indexes its gas's `states`); `Data` gains `std::vector<GasRec> gases;`
  - `struct BaselineRec { std::string detector, gas; int segment = 0, position = 0; std::string when; std::optional<double> magnet; std::optional<double> deflection; BaselineResult result; };`
  - `struct BaselineChoice { std::string rule; std::vector<std::size_t> baselines; double value = 0, error = 0; };` (`rule` `at`, `nearest`, `bracketing_average`; indexes into `Results::baseline_positions`)
  - `struct BaselineModifierRec { std::string variable, model, error; double value = 0, error_value = 0; };`
  - `Results` gains `std::vector<BaselineRec> baseline_positions; std::map<std::string, BaselineChoice> baseline_choices; std::map<std::string, BaselineModifierRec> baseline_modifiers;` — `intercepts` keyed `label@detector`; `baselines` (by detector) kept, filled with the chosen baseline of the detector's first signal series (the store's value, see "Decided here").
  - `from_toml`/`from_json` read v2 and v3; a v2 record loads with one implicit gas (name `""`), `segment = position = 0` on every series, `intercepts` re-keyed `label@detector` from the series, `baseline_positions` one per detector, `provenance.schema_version == 2`. `to_toml`/`to_json`/`compute_sha` write the v2 layout for a record whose `schema_version == 2`, the v3 layout otherwise.

- [x] **Step 1: Write failing tests:** `AnalysisRecord.V2FixturesKeepTheirSha` (load `v2.toml` and `v2.json`, `verify_sha` true, write back byte-identical); `AnalysisRecord.V2LoadsAsOneImplicitGas`; `AnalysisRecord.V3RoundTrips` (build a v3 record with two gases, three states, two baseline positions on H1 and one choice, round-trip TOML and JSON, equal and `verify_sha`); `AnalysisRecord.V3FixtureSha` (`v3.toml` and `v3.json` verify; these fixtures are what Windows CI checks).
- [x] **Step 2: Run** `--gtest_filter='AnalysisRecord.*'` → FAIL.
- [x] **Step 3: Implement**; write `v3.*` with the new writer on this Mac and commit them.
- [x] **Step 4: Run** → PASS.
- [x] **Step 5: Commit** `feat(experiment): AnalysisRecord v3 with gases, states and baseline positions`.

Done notes: Step 2 was observed failing only as a build failure (no v3 types). Departure (spec 6.1 updated): `RecordBuilder::finalize` writes v2 for a record v2 can say all of (`record::as_v2`: no gas but the implicit one, every series at segment, position and state 0, no baseline position but one a detector, no choice or modifier, intercept keys that v2 writes apart), as `instrument.simulated` and `provenance.status` were added; otherwise `RecordIngest.AnOrdinaryRunConvertsAsItAlwaysDid`, which pins `schema=2` and the record sha as the item payload, could not pass unedited. In memory a v2 record, read or built, is in v3 terms (`as_v3`): one gas `""` with the record's time zero and one state from `SpectrometerRec`, intercepts `label@detector`, one baseline position a detector. `record/names.hpp` gained `qualified_key`, `v3_intercept_key`, `v2_intercept_key`, and `intercept_name` takes a `label@detector` key; `RecordMetrics::intercept` finds one; the builder's completeness check accepts either key; `DataSeries` gained a constructor (gas, segment, position, state defaulted) so its four-field brace initializations stand. `RecordBuilder` gained `add_gas`, `add_baseline_position`, `set_baseline_choice`, `set_baseline_modifier`. v3 TOML: `data.series[].{gas,segment,position,state}`, `data.gases[]` (`name`, `time_zero`, `states[]`, optional `prepare`, `pump_out`), `results.baseline_positions[]` (`magnet`, `deflection` only when known), `results.baseline_choices`, `results.baseline_modifiers`. Tests reading a record's intercepts by v2 key (`RunTest.FullRunSucceedsAndSaves`, `MeasurementSim.ExecutorRunsAQueueOnTheSimLab`, `InstallE2E.EveryShippedPlanIsStoredAndReadsAsItsRecord`) now read `Ar40@H1`. `v3.*` rewritten with `PYCHRON_UPDATE_GOLDENS=1`.

### Task 13: Results from v3 data; names, ingest and the records path

**Files:**
- Modify: `libs/experiment/src/measurement/results.cpp`, `libs/experiment/include/pychron/experiment/measurement/results.hpp`, `libs/experiment/src/record/builder.cpp`, `libs/experiment/src/record/names.cpp`, `libs/experiment/include/pychron/experiment/record/names.hpp`, `libs/experiment/adapters/store/src/record_ingest.cpp`, `libs/processing/adapters/records/src/record_source.cpp`
- Test: `tests/experiment/test_record_names.cpp`, `tests/experiment/test_record_ingest.cpp`, `tests/processing/` records tests (`pychron_processing_records_tests`)

**Interfaces:**
- Produces:
  - `FitOutput fit_results(const collect::RunData& data, const plan::MeasurementPlan& plan);` (takes the plan for per-gas/segment fits and baseline `at`); intercepts keyed `label@detector`; one `BaselineRec` per baseline position per detector; per signal series a `BaselineChoice` by the §6.2 rule: a baseline position whose `at` matches the signal position's target, else the nearest in time (by mean t), else when two baselines bracket the signal (one ends before its first point, one starts after its last) and neither matches, their average with error `sqrt(e1² + e2²) / 2`.
  - `names.hpp`: `std::string analysis_name(const Data&, std::string_view result_key)` maps `Ar40@H1` → `Ar40` (one detector) or `H1:Ar40` (several); `intercept_name` and `named_series` accept v3 keys.
  - `to_ingest`: per-detector `baseline_value` from `Results::baselines`, `extra` = `{"positions": [...], "choices": {...}}` only when the detector has more than one baseline position (an ordinary run's items unchanged).
- [x] **Step 1: Write failing tests:** `RecordNames.QualifiedKeysMapToTodaysNames`; `FitResults.BaselineRuleAt`, `FitResults.BaselineRuleNearest`, `FitResults.BaselineRuleBracketingAverage` (values and the recorded rule); `RecordIngest.SeveralBaselinePositionsGoToExtra`; `RecordSource.V3RecordReadsWithPerSeriesBaselines` (records path uses the per-series choice). `RecordIngest.AnOrdinaryRunConvertsAsItAlwaysDid` must pass unedited.
- [x] **Step 2: Run** `pychron_experiment_tests --gtest_filter='RecordNames.*:FitResults.*'`, `pychron_experiment_store_tests`, `pychron_processing_records_tests` → FAIL on the new ones.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run** the three binaries → PASS; `pychron_install_e2e_tests --gtest_filter='InstallE2E.EveryShippedPlanIsStoredAndReadsAsItsRecord'` → PASS.
- [x] **Step 5: Commit** `feat(experiment): v3 results, the baseline rule, and names for qualified keys`.

Done notes: Step 2 was observed failing as a build failure (`analysis_name`, the new `fit_results` signature). `to_record_data(data, plan)` gives one record series per collector series (sorted by label, detector, kind, gas, segment, position) and one `GasRec` per gas measured, each with the run's time zero and one state holding the main segment's integration (Task 22 gives each gas its own time zero and states; every series' `state` is 0 until then). `fit_results(data, plan, spectrometer_reference)`: the rule's candidates are restricted to the signal's gas, an `at` match narrows them, and two of them that bracket the signal average before the nearest is taken (spec 6.2 updated: the plan's order "at, else nearest, else bracketing" would never reach the last). A real run's record is v3 now (series carry segment indexes); the in-memory record of an ordinary sample is still v2 (Task 12). `named_series` joins one isotope's series of one kind on one detector (several positions) into one, signal kinds in time order, baselines in record order as before; `to_ingest` refuses two series at one place in the plan instead (same message start, `RecordIngest.TwoSeriesOfOneKindIsotopeAndDetectorAreRefused` unedited). Store `extra` JSON: `{"positions": [{gas, segment, position, when, value, error, magnet?, deflection?}], "choices": {"<label>@<det>": {rule, baselines, value, error}}}`. Records path: `analysis_from_record` takes each isotope's baseline from its choice, else the detector's. The e2e spec's not-stored list names the v3 fields the store does not keep. Tests ported to `label@detector` keys: `EngineTest.ResultsFitInterceptsAndBaselines`, `EngineTest.IsotopeOnTwoDetectorsGetsDetectorQualifiedIntercepts`, the `MeasurementSim*` fits (helper `intercept_of`); `EngineTest.RecordMergesThePositionsOfOneLabelOnOneDetector` became `RecordKeepsPositionsAndFitsOneLabelOnOneDetectorAsOne`. Added: `RecordNames.PositionsOfOneLabelOnOneDetectorAreOneSeries`.

### Task 14: Fits by `label@detector`, per gas and per segment

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/plan/plan.hpp`, `libs/experiment/src/plan/plan.cpp`, `libs/experiment/src/plan/plan_parse.cpp` (segment `fits` leaves the gate), `libs/experiment/src/collect/collector.cpp`
- Test: `tests/experiment/test_plan_v2.cpp`

**Interfaces:**
- Produces: `reduction::FitSpec fit_for(const MeasurementPlan&, const Gas&, const Segment*, std::string_view kind, std::string_view label, std::string_view detector);` — lookup order: segment `fits` then gas `fits` then plan `fits`; within each, `label@detector`, `label` (signal) or `detector` (baseline), `default`. Replaces `signal_fit`/`baseline_fit`; the collector's live fits and `fit_results` both call it.
- [x] **Step 1: Write failing tests:** `PlanFits.QualifiedBeatsBareBeatsDefault`; `PlanFits.SegmentBeatsGasBeatsPlan`; `PlanFits.V1KeysKeepTheirMeaning` (`[fits] signal.Ar40` → every detector of Ar40; `baseline.H1` → every baseline on H1).
- [x] **Step 2: Run** `--gtest_filter='PlanFits.*'` → FAIL.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run** the binary → PASS.
- [x] **Step 5: Commit** `feat(experiment): fits keyed label@detector, per gas and segment`.

Done notes: Step 2 was observed failing as a build failure (no `fit_for`). `kind` "baseline" reads the baseline table, every other kind (signal, sniff, whiff) the signal table; a baseline with no key there is averaged (as `baseline_fit`). A segment override is looked up `label@detector`, `label`, `detector`, `default`, and only the members it sets replace the gas's. A series read in several segments (one intercept) takes the fit of its first part's segment, in `fit_results` and in the collector's live fits. The collector takes the whole plan (`Collector::set_plan`, the engine's call); `set_fits` stays for tests (a plan of those fits alone). `signal_fit` and `baseline_fit` stay as plan-level lookups (`PlanSchema` reads them); nothing in the engine, collector or results calls them now. Segment `fits` left the gate (`PlanV2Gate` lost its case).

### Task 15: Conditional metric qualifier, live `cycles`, path aliases

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/conditionals/metrics.hpp`, `libs/experiment/src/conditionals/metrics.cpp`, `libs/experiment/src/conditionals/expr.cpp`, `libs/experiment/src/collect/collector.cpp`, `libs/experiment/include/pychron/experiment/measurement/engine.hpp`, `libs/experiment/src/measurement/engine.cpp`, `libs/experiment/include/pychron/experiment/run/run.hpp`, `libs/experiment/src/run/run.cpp`, `libs/experiment/src/plan/plan_parameters.cpp`
- Test: `tests/experiment/test_conditional_metrics.cpp`, `tests/experiment/test_measurement_engine.cpp`, `tests/experiment/test_plan_parameters.cpp`

**Interfaces:**
- Produces:
  - `MetricRef` gains `std::string detector;` (parsed from `Ar40@H1.cur`, `Ar40@H1/Ar39`); bare names unchanged.
  - `void MeasurementEngine::set_cycles(int cycles);` (running segment only, ≥ the cycle in progress); `RunControl::set_cycles(int)` beside its counts control (grep `set_target(` in `run/` and `lab/session.cpp` for the path counts take, and give cycles the same one).
  - `std::optional<std::string> resolved_path(const MeasurementPlan&, std::string_view v1_path);` — `main.hops[i].counts` → `segment[k].positions[i].counts` (k: the desugared signal segment), `main.cycles` → `segment[k].cycles`; used where a live edit names a v1 path.
- [x] **Step 1: Write failing tests:** `ConditionalMetrics.DetectorQualifier` (`Ar40@H1.cur` reads H1's series when Ar40 is on H1 and H2); `LiveEdit.CyclesChangeOnlyTheRunningSegment` (Review Focus 5: two signal segments of 5 cycles, `set_cycles(2)` during segment 1 cycle 1 → segment 1 runs 2, segment 2 runs 5); `LiveEdit.CountsChangeOnlyTheRunningPosition`; `PlanParameters.V1PathResolvesToSegmentPath`.
- [x] **Step 2: Run** those filters → FAIL.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run** the binary → PASS.
- [x] **Step 5: Commit** `feat(experiment): detector-qualified metrics and live cycles`.

Done notes: Step 2 was observed failing as build failures (no `MetricRef::detector`, `set_cycles`, `resolved_path`). `MetricRef` gained `b_detector` too (`Ar40/Ar39@CDD`); the tokenizer takes `@<detector>` after a label (with a mode, `Ar40@H2(CDD)`); a detector field (`.deflection`) refuses a qualifier. `qualified_a`/`qualified_b` give the collector's and the record's lookup name; `RecordMetrics` reads `Ar40@H1` as that detector's series and intercept. Live cycles: `MeasurementEngine::set_cycles` sets the running segment's cycles (at least the cycle in progress; nothing between segments; cleared at each segment's start), and main's counts check after a gap counts the cycles main ran to. `RunControl::set_cycles` beside `set_counts`; no session or UI path takes counts today (grep found none), so none takes cycles. `resolved_path` lives in `parameters.hpp` (`main.hops[i].counts|settle_s`, `main.cycles`, `main.integration_s`); nothing calls it yet: no live edit names a path today. Tests are `EngineTest.LiveEdit{CyclesChangeOnlyTheRunningSegment,CyclesNeverCutTheCycleInProgress,CountsChangeOnlyTheRunningPosition}` (the engine fixture), `CollectorTest.DetectorQualifiedMetrics` added.

---

## Phase 4: Steps and hooks (§3.3 steps, §5.5; spec item 4)

### Task 16: `peak_center` step with `detectors[]`, `extra_detectors`, `write_table`

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/measurement/ports.hpp`, `libs/experiment/src/measurement/adapters.cpp`, `libs/experiment/src/measurement/engine.cpp`, `libs/experiment/src/plan/plan_parse.cpp` (gate)
- Test: `tests/experiment/test_measurement_engine.cpp`, `tests/integration/test_measurement_sim.cpp`

**Interfaces:**
- Produces: `PeakCenterRequest` gains `std::vector<std::string> extra_detectors; std::string write_table;`; `PeakCenterReport` unchanged. `SpectrometerPeakCenter` with `write_table` set: after an `ok` center, `update_table` on a per-run copy of the current table registered under that name, so a later `with_table(name)` uses it (per-run tables live in the adapter for the run's life, never written to disk). With `detectors[]` the engine calls the port once per detector, in order.
- [x] **Step 1: Write failing tests:** `PeakCenterStep.OncePerDetector` (worked plan 7.3: three port calls H1, AX, L2, each `write_table == "ic_run"`); `PeakCenterStep.ExtraDetectorsPassed` (worked plan 7.1); `PeakCenterStep.MinSignalSkipsAfterRun` (v1 rule kept: mean of last 10 readings below → `skipped`); `PeakCenterStep.FailedWriteTableFallsBackToCurrentTable` (Review Focus 1: port returns `ok=false`; later positions trace `table <current>`, `result.notes` holds `table ic_run not written: <message>`, outcome Completed); `SpectrometerPeakCenterSim.WriteTableIsUsedByLaterPositions` (sim: the per-run table's value for Ar40 on AX equals the center found).
- [x] **Step 2: Run** those filters → FAIL.
- [x] **Step 3: Implement**; drop the step gate entries for `peak_center`.
- [x] **Step 4: Run** both binaries' filters → PASS.
- [x] **Step 5: Commit** `feat(experiment): peak center step per detector with a per-run field table`.

Done notes: Step 2 was observed failing as build failures (no `extra_detectors`, `write_table` on the request). The facade gained run tables (`Spectrometer::add_run_table`, `drop_run_table`, `is_run_table`): an in-memory table selected with `with_table`, refused under a name loaded or in the store, and refused by `save_table`; `jobs::PeakCenterConfig` gained `save_table` (not a file key) so a write_table job updates its detector's column only (`propagate = false`) and saves nothing. `SpectrometerPeakCenter` copies the current table into the run table at its first write_table job and selects it for the job's length; `IPeakCenterPort::drop_run_tables` (default no-op) forgets them, called by the engine's cleanup after it lets go of its own table selection. The engine runs a step's detectors in order, each with the min_signal check; a failed write_table job notes `table <name> not written: <message>` once per table and every later position naming that table runs on the current one: no `table <name>` call is made (the trace then has no `table` entry, the current table being selected already, rather than a `table <current>` entry). The `peak_center` gate entries are gone; a peak center outside before_inlet/after_run is still the `when` gate's (Task 19). Tests: `EngineTest.PeakCenterStep{OncePerDetector,ExtraDetectorsPassed,MinSignalSkipsAfterRun,FailedWriteTableFallsBackToCurrentTable}` (inline plans: 7.3 has a `set` step, Task 17), `MeasurementSim.WriteTableIsUsedByLaterPositions`, `Spectrometer.ARunTableIsSelectableAndNeverSaved`.

### Task 17: `set`, `blank_beam`, `park`, `delay`, `time_zero` steps

**Files:**
- Modify: `libs/experiment/src/measurement/engine.cpp`, `libs/experiment/src/plan/plan_parse.cpp` (gate)
- Test: `tests/experiment/test_measurement_engine.cpp`

**Interfaces:**
- Consumes: Task 7 port calls. Each `set` sub-key maps to one port call: `deflection` → `set_deflection` per detector, `gain` → `set_gain`, `hv` → `set_hv`, `cdd_voltage` → `set_cdd_voltage`, `source` → `set_param` per entry, `integration_s` → the engine's integration for later collections in the gas. `park` → `position(AtLabel|AtMass)` with no collection. `delay` → `wait(seconds)`. `time_zero` `now` → `collector.set_time_zero(now)`; `{offset_s}` → now + offset.
- [x] **Step 1: Write failing tests:** `Steps.SetMapsEachSubKeyToOneCall` (fake journal order = TOML order); `Steps.BlankBeam`; `Steps.ParkMovesWithoutCollecting`; `Steps.DelayWaitsOnTheClock` (ManualClock advances exactly the seconds); `Steps.TimeZeroStepAfterIcBlock` (worked plan 7.3: time zero equals the clock at the step, not inlet close).
- [x] **Step 2: Run** `--gtest_filter='Steps.*'` → FAIL.
- [x] **Step 3: Implement**; drop the gate entries.
- [x] **Step 4: Run** the binary → PASS.
- [x] **Step 5: Commit** `feat(experiment): set, blank_beam, park, delay and time_zero steps`.

Done notes: Step 2 was not observed failing apart from the gate (the plans did not resolve). `SetStep` holds maps, not the file's order, so a `set` calls in a fixed order: deflection (by detector), gain, source, hv, cdd_voltage, integration_s (spec 5.2 updated). A `set` deflection stands until the run ends (`standing_`, restored in cleanup); a position's own deflection of that detector goes back to the standing value. A blanked beam is unblanked in cleanup. `set.integration_s` holds for later collections whose position names none (a segment's `integration_s` always has a value, so it does not win over the step). `hv` forgets the magnet position. A `time_zero` step sets the collector's time zero and the gas's policy does not set it again. Gate: these steps leave the kind gate and are held to before_inlet, after_main or after_run (where the engine runs every segment of a `when`) until Task 19 runs the other phases; `PlanV2Gate.EachUnbuiltFeatureIsNamed` names the delay case by its `when`. `Steps.TimeZeroStepAfterIcBlock` puts the step at after_main for that reason; worked plan 7.3 as written (step at after_inlet) runs with Task 19. Tests are `EngineTest.Steps*`.

### Task 18: Hooks as capable as the plan (§5.5)

**Files:**
- Modify: `libs/scripting/include/pychron/scripting/services.hpp`, `libs/scripting/src/python/sources.cpp` (measurement hook bindings; find the module that binds `IMeasurementApi` with `grep -rn "add_conditional" libs/scripting/src`), `libs/scripting/src/vocabulary.cpp`, `libs/experiment/src/measurement/engine.cpp` (`MeasurementEngine::Api`)
- Test: `tests/scripting/` hook-API test file (`grep -ln IMeasurementApi tests/scripting`), `tests/experiment/test_measurement_engine.cpp`

**Interfaces:**
- Produces on `scripting::IMeasurementApi`: `virtual Result<void> peak_center(std::string_view label, std::string_view detector, std::string_view write_table) = 0; virtual Result<void> set_deflection(std::string_view detector, double value) = 0; virtual Result<void> set_integration(double seconds) = 0; virtual Result<void> with_table(std::string_view name) = 0; virtual Result<void> apply_optics(std::string_view name) = 0;` Python names the same.
- [x] **Step 1: Write failing tests:** `EngineHookApi.NewCallsReachThePorts` (a `FakeHook` calling each → fake journal); a scripting test that a hook script calling `set_deflection("CDD", 110)` reaches a fake `IMeasurementApi`.
- [x] **Step 2: Run** → FAIL.
- [x] **Step 3: Implement**; each Python binding releases the interpreter lock before calling in (root `AGENTS.md`, Time: `host_state.cpp`).
- [x] **Step 4: Run** `pychron_scripting_tests` and the engine filter → PASS.
- [x] **Step 5: Commit** `feat(scripting): measurement hooks gain peak_center, deflection, integration, table and optics`.

Done notes: Step 2 was observed failing as build failures (no such calls). The hook's binding module is `libs/scripting/src/python/runtime.cpp` (`MeasurementAPI`); `peak_center(label, detector, write_table='')` in Python. Engine side: `peak_center` runs one detector as a step does (refused inside a collection, Config without a peak center service); `set_deflection` stands until the run ends as a `set` step's (shared `set_standing_deflection`); `set_integration` is the `set` step's integration (seconds > 0); `with_table` and `apply_optics` hold until a position selects its own and make the next position move again. Nothing in `vocabulary.cpp` names hook API calls (the static check does not look at `api.` attributes), so it is unchanged. Tests: `EngineTest.EngineHookApiNewCallsReachThePorts`, `ScriptHostTest.MeasurementHookReachesThePlansCalls`.

---

## Phase 5: `when` phases and `during_extraction` (§5.1, §5.4, §3.5; spec item 5)

### Task 19: Phases in the engine

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/plan/plan.hpp`, `libs/experiment/src/plan/plan.cpp`, `libs/experiment/include/pychron/experiment/measurement/engine.hpp`, `libs/experiment/src/measurement/engine.cpp`, `libs/experiment/src/plan/plan_parse.cpp` (gate for `when`)
- Test: `tests/experiment/test_measurement_engine.cpp`

**Interfaces:**
- Produces:
  - `std::vector<std::size_t> segments_at(const Gas& gas, When when);` — file order within a `when`; for `AfterInlet` whiff segments first.
  - `Result<void> MeasurementEngine::run_phase(plan::When when, scripting::CancelToken& token);` (runs that phase of gas 0; used by `Run` for `DuringExtraction` only) and `MeasurementResult run(token)` runs every remaining phase of every gas in §5.1 order: `before_inlet`, inlet open, `during_equilibration`, inlet close / time zero, `after_inlet`, `after_main`, then `after_run` once after the last gas. `bool has_phase(plan::When) const;`
- [x] **Step 1: Write failing tests:** `Phases.OrderAcrossWhens` (one segment per `when` except during_extraction, declared in reverse file order → trace in §5.1 order); `Phases.SameWhenRunsInFileOrder`; `Phases.AfterMainAfterLastSignalSegment` (two `after_inlet` signal segments, `after_main` baseline runs after both); `Phases.BaselineDuringEquilibration` (positions collected while the equilibration waits run, valve timing at reading granularity as sniff today).
- [x] **Step 2: Run** `--gtest_filter='Phases.*'` → FAIL.
- [x] **Step 3: Implement**; drop the `when` gate except `during_extraction` (Task 20).
- [x] **Step 4: Run** the binary → PASS.
- [x] **Step 5: Commit** `feat(experiment): plan segments run by phase`.

Done notes: Step 2 was not observed failing: the tests were written with the engine change (they would have failed at the gate before it). Built here: `plan::segments_at`, and the engine's phase walk (`run_gas`, `run_when`, `block`): before_inlet; the main segment's first position; the equilibration (the whiff as the inlet opens, then the gas's other during_equilibration segments in file order, then the sniff, which starts after them when there are any); the `before_main` hook; every after_inlet segment in file order but the whiff, the first signal one as `Main`; the `after_main` hook; after_main; after_run once after the gas. A segment run while the gas equilibrates closes the inlet on time, checked after each reading and before each position (`inlet_due`, `finish_inlet`: close, `OverlapReady`, time zero on close), and is read to its end. `run_phase` and `has_phase` are left to Task 20, their one user. Gate: `when` is refused only as `during_extraction` (Task 20), a sniff anywhere but during_equilibration and a whiff anywhere but after_inlet (read where the engine reads them); worked plan 7.3 now passes the gate. Tests are `EngineTest.Phases*`.

### Task 20: `during_extraction` in the run and executor

Adds a thread: run under `-DPYCHRON_SANITIZE=thread` before it lands.

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/run/run.hpp`, `libs/experiment/src/run/run.cpp`, `libs/experiment/src/plan/plan_parse.cpp` (gate)
- Test: `tests/experiment/test_run.cpp`, `tests/integration/test_lab_sim.cpp`

**Interfaces:**
- Produces: when `engine.has_phase(When::DuringExtraction)`, `Run::extract` builds the engine (moved out of `measure` into `Result<void> Run::build_engine()`), acquires the spectrometer (`hooks_.acquire_spectrometer`) before `acquire_extraction`, and runs `engine.run_phase(DuringExtraction, token)` on a second thread concurrently with the extraction script, under the run's one cancel token. Thread rules (root `AGENTS.md`, Time): starter makes a `Clock::Hold` before `std::thread`; the thread constructs a `Clock::Participant` first; as its last act it sets `baseline_done_` under the run's mutex and `clock.notify_all(cv)`; `Run::measure` waits for the flag with `clock.wait` and then `join()`s, before `before_inlet`. `Run::measure` does not acquire the spectrometer again. Baselines of that phase carry `when = "during_extraction"` in the record (Task 12 `BaselineRec::when`).
- [x] **Step 1: Write failing tests:**
  - `RunTest.DuringExtractionClaimsTheSpectrometerAtExtraction` (fake hooks journal: `acquire_spectrometer`, `acquire_extraction`, ..., `release_spectrometer` last).
  - `DuringExtraction.BaselineEndsBeforeBeforeInlet` (`VirtualTimeTest`, `test_lab_sim.cpp`, simulated laser and spectrometer, worked plan 7.1: last baseline point t < first `before_inlet` or inlet-open time; extraction shorter than the baseline → the run waits for the baseline).
  - `DuringExtraction.ExtractionFailureStopsTheBaselinePhase` (Review Focus 3: script fails after 10 s → token cancels the phase, thread joined, spectrometer released, run `Failed`, record saved as an extraction failure is today).
  - `DuringExtraction.PlansWithoutItStillOverlap` (existing `ExecutorTest.OverlapStartsTheNextRunBeforeThisOneFinishes` shape with a plain plan, plus a run with `during_extraction` after it: that run's extraction does not start until the previous run's measurement released the spectrometer).
- [x] **Step 2: Run** `pychron_experiment_tests --gtest_filter='RunTest.DuringExtraction*'`, `pychron_integration_tests --gtest_filter='DuringExtraction.*'` → FAIL.
- [x] **Step 3: Implement**; drop the last `when` gate entry.
- [x] **Step 4: Run** both → PASS. Then configure a TSan tree (`cmake --preset dev -B build/tsan -DPYCHRON_SANITIZE=thread`), build `pychron_experiment_tests` and `pychron_integration_tests` there, run the same filters plus `ExecutorTest.*` → no report.
- [x] **Step 5: Commit** `feat(experiment): baselines during extraction`.

Done notes: Step 2 was not observed failing apart from the gate (the plans did not resolve before the gate entry went). Engine: `begin()` (a measurement's state, its time 0) runs once for `run_phase` and `run`; `run_phase(when, token)` runs that phase of the first gas and leaves acquisition stopped and nothing protected; `has_phase(when)`. Run: `build_engine` (post-equilibration thread state became members); the phase thread follows the Time rules (Hold before the thread, Participant first, done flag set under the run's mutex and notified through the clock, `clock.wait` then `join`); the phase runs on its own `CancelToken`, which the run's token feeds (cancel or abort) and a failed extraction cancels, so the run then fails as an extraction failure rather than ending Cancelled. The extraction resource is released as today, after the script; the run waits for the phase before going on. `PlanResolvers::has_extraction` is set where the lab builds its plan library (`rules_for(type).extraction`; only `pause` has none, and it has no plan), and a run whose plan measures during the extraction but that has no extraction script fails at prepare. The `when` gate is gone; worked plan 7.1 passes the gate. Tests: `RunTest.DuringExtraction{ClaimsTheSpectrometerAtExtraction,AFailedExtractionReleasesTheSpectrometer,NeedsAnExtractionScript}` (ManualClock), and the three integration cases as `ExecutorVirtual.DuringExtraction{BaselineEndsBeforeBeforeInlet,ExtractionFailureStopsTheBaselinePhase,PlansWithoutItStillOverlap}` in `tests/experiment/test_executor.cpp`: the executor on a `VirtualClock` with the run fakes and a plan `syn` (`run_fakes.hpp`, a 60-reading baseline during the extraction), not the simulated laser lab with worked plan 7.1 (that is Task 33's `MeasurementSimV2.EveryWorkedPlanRunsWithItsTrace`, which runs 7.1 through the simulator). TSan (`-DPYCHRON_SANITIZE=thread`, `build/tsan`): the whole `pychron_experiment_tests` binary, no report. `Outage.*` passes (17).

### Task 21: Baseline modifiers (§3.5)

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/plan/plan.hpp`, `libs/experiment/src/plan/plan_parse.cpp`, `libs/experiment/src/measurement/results.cpp`, `libs/experiment/adapters/store/src/record_ingest.cpp`
- Test: `tests/experiment/test_plan_v2.cpp`, `tests/experiment/test_record_ingest.cpp`

**Interfaces:**
- Produces: `struct BaselineModifier { std::string variable, model, error; };` `MeasurementPlan` gains `std::map<std::string, BaselineModifier> baseline_modifiers;` (`[baseline.modifier.<detector>]`; `baseline` table allowed keys add `modifier`). `model` is a CSV path relative to the plan library or an expression in `x`; `variable` an expression over this run's intercepts (the conditionals expression parser, intercept names only). Applied in `fit_results`: `Results::baseline_modifiers[det]` with the evaluated variable, the model's value and error; the detector's baseline value is not changed in the record (applied at reduction, §3.5). `to_ingest` writes `baseline_modifiers_json` and the detector's `modifier_value`/`modifier_error`.
- [x] **Step 1: Write failing tests:** `PlanV2Parse.BaselineModifier` (worked plan 7.1 table read: put the worked plan's `[baseline.modifier.CDD]` back into `v2_71_argus_syn_baseline.toml`, left out by Task 2); invalid fixture `v2_modifier_unknown_detector.toml` (`# expect: unknown detector`); `FitResults.BaselineModifierEvaluated` (model expression `0.01*x`, variable `Ar40+Ar39` → value); `RecordIngest.BaselineModifiersHaveAProducer` (row in the field table of that test file).
- [x] **Step 2: Run** → FAIL.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run** `pychron_experiment_tests`, `pychron_experiment_store_tests` → PASS.
- [x] **Step 5: Commit** `feat(experiment): baseline modifiers in plan and record`.

Done notes: Step 2 was observed failing as build failures (no `baseline_modifiers`). Settled here (spec 3.5 updated): the variable reads only intercept names (`Ar40`, the label's first detector's intercept, or `Ar40@H1`) and the model only `x`, both checked at load; a CSV model is the weighted least-squares line through its `x, baseline[, error]` points (weights 1/error^2, or equal weights and the scatter's variance without errors; `#` comment lines, a header line skipped), its value and standard error at the run's variable; an expression model's `error_value` is 0. `error` is the name of an error model, kept in the record and the store for the reduction to apply, not evaluated here. The CSV is read by the run from `RunServices::plan_dir` (`<lab>/plans`, set by `LabSession`) and handed to `fit_results` as `ModelFiles`; one not read is a fit error and no modifier is recorded. `to_json` writes `baseline_modifiers` only when there are any, so only the 7.1 golden changed. The store: the detector's `modifier_value`/`modifier_error` and `analysis_meta.baseline_modifiers` JSON. Added: `PlanV2Parse.BaselineModifierExpressionsAreChecked`, `FitResults.BaselineModifierFromAModelFile`.

---

## Phase 6: Gases (§3.4; spec item 6)

### Task 22: Several gases in one run

**Files:**
- Modify: `libs/experiment/src/measurement/engine.cpp`, `libs/experiment/src/measurement/results.cpp`, `libs/experiment/src/record/builder.cpp`, `libs/experiment/src/plan/plan_parse.cpp` (gate; the §3.7 rule below)
- Test: `tests/experiment/test_measurement_engine.cpp`, `tests/experiment/test_record_ingest.cpp`, `tests/experiment/test_plan_schema.cpp` (invalid fixtures); new invalid fixture `tests/experiment/fixtures/plans/invalid/v2_two_gases_same_series.toml`

Rule (spec §3.7, last bullet but one): the store keeps one series of a kind per isotope per detector (`libs/experiment/AGENTS.md`; `to_ingest` refuses a second). Two gases that collect the same `label@detector` with the same `kind` would give a run that measures and then cannot be stored, against §11.6 ("a gas already measured is saved whatever happens"). So the plan is refused at load, before any run: `gas '<a>' and gas '<b>' both collect <kind> <label>@<detector>: the store keeps one`. Different kinds (a `signal` and a `baseline`), or the same label on different detectors, pass. A store mapping that keeps both (a gas-qualified name) is a change to the stored names, which `RecordIngest.AnOrdinaryRunConvertsAsItAlwaysDid` pins; not taken here.

**Interfaces:**
- Produces: per gas, in file order: `before_inlet` (after the previous gas's `after_main`), its own inlet/outlet/equilibration, its own time zero (`Collector` holds `std::map<std::string, double> time_zeros` by gas; `set_time_zero(gas, s)`; fits use the series' gas's time zero), `after_inlet`, `after_main`. `after_run` once, after the last gas. Each gas's distinct `SpectrometerState`s are recorded once in `GasRec::states`, each series pointing at its state. `OverlapReady` is published when the last gas's inlet closes.
- [x] **Step 1: Write failing tests:** `Gases.WorkedPlan76Order` (trace: He inlet open, He close, He signal, He baseline, Ne inlet, ..., Ar signal); `Gases.EachGasItsOwnTimeZero` (record `gases[i].time_zero` differs, fits per gas); `Gases.StatesRecordedOncePerGas` (Ne's five positions share one state: `states.size()==1`); `Gases.OverlapReadyAfterLastInlet`; `RecordIngest.MultiGasRecordStores` (the worked plan 7.6 record, three gases with distinct labels, converts: every series stored, none refused); invalid fixture `v2_two_gases_same_series.toml` (gases `A` and `B` each with a signal position collecting `Ar40` on `H1`) — `# expect: gas 'A' and gas 'B' both collect signal Ar40@H1`, picked up by `PlanSchema.EveryInvalidFixtureFailsWithItsRule`; `Gases.SameLabelOtherKindOrDetectorIsFine` (gas A signal `Ar40@H1`, gas B baseline `Ar40@H1` and signal `Ar40@AX` → resolves, through `resolve_plan_ungated`).
- [x] **Step 2: Run** `--gtest_filter='Gases.*:PlanSchema.EveryInvalidFixtureFailsWithItsRule'` and the ingest filter → FAIL.
- [x] **Step 3: Implement**; the rule in `check_v2` (over `plan.gases`, so a v1 file, one implicit gas, never trips it); drop the "more than one gas" gate entry.
- [x] **Step 4: Run** the binaries → PASS.
- [x] **Step 5: Commit** `feat(experiment): several gases in one run`.

Done notes: Step 2 was observed failing at the gate (more than one gas) and as build failures (`time_zeros`, `states`). The collector keeps `Timing::time_zeros` by gas (`set_time_zero(gas, s)`; `time_zero()` is the first gas's, set again with it) and fits every series, live and in `fit_results`, on its gas's time zero; a record series' `t` is relative to its gas's time zero, `GasRec::time_zero` each gas's, `Data::time_zero` the first's. The engine records each place's state (deflections in force, integration, optics, table) as `MeasurementResult::states` (`Place`, `PositionState` in `results.hpp`); `to_record_data(data, plan, states)` lists each gas's distinct states once, and a place without one (a sniff, a whiff) takes the gas's main segment's integration. Per gas: inlet, time zero, main counts, a `set` step's integration, the `time_zero` step; `OverlapReady` and `on_inlet_closed` (the post-equilibration script) only at the last gas's inlet close; `validate` covers every gas. A named gas's collections are counted under `<gas>.main`, `<gas>.segment[i]`, `<gas>.sniff`. The rule refuses signal, sniff and whiff only: a baseline is one stored series a detector whatever reads it (spec 3.7 updated). `PlanDuration.StepsAndGases` and `PlanFits.SegmentBeatsGasBeatsPlan` read Ar40 on H1 in two gases, which the rule now refuses; their second gas reads another label (or detector). Worked plan 7.6 passes the gate. Tests are `EngineTest.Gases*` (an inline three-gas plan shaped as 7.6: 7.6's valves are aliases the engine fixture does not resolve), `Gases.SameLabelOtherKindOrDetectorIsFine`, `RecordIngest.MultiGasRecordStores`, and the invalid fixture.

### Task 23: `source_dark_phase`

**Files:**
- Modify: `libs/experiment/src/measurement/engine.cpp`, `libs/experiment/src/plan/plan_parse.cpp` (gate; validation: `source_dark_phase` only when the catalog reports source output control, catalog `virtual bool has_source_output() const = 0;`)
- Test: `tests/experiment/test_measurement_engine.cpp`

- [x] **Step 1: Write failing tests:** `Gases.SourceDarkPhase` (journal: `set_source_output(false)` before inlet open, `set_source_output(true)` at time zero; on cancel during equilibration the engine's cleanup turns the source back on); invalid fixture `v2_dark_phase_unsupported.toml` (`# expect: source_dark_phase needs source output control`).
- [x] **Step 2: Run** → FAIL.
- [x] **Step 3: Implement**; safety rule in `cleanup`: a source the engine darkened is switched on again on every exit.
- [x] **Step 4: Run** → PASS.
- [x] **Step 5: Commit** `feat(experiment): source dark phase during inlet`.

Done notes: Step 2 was observed failing at the gate and as a build failure (`has_source_output`). The source goes off just before the gas's inlet opens and on when its equilibration ends (`finish_inlet`: the inlet's close, time zero by default; spec 3.4 updated), traced `source off`/`source on`; cleanup switches on a source the engine darkened. `SpectrometerCatalog::has_source_output` answers false until Task 29 gives a driver the control, so a lab plan with `source_dark_phase` is refused at load until then; the gate entry is gone. Test is `EngineTest.GasesSourceDarkPhase` (with a stop in the sniff).

---

## Phase 7: Staged gas release, row T027 (§11; spec item 6b)

### Task 24: `prepare` and `pump_out` on a gas

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/plan/plan.hpp`, `libs/experiment/src/plan/plan_parse.cpp`, `libs/experiment/include/pychron/experiment/plan/plan_loader.hpp`, `libs/experiment/src/plan/duration.cpp`
- Test: `tests/experiment/test_plan_v2.cpp`, `tests/experiment/test_plan_parameters.cpp`; fixtures `valid/v2_staged_cryo_release.toml` (two gases He and Ne, each `prepare = { script = "cryo:release", variables = { cryo_temp = 35|75, hold_s = 300 } }`, He `pump_out = { valve = "@valves.ion_pump", time_s = 120 }`, `parameters.expose` of `gas[1].prepare.variables.cryo_temp`), invalid `v2_prepare_unknown_key.toml`, `v2_prepare_unknown_script.toml`, `v2_pump_out_unknown_key.toml`

**Interfaces:**
- Produces: `using PrepareValue = std::variant<bool, std::int64_t, double, std::string>;` `struct Prepare { std::string script; std::map<std::string, PrepareValue> variables; };` `struct PumpOut { std::string valve; double time_s = 0; };` `Gas` gains `std::optional<Prepare> prepare; std::optional<PumpOut> pump_out;` (`pump_out.valve` resolved through the alias resolver like `inlet`). `PlanResolvers` gains `const model::IScriptResolver* scripts = nullptr;` (`has_script(name)`; null skips the check, as `check_queue` does). Duration adds `pump_out.time_s` and, per `prepare`, the variable `hold_s` when present (else 0; the estimate cannot know a script's length).
- Gate: Task 5's initial `unbuilt_features` list cannot name these keys (they do not exist before this task), so this task adds two entries to it: a gas with `prepare` → `gas[<g>].prepare: prepare: not run by this build yet`, a gas with `pump_out` → `gas[<g>].pump_out: pump_out: not run by this build yet`. Without them a plan with `prepare` would validate from here to Task 25 and the engine would skip the script, breaking the rule that a plan that validates runs as written. Task 25 removes both.
- [x] **Step 1: Write failing tests:** `StagedPlan.PrepareAndPumpOutParse` (values above, through `resolve_plan_ungated`); invalid fixtures `# expect: gas 'He': prepare: unknown key 'temp'`, `# expect: gas 'Ne': unknown script 'cryo:nope'`, `# expect: gas 'He': pump_out: unknown key 'valves'`; `PlanParameters.PrepareVariableIsExposable` (override `gas[1].prepare.variables.cryo_temp = 80` → resolved Ne variable 80, through `resolve_plan_ungated`); `StagedPlan.NoPrepareNoChange` (worked plan 7.6 resolves to the same golden as before this task); `PlanV2Gate.PrepareRefusedUntilTask25` (`load_plan` of `valid/v2_staged_cryo_release.toml` → error containing `gas[0].prepare: prepare: not run by this build yet` and `gas[0].pump_out: pump_out: not run by this build yet`).
- [x] **Step 2: Run** `--gtest_filter='StagedPlan.*:PlanParameters.PrepareVariableIsExposable:PlanSchema.EveryInvalidFixtureFailsWithItsRule:PlanV2Gate.*'` → FAIL.
- [x] **Step 3: Implement**, including the two gate entries above; `PlanGoldens.*` reads the staged fixture through `resolve_plan_ungated` as it does every gated fixture.
- [x] **Step 4: Run** → PASS.
- [x] **Step 5: Commit** `feat(experiment): prepare script and pump-out on a plan gas`.

Done notes: Step 2 was observed failing as build failures (no `prepare`, `pump_out`, `PrepareValue`). `PlanResolvers::scripts` is the queue's `IScriptResolver` (`model/queue_validation.hpp`; the lab's `LabScripts`, now built before the plan library and handed to it); without one the script is not checked. A variable is a TOML boolean, integer, float or string (anything else refused); `to_json` writes `prepare` and `pump_out` only when a gas has them, so no other golden changed. `PlanParameters.PrepareVariableIsExposable` checks the override in the effective text: the fixture's valves are lab aliases that test has no resolver for. Added: `StagedPlan.DurationCountsPumpOutAndHold`; `PlanV2Gate.AV1FileNeverTripsIt` skips the staged fixture (gated until Task 25).

### Task 25: `Block::Prepare` and `Block::PumpOut` in the engine

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/measurement/ports.hpp`, `libs/experiment/include/pychron/experiment/measurement/engine.hpp`, `libs/experiment/src/measurement/engine.cpp`, `libs/experiment/src/plan/plan_parse.cpp` (gate), `apps/pychron-ui/src/executor_pane.cpp` (timeline label `prepare <gas>` / `pump out <gas>` from `BlockStarted::gas`)
- Test: `tests/experiment/test_measurement_engine.cpp`, `tests/experiment/test_plan_v2.cpp`, the UI test that covers the executor pane's timeline (`grep -ln executor_pane tests/ui`)

**Interfaces:**
- Produces:
  - `struct PrepareRequest { std::string gas; std::string script; std::map<std::string, plan::PrepareValue> variables; };` `struct PrepareReport { TimePoint started{}, finished{}; std::map<std::string, std::string> variables; };` (variables as given to the script, as text)
  - `class IPrepareScriptPort { public: virtual ~IPrepareScriptPort() = default; virtual Result<PrepareReport> run(const PrepareRequest& request, scripting::CancelToken& token) = 0; };`
  - `EngineContext` gains `IPrepareScriptPort* prepare = nullptr;` — `validate()` requires it when any gas has `prepare`, and `valves` when any gas has `pump_out`.
  - Engine, per gas with `prepare`: `Block::Prepare` (`BlockStarted{run_id, Prepare, gas}`) runs `prepare->run` after the previous gas's last segment and pump-out and before this gas's `before_inlet`; the first gas's runs before its `before_inlet`. `Block::PumpOut`: after the gas's last segment, `open(valve)`, `wait(time_s)`, `close(valve)` (close through `critical`, as the inlet close). `MeasurementResult` gains `std::vector<GasOutcome> gases;` with `struct GasOutcome { std::string name; std::optional<PrepareReport> prepare; std::optional<plan::PumpOut> pump_out; };`.
  - A failed `prepare` → outcome `Failed`, `error` names the gas (`prepare for gas 'Ne': <error>`), the data of gases already measured stays in `result.data`.
- [x] **Step 1: Write failing tests** (fakes: `FakePrepare` journals requests and can fail or block on the token):
  - `StagedRelease.OrderPrepareInletSegmentsPumpOut`: two gases with `prepare`, first with `pump_out` → trace `prepare He`, `inlet open/close He`, He segments, `pump_out He` (open, 120 s on the ManualClock, close), `prepare Ne`, `inlet Ne`, Ne segments.
  - `StagedRelease.VariablesReachTheScript`: `FakePrepare` saw `cryo_temp == 35` then `75`, `hold_s == 300`.
  - `StagedRelease.FailingSecondPrepareKeepsFirstGas`: second `prepare` fails → outcome `Failed`, error contains `gas 'Ne'`, He's signal series present in `result.data`, no Ne inlet opened.
  - `StagedRelease.CancelDuringPrepareKeepsEarlierGases` (Review Focus 2): token cancelled while `FakePrepare` waits → outcome `Cancelled`, `FakeValves` shows no open inlet, He data kept.
  - `StagedRelease.ValidateNeedsThePorts`: plan with `prepare` and `ctx.prepare == nullptr` → `validate()` error.
  - `StagedPlan.StagedFixtureLoads` (in `tests/experiment/test_plan_v2.cpp`, replacing `PlanV2Gate.PrepareRefusedUntilTask25`): `load_plan` of `valid/v2_staged_cryo_release.toml`, gated → `ok()`.
- [x] **Step 2: Run** `--gtest_filter='StagedRelease.*:StagedPlan.StagedFixtureLoads'` → FAIL.
- [x] **Step 3: Implement**; drop the `prepare`/`pump_out` gate entries Task 24 added and the test `PlanV2Gate.PrepareRefusedUntilTask25`; the UI label (build with `dev-ui`, sweep with `--build-dir build/dev-ui`).
- [x] **Step 4: Run** the engine filter and the UI test → PASS.
- [x] **Step 5: Commit** `feat(experiment): prepare and pump-out blocks between gases`.

Done notes: Step 2 was observed failing as build failures (no `IPrepareScriptPort`). A prepare that fails with the token stopped is the stop's outcome (Cancelled, Aborted), not a failure; one that fails otherwise fails the measurement with `prepare for gas '<n>': <error>`. A pump-out is skipped once the run is stopping and closes its valve through `critical` whatever the wait gave; `MeasurementResult::gases` has an entry for each gas with a prepare or a pump-out. Traced as `prepare <gas> <script>` and `pump_out <gas> <valve> <s>`. The executor pane names the blocks `prepare <gas>` and `pump out <gas>` (`TestExperimentWindow::aPrepareAndAPumpOutAreNamedWithTheirGas`). `FakePrepare`'s "blocks on the token" is the token cancelled from inside the script, as an operator's stop reaches it.

### Task 26: The run runs `prepare` scripts and records them

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/run/run.hpp`, `libs/experiment/src/run/run.cpp`, `libs/experiment/src/record/builder.cpp`, `libs/experiment/adapters/store/src/record_ingest.cpp` (not stored: `GasRec::prepare`/`pump_out` go in the e2e spec's not-stored list, `docs/superpowers/specs/2026-10-10-e2e-sim-experiment-design.md`)
- Test: `tests/experiment/test_run.cpp`, `tests/experiment/test_record_ingest.cpp`, `tests/scripting/test_script_host.cpp`

§11.6 "a `prepare` script is an extraction script: no measurement host calls" is enforced by what exists, once the script runs as `ScriptKind::Extraction`: an extraction script's namespace holds only the extraction vocabulary (`libs/scripting/src/vocabulary.cpp`); measurement calls (`position`, `acquire`, `add_conditional`, `truncate`, the Task 18 additions) live on the `HookApi` object, handed only to a `MeasurementHook`'s functions (`python_host.cpp`); the static check reports a call to a name the script does not define, and `run` refuses a script that fails the check (`ScriptHostTest.RunRefusesScriptsThatFailTheCheck`). This task pins both halves: the run gives a `prepare` script the extraction kind, and that kind cannot reach a measurement call.

**Interfaces:**
- Produces: `class RunPrepareScripts final : public measurement::IPrepareScriptPort` (in `run.cpp`, anonymous namespace): resolves `request.script` with `scripting::ScriptKind::Extraction` through `RunServices::resolver`, runs it with `RunServices::scripts->run(script, env, token)` where `env.context` is `Run::script_context()` with `request.variables` written over it (a plan variable wins over a run variable of the same name), on the run's clock. A plan whose gases have a `prepare` keeps the extraction resource from `Run::extract` until the measurement ends (no release at `run.cpp:438`; released after `measure`). `RecordBuilder` writes `GasRec::prepare` (`script`, `variables`, `started`/`finished` as ISO-8601 from `clock.wall_now()` at those points) and `GasRec::pump_out`. A failed `prepare` fails the run as a failed extraction does (`fail_run`), past the point of no return: record saved with status `failed`, first gas's data in it.
- [x] **Step 1: Write failing tests:** `RunTest.PrepareScriptsRunWithPlanVariables` (`FakeScriptHost` sees two extraction scripts named `cryo:release` with `cryo_temp` 35 then 75, and the run's own `cryo_temperature` global under them); `RunTest.StagedPlanHoldsTheExtractionLineForTheWholeRun` (hooks journal: no `release_extraction` between the extraction script and the end of the measurement; `release_extraction` comes after `release_spectrometer`); `RunTest.FailingPrepareSavesEarlierGases` (record status `failed`, failure text names gas `Ne`, He intercepts present); `RecordIngest.PrepareRecordIsNotStored` (row in the not-stored table of that file); `RunTest.PrepareScriptRunsAsExtractionKind` (`FakeScriptHost` journals the `ScriptKind` of each script it runs: both `cryo:release` runs are `ScriptKind::Extraction`); `RunTest.PrepareScriptRefusedByTheHostFailsTheGas` (`FakeScriptHost` answers the Ne `prepare` with the `Config` error the static check gives → run fails, failure text names gas `Ne` and carries the host's text, He data saved); `ScriptHostTest.ExtractionScriptCannotMakeMeasurementCalls` (real host: `host->run` of an `ScriptKind::Extraction` inline script whose `main` calls `position('Ar40', 'H1')` and `acquire(5, 1.0)` → error `Config`, text names the call, and the rig's log has no `position` or `acquire` line; the same calls through `call_hook` on a `MeasurementHook` reach the log, as `MeasurementHookGetsTypedApi` shows).
- [x] **Step 2: Run** `pychron_experiment_tests --gtest_filter='RunTest.Prepare*:RunTest.StagedPlan*:RunTest.FailingPrepare*'`, `pychron_experiment_store_tests` → FAIL; `pychron_scripting_tests --gtest_filter='ScriptHostTest.ExtractionScriptCannotMakeMeasurementCalls'` → PASS on first run, since it pins the existing mechanism. To see it red once: replace the two measurement calls with an extraction command from the vocabulary (`sleep(1)`) → FAIL (no error); restore → PASS. Its pass then rests on the measurement calls being refused, not on some other check.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run** → PASS; `ctest --preset dev-quick`.
- [x] **Step 5: Commit** `feat(experiment): runs execute and record gas prepare scripts`.

Done notes: Step 2 was observed failing as build failures (`kinds()`, `globals()` on the fake host; `PrepareReport` times). The port is `Run::PrepareScripts` (a nested class of `Run`, not an anonymous one: it reads the run's services, device and context), handed to the engine only when a gas has a prepare. Variables reach the script as globals over `script_context()`'s; in the record as text (integers plain, floats `%.15g`, booleans `true`/`false`). `PrepareReport` gained `started_utc`/`finished_utc` (the clock's wall time), the record's `GasRec::prepare` times. The extraction resource is kept from the extraction script to the end of the measurement (released after the spectrometer, `Run::release_held_extraction`), and released at once when the extraction failed or the run was stopped. `ScriptHostTest.ExtractionScriptCannotMakeMeasurementCalls`: in an extraction script `position` is the run's hole (a context global) and `acquire` takes a resource, so the measurement calls tried are `api.position(...)`, `add_conditional(...)` and `truncate()`, each refused by the static check naming it, with nothing moved; seen red by replacing the call with `sleep(1)`. `RecordIngest.PrepareRecordIsNotStored` compares the rows of a record with and without a gas's prepare and pump-out.

### Task 27: Queue validation refuses overlap with a staged plan

**Files:**
- Modify: `libs/experiment/src/model/queue_validation.cpp`, `libs/experiment/include/pychron/experiment/model/queue_validation.hpp`
- Test: `tests/experiment/test_queue_validation.cpp`

**Interfaces:**
- Produces: `IPlanResolver` (model side) gains `virtual bool holds_extraction_line(std::string_view plan, const ParamOverrides& overrides) const = 0;` (`PlanLibrary` answers: any gas has `prepare`). `check_queue` emits an error diagnostic on each row that asks for overlap (`overlap.duration > 0` on an unknown, the `Executor::overlaps` conditions that the queue can know) while its own plan holds the extraction line: text `row <n>: plan '<name>' holds the extraction line until its last gas; overlap is not possible`. That is §11.4 and nothing more. A row's overlap lets the next row's extraction start during this row's measurement (`executor.cpp`, `Executor::overlaps`); it is this row's plan whose `prepare` needs the line then. A staged plan on the next row is not refused: its extraction during the previous row's measurement is ordinary overlap, and its own `prepare` blocks run in its own measurement, after the previous row's.
- [x] **Step 1: Write failing tests:** `QueueValidation.OverlapWithStagedPlanIsRefused` (row 0 staged plan with overlap, row 1 any → one error, on row 0); `QueueValidation.OverlapBeforeStagedPlanIsFine` (row 0 ordinary plan with overlap, row 1 staged → no error); `QueueValidation.StagedPlanWithoutOverlapIsFine`; `QueueValidation.NullPlanResolverSkipsTheCheck`.
- [x] **Step 2: Run** `--gtest_filter='QueueValidation.*'` → FAIL.
- [x] **Step 3: Implement**; update every `IPlanResolver` implementation (grep `public IPlanResolver`).
- [x] **Step 4: Run** → PASS.
- [x] **Step 5: Commit** `feat(experiment): queue validation refuses overlap with staged plans`.

Done notes: Step 2 was observed failing as build failures (no `holds_extraction_line`). The row's conditions are the executor's the queue knows: an unknown, not skipped, `overlap.duration > 0`, with a later row that is not skipped (`allow_overlap` is the executor's option, not the queue's); the diagnostic is on field `overlap`. `PlanLibrary` loads the plan as advanced (any override key) to ask. Added: `RunTest.TheLibrarySaysWhichPlansHoldTheExtractionLine`.

### Task 28: Simulated cryotrap releases two fractions

Read `libs/sim/AGENTS.md` first.

**Files:**
- Modify: `libs/sim/include/pychron/sim/sim_system.hpp`, `libs/sim/src/sim_system.cpp`, `libs/sim/src/sim_config.cpp`, `configs/examples/sim.toml`, `docs/simulator.md`
- Create: `tests/integration/fixtures/staged/` (a lab with a cryotrap volume and `scripts/extraction/cryo_release.py` that calls `set_cryo(cryo_temp)`, waits `hold_s` on the line's clock, opens the trap valve), plan `v2_staged_cryo_release.toml` copied there
- Test: `tests/sim/test_sim_system.cpp` (or the file testing `SimSystem`), `tests/integration/test_lab_sim.cpp`, `tests/sim/` `SimConfig.TheExampleFileIsTheDefaults`

**Interfaces:**
- Produces: `sim.toml` `[[cryo_trap]] volume = "<name>", cryostat = "<name>", release_k = { He = 30, Ne = 70 }` — a pure `GasNetwork` rule: each species in the trap volume is held (no flow out) while the cryostat's simulated temperature is below its `release_k`, and flows normally above it. `SimSystem` applies it when moving the network to `clock.now()`. Default: no trap.
- [x] **Step 1: Write failing tests:** `SimCryoTrap.HoldsBelowReleaseTemperature` (He and Ne injected into a trap at 20 K: none leaves; at 35 K He leaves, Ne stays; at 75 K both); `SimConfig.CryoTrapKeysRangeChecked` (`release_k` ≤ 0 refused); `StagedReleaseSim.EachGasAppearsOnlyAfterItsRelease` (`VirtualTimeTest`, simulated lab, plan `v2_staged_cryo_release.toml`: He signal mean ≫ Ne signal mean in gas He; Ne's signal appears only in gas Ne; record has two `GasRec`s with `prepare.variables.cryo_temp` `35` and `75`).
- [x] **Step 2: Run** `pychron_sim_tests --gtest_filter='SimCryoTrap.*:SimConfig.*'`, `pychron_integration_tests --gtest_filter='StagedReleaseSim.*'` → FAIL.
- [x] **Step 3: Implement**; example `sim.toml` entry commented with its default; row in `docs/simulator.md`.
- [x] **Step 4: Run** → PASS; `ctest --preset dev-quick`.
- [x] **Step 5: Commit** `feat(sim): cryotrap that releases each species at its temperature`.

Done notes: Step 2 was observed failing as build failures (no `cryo_traps`, no He or Ne). The simulator had argon and `active` only, so `He4` and `Ne20` became species of `gas.hpp` (eight; masses 4.0026 and 19.992). Air has none of them here (a few ppm in nature: left out so the example lab's numbers stay those of its argon; a volume or a `[compositions.*]` gives them); a getter takes neither, a pump both, the ion source uses them as the argon; the beam reads them at their masses once the source holds any (lab simulator spec 3.1 and `docs/simulator.md` updated). The trap: `GasNetwork::set_held` (no valve carries a held species to or from the volume, either way: the flows stay symmetric, and gas reaching the cold surface would freeze too); `SimSettings::cryo_traps`, `[[cryo_trap]] volume, cryostat, input, release_k` (`cryostat` a `lakeshore` driver's name; `input` A to D, default A; release above 0 to 1e4 K); `SimSystem` applies it whenever it moves the network, at the cryostat's temperature then (`LakeshoreSim::current_temperature`, and `SimSystem::cryostat(driver)` for tests). A trap whose cryostat has no driver holds nothing. The integration test is `LabSim.StagedReleaseSimEachGasAppearsOnlyAfterItsRelease` (`tests/integration/test_lab_sim.cpp`): a scratch copy of the example lab, not a fixture directory under `tests/integration/fixtures/staged/` — the example lab already has the cryo stage behind Y and its Lake Shore; the test adds the trap's gas and rule to the copy's `sim.toml`, He and Ne points to its field table, `cryo_release.py` and a plan of two gases. Measured: He4 4.2e5 fA in gas He, Ne20 0.34 fA on AX in gas He (still frozen), Ne20 8.4e4 fA in gas Ne; 0.7 s real.

---

## Phase 8: Optics, interferences, drivers (§3.6, §6.2 IC factor, §5.3; spec item 7)

### Task 29: `[optics.<name>]` and `[interferences]` in the spectrometer config

**Files:**
- Modify: `libs/systems/include/pychron/systems/spectrometer/config.hpp`, `libs/systems/src/spectrometer/config_loader.cpp`, `libs/systems/include/pychron/systems/spectrometer/spectrometer.hpp`, `libs/systems/src/spectrometer/spectrometer.cpp`, `libs/systems/include/pychron/systems/spectrometer/roles.hpp` (or wherever `SpectrometerRoles` is declared), `libs/experiment/src/measurement/adapters.cpp`, `libs/experiment/src/plan/plan_parse.cpp` (optics gate), `libs/experiment/src/record/builder.cpp`
- Test: `tests/systems/` config and facade tests, `tests/experiment/test_measurement_engine.cpp`

**Interfaces:**
- Produces: `struct OpticsPreset { std::vector<std::pair<std::string, std::string>> params; };` (opaque to the plan: each key and its value as text, in file order; no toml type in `config.hpp`); `SpectrometerConfig` gains `std::map<std::string, OpticsPreset> optics;` and `std::vector<Interference> interferences;` with `struct Interference { std::string monitor, corrects; };` (TOML `[interferences] "Ar40++" = "Ne20"`). Role `class IOpticsControl { public: virtual ~IOpticsControl() = default; virtual Result<void> apply_optics(const std::string& name, const OpticsPreset& preset) = 0; virtual Result<void> set_source_output(bool on) = 0; };` optional in `SpectrometerRoles`; facade `Result<void> apply_optics(const std::string& name)`, `Result<void> set_source_output(bool on)`, `bool has_optics_control() const`. Catalog `has_optics(name)` true when the config defines it and the role exists; `has_source_output()` likewise. The simulator's source driver (`SimSourceCore`, `libs/sim/include/pychron/sim/spectrometer/sim_drivers.hpp`) implements `IOpticsControl` and hands the applied preset name to `BeamModel` (`void set_optics(std::string name); std::string optics() const;`), so tests observe it (§5.3). Each `SpectrometerState` records `optics`; the record's per-gas data carries `interferences` (`GasRec` gains `std::vector<std::pair<std::string, std::string>> interferences;`, written only when non-empty).
- [x] **Step 1: Write failing tests:** `SpectrometerConfig.OpticsAndInterferences`; `SpectrometerFacade.ApplyOpticsWithoutRoleIsRefused`; `PositionExec.OpticsBeforeMove` (worked plan 7.5: trace `optics ar_config_a` before the move, `optics ar_config_b` before position 2, every cycle); `AnalysisRecord.StateCarriesOptics`.
- [x] **Step 2: Run** those filters in `pychron_systems_tests` and `pychron_experiment_tests` → FAIL.
- [x] **Step 3: Implement**; drop the optics gate entry.
- [x] **Step 4: Run** → PASS.
- [x] **Step 5: Commit** `feat(systems): optics presets and interference relations in the spectrometer config`.

Done notes: Step 2 was observed failing as build failures. The role is a driver's `roles = [..., "optics"]` (`cfg::Role::Optics`), bound as `SpectrometerRoles::optics`, as `beam_blank` is; `IOpticsControl` lives in `libs/devices` `roles.hpp` beside the other roles and takes the preset as `OpticsParams` (pairs of text), so a driver does not depend on `libs/systems`. Preset values: strings as given, integers, floats (`%.15g`), booleans `true`/`false`, in file order (sorted by source position: a TOML table is not ordered). `SpectrometerCatalog::has_optics(name)` is true when a driver declares the role and the config has the preset; `has_source_output()` when a driver declares the role. `SimIntegrated` implements the role: the preset's name to `BeamModel::set_optics` (read back with `optics()`), and `set_source_output` turns every detector's gas off as a blank does. `SpectrometerPort::apply_optics("")` leaves the preset in effect. The record: `GasRec::interferences` (TOML `[[monitor, corrected], ...]`, written only when any; `v3.*` regenerated), filled from `RunServices::interferences`, which `LabSession` takes from the spectrometer config. The optics gate entry is gone (a sniff or whiff's optics stay gated), so every worked plan passes the gate; `PlanV2Gate.UnbuiltFeatureIsRefusedWithItsPath` now uses a sniff with optics. `OpticsBeforeMove` gives 7.5's positions an explicit `at` (with no reference anywhere they would be read in place, with no move to precede).

### Task 30: Same-run IC factor with its optics

Read `libs/processing/AGENTS.md` first.

**Files:**
- Modify: `libs/processing/include/pychron/processing/reference_fit.hpp`, `libs/processing/src/reference_fit.cpp`
- Test: `tests/processing/test_reference_fit.cpp` (or the file testing `reference_fit`)

**Interfaces:**
- Produces: `std::vector<ReferencePoint> same_run_references(const Analysis& analysis, std::string_view label, std::string_view reference_detector);` — for a label measured on several detectors in one run (worked plan 7.3, 7.7), one IC-factor point per non-reference detector: ratio of baseline-corrected intercepts, error propagated; each point carries the `optics` of the state its series was measured under. The IC-factor path (`ReferenceFitTarget::IcFactors`) accepts these as a source beside other runs; the stored IC factor keeps its optics name.
- [x] **Step 1: Write failing tests:** `ReferenceFit.SameRunIcFactorFromTwoDetectors` (Ar40 on H1 = 100, on AX = 98 after baselines, reference H1 → factor 100/98 on AX); `ReferenceFit.SameRunIcFactorKeepsOptics`.
- [x] **Step 2: Run** `pychron_processing_tests --gtest_filter='ReferenceFit.SameRun*'` → FAIL.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run** → PASS.
- [x] **Step 5: Commit** `feat(processing): IC factor from one run's intercepts`.

Done: `same_run_references` in `reference_fit.{hpp,cpp}`; `ReferencePoint` gains `detector`, `optics` (and a constructor, so old brace inits stay whole); `IsotopeData::optics` filled by `record_source` from the v3 state of the label's signal (`RecordSource.V3IsotopesCarryTheirStatesOptics`). Left out: the IC-factor figure using same-run points as a source and storing the optics name with the IC factor, which needs a store column and so a migration (spec 6.2; owner's call).

### Task 31: Qtegra driver: optics, source output, collector mode

Read `libs/devices/AGENTS.md` first: every new command gets a row in `tests/devices/test_repeat_tables.cpp`.

**Files:**
- Modify: `libs/devices/include/pychron/devices/spectrometer/thermo_qtegra.hpp`, `libs/devices/src/spectrometer/thermo_qtegra.cpp`, `libs/devices/src/spectrometer/thermo_qtegra_sim.cpp`, `libs/systems` bring-up that builds `SpectrometerRoles` for Qtegra
- Test: `tests/devices/` Qtegra tests, `tests/devices/test_repeat_tables.cpp`

**Interfaces:**
- Produces: the Qtegra driver implements `IOpticsControl`: `apply_optics` sends the preset's keys as Qtegra parameter sets in the preset's key order (sub-cup configuration, HR collector position, source tune; names as the preset gives them), `set_source_output` the source on/off command, and a collector mode switch for logical detectors (`H2` ↔ `H2(CDD)`), each through the existing link with the repeat rules its table row states. The sim answers each.
- [ ] **Step 1: Write failing tests:** `ThermoQtegra.ApplyOpticsSendsPresetInOrder`; `ThermoQtegra.SourceOutput`; `ThermoQtegra.CollectorModeSwitch`; rows in `test_repeat_tables.cpp` for each new command (whether it may be sent twice).
- [ ] **Step 2: Run** `pychron_devices_tests --gtest_filter='ThermoQtegra.*:RepeatTables.*'` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS; `ctest --preset dev-quick`.
- [ ] **Step 5: Commit** `feat(devices): Qtegra optics presets, source output and collector mode`.

---

## Phase 9: Legacy converter (§4.2; spec item 8)

### Task 32: `tools/pychron_measurement_import.py`

**Files:**
- Create: `tools/pychron_measurement_import.py`, `tools/tests/test_pychron_measurement_import.py`, `tools/tests/fixtures/measurement/` (written here in the legacy formats: `hops_with_mass_35.txt`, `ic_hops.txt`, `argonH.yaml`, `jan_ic_peak_hop3.py`, an ARGUS `multicollect` script with an `mx` docstring, `syn_extraction/default.yaml`, one script with an unmapped command)

**Interfaces:**
- Produces: CLI `python tools/pychron_measurement_import.py <script.py|hops.txt|hops.yaml> [--hops <file>] [--syn-extraction <yaml>] -o plan.toml [--report report.txt]`, same structure as `pychron_conditionals_import.py` (`convert(...) -> Result(toml: str, report: list[str])`). Mapping (§4.2): `Iso:Det:Defl` → entry with `deflection`; `bs:Mass:Det` → `kind = "baseline"`, `at = { mass, detector }`; YAML `active`, `protect`, `positioning.dac` → `at = { dac }`; `mx` docstring → `[equilibration]`, baseline and peak-center segments, counts, integration; more than one `multicollect`/`peak_hop` call → several segments; `set_time_zero`, `set_integration_time`, `set_deflection`, `generate_ic_mftable` between calls → steps (`time_zero`, `set.integration_s`, `set.deflection`, `peak_center` with `write_table`); `syn_extraction` YAML → a `baseline` segment at `during_extraction` plus `[baseline.modifier.<det>]`; anything else → a hook stub with the original command in a comment and a report line `line <n>: <command>: hook stub`.
- [x] **Step 1: Write failing tests:** one per fixture, asserting the converted TOML (parsed with `tomllib`) equals the matching worked plan's structure (7.2 from `hops_with_mass_35.txt`, 7.3 from `jan_ic_peak_hop3.py` + `ic_hops.txt`, 7.4 from `argonH.yaml`, 7.1 from the ARGUS script + `syn_extraction`); `test_unmapped_command_becomes_hook_stub_and_report_line`; when env `ELCTL` points at a built `elctl`, every output passes `elctl exp validate`-style plan check (find the plan-check verb in `apps/elctl/src/cli.cpp`).
- [x] **Step 2: Run** `python3 -m pytest tools/tests/test_pychron_measurement_import.py -q` → FAIL.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run** → PASS (and with `ELCTL=build/dev/apps/elctl/elctl`).
- [x] **Step 5: Commit** `feat(tools): convert legacy measurement scripts and hop files to v2 plans`.

Done: `tools/pychron_measurement_import.py` (`convert(path, hops=, syn=, reference=, cycles=, integration=, family=, ...) -> Result(toml, report, hooks)`), fixtures under `tools/tests/fixtures/measurement/` (the ARGUS script is `argus_multicollect.py`, the unmapped one `unmapped.py`), 12 tests, all passing with `ELCTL` set (the check is `elctl -c <lab>/extraction_line.toml exp validate` on a one-run queue in a copy of `configs/examples`, since the line resolves `@extraction.eqtime`). Choices the report states: a script's labels for `multicollect` come from the positioned isotope stepped one mass unit per cup along `--cups` (default ARGUS `H2 H1 AX L1 L2 CDD`); `set_time_zero()` between `equilibrate` and the first measurement becomes `time_zero = "on_inlet_close"` on the first signal segment, after it a `time_zero` step; `set_integration_time` before the first collection becomes that segment's `integration_s`, after it a `set` step; a hop's `at` is written only when its positioning is not its first entry; a `bs:<mass>:<det>` hop becomes an `after_main` baseline collecting each detector the hops read under its first label; a hops file alone takes `--cycles` (1 otherwise, reported), `--integration`, `--reference`. The `syn_extraction` YAML layout (`baseline` with counts, mass, detector, settling_time, integration_time; `modifier.<det>` with variable, model, error) is this converter's reading; no lab's file was at hand to check it against.

---

## Phase 10: Branch close

### Task 33: User guide, gate gone, full run

**Files:**
- Modify: `docs/user/04-experiments.md` (measurement plan section: segments, positions, gases, steps, `when`, `prepare`/`pump_out`, the worked plans as examples, the converter), `libs/experiment/src/plan/plan_parse.cpp` (`unbuilt_features` empty: delete it and `resolve_plan_ungated`)
- Test: everything

- [ ] **Step 1:** Confirm `unbuilt_features` returns nothing for every worked plan and the staged fixture; delete it and its test `PlanV2Gate.*`.
- [ ] **Step 2:** Write `MeasurementSimV2.EveryWorkedPlanRunsWithItsTrace` in `tests/integration/test_measurement_sim.cpp` (§8): each of the seven `v2_7*.toml` against simulated facades with a fixture spectrometer config for its family (ARGUS example configs for 7.1-7.3; the Task 10 Helix fixture for 7.4, 7.7; a Helix SFT fixture with `Faraday`/`SEM` for 7.6; a fixture with `F0`, `IC0`-`IC2` and optics presets `ar_config_a`/`ar_config_b` for 7.5), asserting the trace's positions, deflections, optics, settles, counts and kinds and the record keys (`Ar40@H2`, `Ar40@H1`, ... for 7.3; `Xe132@AX` and `Xe132@H2` apart for 7.7). Write the test only; the two new fixture configs (Helix SFT; `F0`/`IC0`-`IC2` with optics presets) are not written yet.
- [ ] **Step 2a: Run** `pychron_integration_tests --gtest_filter='MeasurementSimV2.EveryWorkedPlanRunsWithItsTrace'` → FAIL on 7.5 and 7.6 with the config loader's file-not-found error (fixtures absent). Any other plan failing names a real gap from an earlier task: fix at its cause.
- [ ] **Step 2b: Write the two fixtures** under `tests/integration/fixtures/` (`helix_sft_v2/`: `Faraday`, `SEM`; `optics_v2/`: `F0`, `IC0`-`IC2`, `[optics.ar_config_a]`, `[optics.ar_config_b]`). Run again → PASS. Commit `test(experiment): every worked plan in the simulator`.
- [ ] **Step 2c:** Write the user-guide section.
- [ ] **Step 3:** `cmake --build --preset dev-ui` (the branch touches `apps/pychron-ui`) → builds.
- [ ] **Step 4:** `ctest --preset dev-ui` → all pass.
- [ ] **Step 5:** `python3 tools/quality_check.py` → exit 0; `python3 tools/compiler_sweep.py` and `python3 tools/compiler_sweep.py --build-dir build/dev-ui` → no warning.
- [ ] **Step 6:** Commit `docs: measurement plan v2 in the user guide`.

## Not in this plan

From §11.7 and the spec's own scope: a gas measured on another spectrometer (the quadrupole on another install; needs a Quadera driver spec and an install-to-install hand-off spec); reduction of non-argon gases; conditionals that choose the next release temperature; the `Ar40++`/`CO2`/hydride correction calculations (a separate spec, §6.2); a Noblesse driver; a UI control for live `cycles` (the engine and `RunControl` take it, Task 15).

## Open questions

None.
