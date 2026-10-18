# Field Table Editor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the operator see, edit, version, compare and restore the spectrometer's field table (pychron's mftable) from a `pychron-ui` window and from `elctl spec table`, with the core refusing any change while a queue or scan holds the table.

**Architecture:** Pure checks and diffs (`check_field_table`, `diff_field_tables`) live in `libs/systems` beside `FieldTable`. `Spectrometer` gains `replace_table`, `restore_table_version`, a counted RAII `TableHold`, and a `FieldTableChanged` bus event; peak center saves its update (D2) and the store's `current` pointer gets one convention (D1). `elctl spec table` works on files through `FieldTableStore` only. The UI is a `FieldTableModel` (grid + undo) inside a `FieldTableEditorWindow`, which talks to core through `SpectrometerBridge`.

**Tech Stack:** C++20, toml++, GoogleTest (`tests/systems`, `tests/integration`, `apps/elctl/tests`), Qt 6 Widgets + QtTest (`tests/ui`), QCustomPlot.

**Spec:** `docs/superpowers/specs/2026-10-18-field-table-editor-design.md`

## Global Constraints

- Errors are `pychron::Result<T>` values; nothing in `libs/` throws for these paths.
- Time stamps from `Clock::wall_now()` (`CoreBridge::wall_now()` in the UI), never `system_clock::now()`.
- No version file is ever overwritten or deleted; `current` is the only file a restore writes.
- `current` holds `<version>.toml`; a bare stem is still read.
- Core refuses `replace_table` and `restore_table_version` while any `TableHold` is out; `update_table` and `save_table` never check holds.
- A table with any §3.2 error is never made current (Save, `set`, `import`, Make Current, `restore`).
- Error codes (`Error::code`) this plan introduces: `"table_held"`, `"table_conflict"`, `"table_invalid"`, `"table_axis"`.
- Diagnostics thresholds: monotonic check at 200 evenly spaced masses; residual warning above 1 % of the detector's value span; mass/weight mismatch warning above 0.01 amu; UI debounce 300 ms.
- Value cells display `%0.5f`; edit role keeps full precision (`QString::number(v, 'g', 17)`).
- Plot colours from `plot_style()` / `plot_series_color(slot)`, restyle on `PlotStyleHub::instance().changed()`; no colour literal (apps/pychron-ui AGENTS.md).
- Editor writes only under `<data_root>/tables/<name>/`; no `mark_as_user_file`.
- Shipped profiles and `configs/examples/tables/*/current` are not edited (libs/setup AGENTS.md).
- Each task: `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed` (`--build-dir build/dev-ui` for UI tasks), build the named target only, run its binary filtered. Task 16 runs the full suite.

## Review Focus

1. Table whose detector has exactly the minimum points for its fit, then a row is removed in the editor: Save must turn disabled with the fit message, not crash on the next plot redraw. Test: Task 13 `removingARowBelowTheFitMinimumDisablesSave`.
2. Two isotopes typed with the same name differing only by surrounding spaces (`"Ar40 "`): treated as duplicate after trimming, not as two rows. Test: Task 2 `CheckFieldTable.IsotopeNamesAreTrimmedBeforeTheDuplicateCheck`.
3. `current` file with Windows line ending and `.toml` suffix (`2026-09-30T000000.toml\r\n`) written by hand on a lab PC: read as that version. Test: Task 1 `FieldTableStoreTest.ReadsAPointerWithTomlSuffixAndCrlf`.
4. Import CSV whose header has a detector not in config (melbourne's `H2`): imported, column marked "not in config", warning only, Save allowed. Test: Task 4 `MftableCsv.ReadsTheMelbourneFileWithItsH2Column` plus Task 2 warning test.
5. Peak center finishes while editor is open on same table with unsaved edits: editor shows conflict banner, keeps edits, and Save then goes through the conflict dialog. Test: Task 13 `aTableChangedWhileEditingShowsTheBannerAndKeepsEdits`.

---

### Task 1: One `current` pointer convention (D1)

**Files:**
- Modify: `libs/systems/src/spectrometer/field_table_store.cpp` (`current_version`, `set_current`)
- Modify: `libs/systems/include/pychron/systems/spectrometer/field_table_store.hpp` (comment: `current` holds `<version>.toml`)
- Test: `tests/systems/test_field_table_store.cpp`

**Interfaces:**
- Produces: `FieldTableStore::current_version()` returns the bare version (no `.toml`) whichever form the file holds; `set_current` writes `"<version>.toml\n"`.

- [x] **Step 1: Write failing tests** in `FieldTableStoreTest`:
  - `SavedPointerIsAFileNameResolveFinds`: `save(table, when)` → read `current` → equals `v + ".toml\n"`; `cfg::resolve_current_table(root, name)` succeeds and its path's stem `== v`.
  - `ReadsAShippedStylePointer`: write `2026-09-30T000000.toml` with a valid table and `current` = `"2026-09-30T000000.toml\n"`; `current_version() == "2026-09-30T000000"`; `load()` succeeds.
  - `ReadsABareStemPointer`: `current` = `"<v>\n"` → `current_version() == v`.
  - `ReadsAPointerWithTomlSuffixAndCrlf`: `current` = `"2026-09-30T000000.toml\r\n"` → `"2026-09-30T000000"`.
  - `PointerWithTwoSuffixesIsRejected`: `"x.toml.toml"` → error (only one suffix stripped, then `valid_version` refuses `.`).
- [x] **Step 2: Run** `cmake --build --preset dev --target pychron_systems_tests && build/dev/tests/systems/pychron_systems_tests --gtest_filter='FieldTableStoreTest.*'`. Expected: the new tests fail.
- [x] **Step 3: Implement**: `current_version` trims trailing whitespace (`\n \r \t`), strips one trailing `.toml`, then `valid_version`; `set_current` writes `version + ".toml\n"`.
- [x] **Step 4: Run** same command, plus `--gtest_filter='*DataDir*:*Spectrometer*'`. Expected: PASS.
- [x] **Step 5: Commit** `fix(spectrometer): one current pointer form for field tables`.

### Task 2: `check_fit`, `clear_fit` and `check_field_table`

**Files:**
- Modify: `libs/systems/include/pychron/systems/spectrometer/field_table.hpp`, `libs/systems/src/spectrometer/field_table.cpp`
- Create: `libs/systems/include/pychron/systems/spectrometer/field_table_check.hpp`, `libs/systems/src/spectrometer/field_table_check.cpp`
- Test: `tests/systems/test_field_table_check.cpp`
- Modify: spec §3.2 last line to the real signature (below).

**Interfaces:**
- Produces (`field_table.hpp`):
  - `Result<void> FieldTable::check_fit(std::string_view det) const;` — same samples and fit as `value_for`, error message identical to what `value_for` would give; Discrete: ok when the detector has at least one point.
  - `void FieldTable::clear_fit(std::string_view det);` — drops the override.
  - `void FieldTable::set_default_fit(FitKind kind) noexcept;`
  - `std::vector<ControlPoint>& FieldTable::mutable_points() noexcept;` — for the editor model; the caller keeps the invariants that `check_field_table` checks.
- Produces (`field_table_check.hpp`, namespace `pychron::spectrometer`):
  ```cpp
  enum class TableSeverity { Error, Warning };
  struct TableDiagnostic {
    TableSeverity severity = TableSeverity::Error;
    std::optional<std::size_t> point;  // index into FieldTable::points()
    std::string detector;              // empty: row-level or table-level
    std::string message;
    friend bool operator==(const TableDiagnostic&, const TableDiagnostic&) = default;
  };
  using TableDiagnostics = std::vector<TableDiagnostic>;
  bool has_error(const TableDiagnostics& d) noexcept;
  const TableDiagnostic* first_error(const TableDiagnostics& d) noexcept;
  TableDiagnostics check_field_table(const FieldTable& table, const cfg::SpectrometerConfig& config,
                                     const MolecularWeights& weights, std::string_view name);
  std::string to_string(const TableDiagnostic& d);  // "error: row 3/H1: ..." form (row 1-based)
  ```
  Spec §3.2's `-> Diagnostics` becomes `-> TableDiagnostics` (spec edited in this task).

- [x] **Step 1: Write failing tests** (`CheckFieldTable.*`), one per rule, on a small 3-point quadratic table built in the test with config detectors `H1, AX` active, `[magnet] field_table = "argon"`:
  - Errors: `EmptyIsotope`, `DuplicateIsotope`, `IsotopeNamesAreTrimmedBeforeTheDuplicateCheck` (`"Ar40"` and `"Ar40 "`), `MassNotFiniteOrNotPositive` (NaN, 0, −1), `ValueNotFinite` (inf), `FitFailsWithTooFewPoints` (cubic on 3 points; message equals `table.check_fit("H1").error().what`), `RepeatedMasses`, `ActiveDetectorWithNoValueWhenTableIsTheFieldTable`, same for `hv_table`, `ActiveDetectorMissingIsNotAnErrorForAnotherTableName`.
  - Warnings: `NonMonotonicFit` (quadratic with vertex inside range), `ResidualAboveOnePercentOfSpan` (linear fit through 3 non-collinear points, one off by 2 % of span; 0.5 % gives none), `DetectorNotInConfig`, `IsotopeNotInWeights`, `MassDiffersFromWeightBy0.01Amu` (0.011 warns, 0.009 does not).
  - `CleanTableHasNoDiagnostics`; `HasErrorAndFirstError`.
  - `FieldTable.ClearFitFallsBackToDefault`, `FieldTable.SetDefaultFit`.
- [x] **Step 2: Run** `build/dev/tests/systems/pychron_systems_tests --gtest_filter='CheckFieldTable.*:FieldTable.*'` after building the target. Expected: compile failure (header missing).
- [x] **Step 3: Implement** the three `FieldTable` members and `check_field_table`. Monotonic: sample `value_for` at 200 masses evenly over `[min mass, max mass]` of the detector's points, warn when the sign of successive differences changes. Residual: `|value − value_for(mass)| > 0.01 × (max value − min value)` for that detector. Skip fit-dependent warnings for a detector whose `check_fit` failed or whose fit is Discrete.
- [x] **Step 4: Run** same filter. Expected: PASS.
- [x] **Step 5: Commit** `feat(spectrometer): field table diagnostics`.

### Task 3: `diff_field_tables`

**Files:**
- Modify: `field_table_check.hpp` / `.cpp` (Task 2)
- Test: `tests/systems/test_field_table_check.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct CellDiff { std::string isotope, detector; std::optional<double> old_value, new_value; };
  struct MassDiff { std::string isotope; double old_mass = 0, new_mass = 0; };
  enum class RowChange { Added, Removed };
  struct RowDiff { std::string isotope; RowChange change; };
  struct TableDiff {
    std::vector<std::string> fit_changes;  // "fit: quadratic -> linear", "fit H1: default -> cubic"; "axis: dac -> field" first if axis differs
    std::vector<RowDiff> rows;             // by isotope name, b's order then a's
    std::vector<MassDiff> masses;
    std::vector<CellDiff> cells;           // isotopes in both, union of detectors, differing only
    bool empty() const noexcept;
  };
  TableDiff diff_field_tables(const FieldTable& a, const FieldTable& b);  // a = old, b = new
  ```

- [x] **Step 1: Write failing tests** `DiffFieldTables.*`: `IdenticalIsEmpty`; `MatchesByIsotopeNotRowIndex` (same rows reordered → empty); `AddedAndRemovedRows`; `ChangedValueHasOldNewAndDelta` (`new − old` checked by test); `ValueOnlyOnOneSideHasNullopt`; `DifferentDetectorSetsStillMatchRows` (legacy failure case); `FitAndOverrideChangesListed`; `MassChangeListed`; `AxisChangeListedFirst`.
- [x] **Step 2: Run** filter `DiffFieldTables.*`. Expected: FAIL.
- [x] **Step 3: Implement** `diff_field_tables`.
- [x] **Step 4: Run** filter. Expected: PASS.
- [x] **Step 5: Commit** `feat(spectrometer): diff two field tables by isotope`.

### Task 4: `read_mftable_csv` reads legacy first-line fit name

**Files:**
- Modify: `libs/systems/src/spectrometer/mftable_csv.cpp`, header comment in `mftable_csv.hpp`
- Create: `tests/systems/data/mftable_melbourne.csv` (fixture)
- Test: `tests/systems/test_mftable_csv.cpp`

**Interfaces:**
- Consumes/Produces: `read_mftable_csv(text, weights, axis)` unchanged signature. A first non-blank line holding one token (no comma) is the table default fit: `linear` → Linear, `parabolic` → Quadratic, `cubic` → Cubic, anything else → Discrete. Without it the default stays quadratic. The existing `#,<fit>,...` row keeps working.

- [x] **Step 1: Get the fixture.** Download melbourne `spectrometer/mftables/mftable.csv` with the Drive connector (`search_files` under the melbourne setupfiles folder from the drive-setupfiles survey memory, then `download_file_content`; never trust a search snippet for bytes). When the connector is not reachable, write the fixture by hand in the surveyed shape: line 1 `parabolic`, line 2 `iso,H2,H1,AX,L1,L2,CDD`, rows `Ar40`, `Ar39`, `Ar36` with values between 5.35 and 6.36, CRLF line endings; say in the commit body which was done. Add the data dir to the test target's compile definitions as `PYCHRON_SYSTEMS_TEST_DATA_DIR` if no such definition exists in `tests/systems/CMakeLists.txt`.
- [x] **Step 2: Write failing tests** `MftableCsv.*`: `FirstLineFitNameLinear|Parabolic|Cubic|OtherIsDiscrete` (inline strings); `ReadsTheMelbourneFileWithItsH2Column` (fixture: 3 points, detectors include `H2`, default fit Quadratic, Ar40 mass from `MolecularWeights::defaults()`); `HashFitRowStillRead` (existing form, kept passing).
- [x] **Step 3: Run** filter `MftableCsv.*`. Expected: new tests FAIL.
- [x] **Step 4: Implement** the first-line rule.
- [x] **Step 5: Run** filter. Expected: PASS.
- [x] **Step 6: Commit** `feat(spectrometer): read legacy mftable fit line`.

### Task 5: `TableHold`, `table_version`, `FieldTableChanged`

**Files:**
- Modify: `libs/systems/include/pychron/systems/spectrometer/spectrometer.hpp`, `libs/systems/src/spectrometer/spectrometer.cpp`
- Test: `tests/systems/test_spectrometer_tables.cpp` (new; uses `spectrometer_fakes.hpp`)

**Interfaces:**
- Produces (in `spectrometer.hpp`):
  ```cpp
  struct FieldTableChanged { std::string name; std::string version; };  // events section
  class Spectrometer {
    class TableHold {  // move-only RAII; default-constructed holds nothing
     public: TableHold() = default; TableHold(TableHold&&) noexcept; TableHold& operator=(TableHold&&) noexcept;
             TableHold(const TableHold&) = delete; TableHold& operator=(const TableHold&) = delete; ~TableHold(); void release();
     private: friend class Spectrometer; Spectrometer* owner_ = nullptr; std::uint64_t id_ = 0;
    };
    [[nodiscard]] TableHold hold_table(std::string holder);
    std::optional<std::string> table_held() const;          // first holder still out
    Result<std::string> table_version(const std::string& name) const;  // store's current; Config error without data_root
    const std::filesystem::path& data_root() const noexcept;
  };
  ```
  Holds are kept as `std::vector<std::pair<std::uint64_t, std::string>>` under a plain `std::mutex holds_mutex_` (a few fields, never held across clock time), not `mutex_`, so `table_held()` from the UI thread never waits behind a settling move. `replace_table` reads them while holding `mutex_` and then `holds_mutex_`. This departs from spec §5 ("under `mutex_`"): edit that sentence of the spec in this task.
- `save_table(when)` publishes `FieldTableChanged{active_table(), version}` after a successful save.

- [x] **Step 1: Write failing tests** `SpectrometerTables.*`: `HoldIsCountedAndNamed` (two holds "queue", "peak_center" → `table_held() == "queue"`; release first → `"peak_center"`; release both → nullopt); `MovedHoldReleasesOnce`; `TableVersionReadsStoreCurrent`; `TableVersionWithoutDataRootIsConfigError`; `SaveTablePublishesFieldTableChanged` (subscribe on the fake's bus, check name and version equal the returned version).
- [x] **Step 2: Run** `build/dev/tests/systems/pychron_systems_tests --gtest_filter='SpectrometerTables.*'`. Expected: FAIL to compile.
- [x] **Step 3: Implement** the members.
- [x] **Step 4: Run** filter, then `--gtest_filter='Spectrometer*'`. Expected: PASS.
- [x] **Step 5: Commit** `feat(spectrometer): table holds and change event`.

### Task 6: `replace_table` and `restore_table_version`

**Files:**
- Modify: `spectrometer.hpp`, `spectrometer.cpp`
- Test: `tests/systems/test_spectrometer_tables.cpp`

**Interfaces:**
- Consumes: `check_field_table`, `has_error`, `to_string(TableDiagnostic)` (Task 2); `TableHold`, `FieldTableChanged` (Task 5).
- Produces:
  ```cpp
  Result<std::string> replace_table(const std::string& name, FieldTable table, std::string_view base_version,
                                    std::chrono::system_clock::time_point when);
  Result<void> restore_table_version(const std::string& name, const std::string& version);
  ```
  Order inside `replace_table`, all under `mutex_`: hold check (`code = "table_held"`, what `"table is held by <holder>"`), axis vs `native_axis()` (`"table_axis"`), `check_field_table(table, config_, weights_, name)` errors (`"table_invalid"`, what = one `to_string` line per error), store current ≠ `base_version` when `base_version` non-empty (`"table_conflict"`, what names the current version), `FieldTableStore::save`, swap `tables_[name]` when loaded, publish `FieldTableChanged`. `restore_table_version`: hold check, `store.load(version)` (parse failure → `"table_invalid"`), diagnostics errors → `"table_invalid"`, `store.restore`, reload `tables_[name]` when loaded, publish.

- [ ] **Step 1: Write failing tests** `SpectrometerTables.*`:
  - `ReplaceWritesVersionAndSwapsInMemory` (`field_table()` equals the new table, `table_version` equals result).
  - `ReplaceRefusesConflict` (save once through store directly, then replace with the old base → `code == "table_conflict"`; empty base succeeds).
  - `ReplaceRefusesAxisMismatch`, `ReplaceRefusesDiagnosticError` (duplicate isotope); each: store current and `field_table()` unchanged.
  - `ReplaceRefusedWhileHeldAcceptedAfter`, `RestoreRefusedWhileHeldAcceptedAfter` (store current and in-memory table unchanged while held).
  - `SaveTableNotRefusedUnderHold`, `UpdateTableNotRefusedUnderHold`.
  - `RestoreReloadsInMemory`; `RestoreRefusesVersionWithDiagnosticError` and `RestoreRefusesUnparseableVersion` (write the bad file by hand; `current` file bytes unchanged).
  - `BothPublishFieldTableChanged`.
- [ ] **Step 2: Run** filter `SpectrometerTables.*`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** filter. Expected: PASS.
- [ ] **Step 5: Commit** `feat(spectrometer): replace and restore field table versions in core`.

### Task 7: Peak center saves its update (D2); jobs hold the table

**Files:**
- Modify: `libs/systems/include/pychron/systems/jobs/peak_center.hpp` (`PeakCenterResult`: add `std::vector<std::string> notes;`), `libs/systems/src/jobs/peak_center.cpp` (`run_peak_center`, `apply`), `libs/systems/src/jobs/sweep.cpp` (`Sweep::run`)
- Test: `tests/systems/test_jobs_peak_center.cpp`, `tests/systems/test_jobs_sweep.cpp`

**Interfaces:**
- Consumes: `Spectrometer::save_table`, `hold_table`, `table_held` (Task 5).
- Produces: after `update_table` succeeds, `apply` calls `spec_.save_table(clock.wall_now())` (the spectrometer's context clock; expose `const Clock& Spectrometer::clock() const noexcept` if no accessor exists). Save failure: result stays ok, `notes` gets `"field table not saved: <what>"`, a warning is logged, in-memory table kept. `run_peak_center` takes `hold_table("peak_center")`, `Sweep::run` takes `hold_table("sweep")`, for their whole run.

- [ ] **Step 1: Write failing tests**: `PeakCenterRun.UpdateTableWritesANewVersionStampedFromTheClock` (data root in a temp dir with the table saved once; after the job `table_version` differs and equals `format_version(clock.wall_now())` at the save, using the test's virtual or manual clock); `PeakCenterRun.SaveFailureLeavesSuccessWithANote` (make `tables/<name>` read-only with `fs::permissions`; restore permissions in TearDown; result ok, `notes` has one entry starting `"field table not saved"`, `field_table()` has the new value); `PeakCenterRun.HoldsTheTableWhileRunning` and `Sweep.HoldsTheTableWhileRunning` (record `table_held()` from the progress callback or a fake acquirer read: `"peak_center"` / `"sweep"`; nullopt after).
- [ ] **Step 2: Run** `--gtest_filter='PeakCenterRun.*:Sweep*'`. Expected: new tests FAIL.
- [ ] **Step 3: Implement.** Update `docs/user/03-spectrometer.md` near line 425 only if wording no longer matches (the sentence is now true; add that a save failure is shown as a note).
- [ ] **Step 4: Run** same filter. Expected: PASS. Then `ctest --preset dev-quick` (peak center results reach `libs/experiment` and UI views).
- [ ] **Step 5: Commit** `fix(spectrometer): peak center saves its field table update`.

### Task 8: Executor holds the table for a queue run

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/executor/executor.hpp` (`ExecutorContext`: add `spectrometer::Spectrometer* spectrometer = nullptr;`), `libs/experiment/src/executor/executor.cpp` (`execute`), `libs/experiment/src/lab/session.cpp` (set `ctx.spectrometer = hw.spectrometer`)
- Test: `tests/integration/test_lab_session.cpp`

Read `libs/experiment/AGENTS.md` first.

**Interfaces:**
- Consumes: `Spectrometer::hold_table`, `table_held` (Task 5).
- Produces: `Executor::execute` takes `hold_table("queue")` for its whole body when `context.spectrometer` is set; released before `execute` returns.

Spec §8 places this test under `tests/experiment`; `tests/experiment` executor tests run without a real `Spectrometer`, while `LabSessionTest` assembles one, so the test lives in `tests/integration`. Edit spec §8's sentence to say so in this task.

- [ ] **Step 1: Write failing test** `LabSessionTest.TheQueueHoldsTheFieldTableWhileItRuns`: subscribe `executor::RunStarted`, record the spectrometer's `table_held()` in the handler; after `QueueEnded`, assert every recorded value `== "queue"` and `table_held()` is nullopt.
- [ ] **Step 2: Run** `cmake --build --preset dev --target pychron_integration_tests && build/dev/tests/integration/pychron_integration_tests --gtest_filter='LabSessionTest.*'`. Expected: new test FAILS.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same filter, then `pychron_experiment_tests --gtest_filter='Executor*'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(experiment): a running queue holds the field table`.

### Task 9: `elctl spec table` read commands

**Files:**
- Create: `apps/elctl/src/spec_table.hpp`, `apps/elctl/src/spec_table.cpp`
- Modify: `apps/elctl/src/cli.cpp` (dispatch `spec`, usage text)
- Test: `apps/elctl/tests/test_spec_table_commands.cpp`

**Interfaces:**
- Consumes: `FieldTableStore`, `cfg::load_spectrometer`, `check_field_table`, `diff_field_tables`.
- Produces: `int spec_command(const std::vector<std::string>& args, const ExpGlobals& globals, Io io);` in namespace `elctl`. Spectrometer file: `--spectrometer <file>` else `globals.spectrometer` (install's); data root = `load_spectrometer(file)->root`. Subcommands here: `table list`, `table show [<name>] [--version V]`, `table versions <name>`, `table diff <name> <v1> [<v2>]`, `table check [<name>]`. Exit: 0 ok, 1 (`kFailed`) on check error or command failure, usage code on bad args.
  - `list`: one line per directory under `<root>/tables` with a `current` file: `<name>  <current version>[  (active)][  (HV)]`.
  - `show`: header `Isotope Mass <detectors…>`, rows sorted by mass, values `%0.5f`, blank as `-`; then `fit:` and overrides.
  - `diff`: fit changes, `+ <iso>` / `- <iso>`, cells `<iso> <det> <old> -> <new> (Δ <d>)`.
  - `check`: one `to_string(TableDiagnostic)` per line; default name = config `[magnet] field_table`.

- [ ] **Step 1: Write failing tests** `SpecTableCmd.*` on a copy of `PYCHRON_EXAMPLE_CONFIGS_DIR` (as `LaserCmd`): `ListMarksActiveAndHv`, `ShowDefaultsToActiveCurrent`, `ShowAVersion`, `VersionsNewestFirst`, `DiffByIsotope`, `CheckCleanTableExitsZero`, `CheckErrorExitsOne` (hand-written version with duplicate isotope, pointer moved to it), `UnknownTableFails`.
- [ ] **Step 2: Run** `cmake --build --preset dev --target pychron_elctl_tests && build/dev/apps/elctl/tests/pychron_elctl_tests --gtest_filter='SpecTableCmd.*'` (binary path: the test target's output directory under `build/dev`; check once). Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** filter. Expected: PASS.
- [ ] **Step 5: Commit** `feat(elctl): spec table list, show, versions, diff, check`.

### Task 10: `elctl spec table` write commands

**Files:**
- Modify: `apps/elctl/src/spec_table.cpp`, `cli.cpp` usage text
- Test: `apps/elctl/tests/test_spec_table_commands.cpp`

**Interfaces:**
- Consumes: Task 9; `FieldTable::update`, `read_mftable_csv`, `format_version`.
- Produces: `table set <name> <det> <isotope> <value> [--propagate|--no-propagate]` (default config `[magnet] propagate`), `table restore <name> <version>`, `table import <name> <mftable.csv>`. `set` and `import` load current, apply, run `check_field_table`; any error → print each, exit 1, nothing written; else `FieldTableStore::save(table, pychron::SteadyClock{}.wall_now())`: elctl opens no device and runs no simulation here, so real wall time. Import keeps the current version's axis and uses config weights. `restore`: load target (parse failure = error), diagnostics; error → exit 1, `current` unchanged. Every write prints `wrote <name> <version>; takes effect when a process next loads the table`.

- [ ] **Step 1: Write failing tests**: `SetWritesANewVersionAndPropagates`, `SetRefusedOnDiagnosticErrorExitsOne` (value `nan`: "value not finite" error, exit 1, versions unchanged), `ImportTheMelbourneFixture` (Task 4 fixture; H2 warning printed, exit 0, new version current), `ImportRefusedOnError` (CSV with duplicate isotope), `RestoreMovesPointer`, `RestoreToVersionWithErrorRefusedCurrentUnchanged` (compare `current` bytes), `OutputSaysWhenItTakesEffect`.
- [ ] **Step 2: Run** filter `SpecTableCmd.*`. Expected: new tests FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** filter. Expected: PASS.
- [ ] **Step 5: Commit** `feat(elctl): spec table set, restore, import`.

### Task 11: `SpectrometerBridge` table operations (D3)

**Files:**
- Modify: `apps/pychron-ui/src/spectrometer_bridge.hpp`, `spectrometer_bridge.cpp`
- Test: `tests/ui/test_spectrometer_bridge.cpp`

**Interfaces:**
- Consumes: Tasks 5, 6.
- Produces:
  ```cpp
  std::filesystem::path data_root() const;
  const spectrometer::cfg::SpectrometerConfig& config() const;
  const spectrometer::MolecularWeights& weights() const;
  QString active_table() const;
  std::optional<QString> table_held() const;            // straight call: holds use their own small mutex
  spectrometer::FieldTable table_copy(const QString& name) const;  // in-memory copy if loaded, else empty
  void replace_table(const QString& name, spectrometer::FieldTable table, const QString& base_version);
  void restore_table_version(const QString& name, const QString& version);
  signals:
  void fieldTableChanged(const QString& name, const QString& version);
  void tableSaved(const QString& name, const pychron::Result<std::string>& result);
  void tableRestored(const QString& name, const QString& version, const pychron::Result<void>& result);
  ```
  `replace_table` / `restore_table_version` run through the bridge's `run(...)` executor thread with `when` from the bridge's wall clock, result posted back. `FieldTableChanged` relayed with `relay<>`; on it the bridge re-reads its table copy (isotope lists, `mass_on_reference_for`) before emitting `fieldTableChanged`.

- [ ] **Step 1: Write failing tests** (private slots): `replaceTableEmitsSavedAndChanged`, `isotopesFollowAPeakCenterStyleSave` (call `spec->update_table` + `save_table` from the core side, wait for `fieldTableChanged`, `isotopes_for` reflects the new row added through `replace_table`), `replaceRefusedWhileHeldReportsTheCode` (`result.error().code == "table_held"`), `restoreEmitsRestored`.
- [ ] **Step 2: Run** `cmake --build --preset dev-ui --target pychron_ui_test_spectrometer_bridge && build/dev-ui/tests/ui/pychron_ui_test_spectrometer_bridge`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same. Expected: PASS.
- [ ] **Step 5: Commit** `feat(ui): spectrometer bridge saves and restores field tables`.

### Task 12: `FieldTableModel` (grid and undo)

**Files:**
- Create: `apps/pychron-ui/src/field_table_model.hpp`, `field_table_model.cpp`; add to `apps/pychron-ui/CMakeLists.txt` source list
- Test: `tests/ui/test_field_table_model.cpp`

**Interfaces:**
- Consumes: `FieldTable` mutators (Task 2), `MolecularWeights`.
- Produces:
  ```cpp
  class FieldTableModel : public QAbstractTableModel {
   public:
    FieldTableModel(std::vector<std::string> config_detectors, spectrometer::MolecularWeights weights, QObject* parent = nullptr);
    void set_table(spectrometer::FieldTable table);   // resets, clears undo stack, marks clean
    const spectrometer::FieldTable& table() const noexcept;
    QUndoStack* undo_stack() noexcept;
    bool modified() const;                            // !undo_stack()->isClean()
    void mark_saved();                                // setClean()
    const std::vector<std::string>& detector_columns() const noexcept;  // config order, then table-only
    bool in_config(const std::string& detector) const;
    int column_of(const std::string& detector) const; // -1 if absent
    static constexpr int kIsotopeColumn = 0, kMassColumn = 1, kFirstDetectorColumn = 2;
    void add_row(const QString& isotope);             // mass from weights, else 0 (diagnostics flag it)
    void remove_rows(std::vector<int> rows);
    Result<void> set_value(const std::string& det, const std::string& isotope, double value, bool propagate);
    void set_default_fit(spectrometer::FitKind kind);
    void set_detector_fit(const std::string& det, std::optional<spectrometer::FitKind> kind);
    // QAbstractTableModel: rowCount, columnCount, data, setData, flags, headerData
   signals:
    void tableEdited();
  };
  ```
  Rows sorted by mass after every edit. Every mutation is a `QUndoCommand` holding the whole `FieldTable` before and after (tables are tens of rows: copy is fine and keeps undo exact). `data`: Display `%0.5f`, Edit full precision, ToolTip residual `value − fit` when fit succeeds, header ToolTip/`" (not in config)"` suffix for table-only detectors. `setData`: empty text on a value cell removes that detector's value; non-numeric or non-finite text refused (returns false); isotope commit trims and fills mass from weights when mass is 0 or equals the old isotope's weight; mass must be finite and > 0.

- [ ] **Step 1: Write failing tests**: `columnsAreConfigOrderThenTableOnly`, `valuesDisplayFiveDecimalsEditFullPrecision`, `clearingACellRemovesTheValue`, `nonFiniteTextIsRefused`, `isotopeCommitFillsMassFromWeights`, `isotopeCommitKeepsAHandTypedMass`, `rowsStaySortedByMass`, `addAndRemoveRows`, `setValuePropagates` (matches `FieldTable::update`), `undoRedoRestoresTableExactly`, `modifiedFollowsTheCleanState`, `fitChangesAreUndoable`.
- [ ] **Step 2: Run** `cmake --build --preset dev-ui --target pychron_ui_test_field_table_model && build/dev-ui/tests/ui/pychron_ui_test_field_table_model`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same. Expected: PASS.
- [ ] **Step 5: Commit** `feat(ui): field table grid model with undo`.

### Task 13: `FieldTableEditorWindow`: lists, grid, diagnostics, Save, Make Current, Revert

**Files:**
- Create: `apps/pychron-ui/src/field_table_editor_window.hpp`, `field_table_editor_window.cpp`; add to CMake source list
- Test: `tests/ui/test_field_table_editor.cpp` (uses `spectrometer_fixture.hpp`, shipped qtegra profile tables)

**Interfaces:**
- Consumes: Tasks 2, 3, 11, 12.
- Produces:
  ```cpp
  class FieldTableEditorWindow : public QMainWindow {
   public:
    enum class Unsaved { Save, Discard, Cancel };
    enum class Conflict { Reload, SaveAnyway, Cancel };
    // bridge null: no spectrometer; saves go straight to FieldTableStore with `wall_clock`.
    FieldTableEditorWindow(SpectrometerBridge* bridge, std::filesystem::path data_root,
                           spectrometer::cfg::SpectrometerConfig config, spectrometer::MolecularWeights weights,
                           std::function<QDateTime()> wall_clock, QWidget* parent = nullptr);
    void open_table(const QString& name);                   // current version
    void open_version(const QString& name, const QString& version);
    QString open_name() const; QString open_version() const; QString base_version() const;
    FieldTableModel* model() noexcept;
    QStringList table_names() const; QStringList versions() const;  // newest first
    void check_now();                                       // skip the 300 ms debounce
    void save();  void make_current(const QString& version);  void revert();
    bool save_enabled() const; QString save_disabled_reason() const;
    bool make_current_enabled(const QString& version) const; QString make_current_disabled_reason(const QString& version) const;
    QString banner_text() const;                            // empty when hidden
    // Injection points, as ConditionalsEditorWindow:
    void set_ask_unsaved(std::function<Unsaved(const QString&)>);
    void set_ask_conflict(std::function<Conflict(const QString& now_version)>);
    void set_confirm(std::function<bool(const QString& question)>);   // comments-lost, Make Current
    void set_show_error(std::function<void(const QString&)>);
   signals:
    void saved(const QString& name, const QString& version);
   protected:
    void closeEvent(QCloseEvent*) override;
    void changeEvent(QEvent*) override;  // ActivationChange: reread version list
  };
  ```
  Behaviour per spec §3, §3.1–3.4: left lists (names with `(active)` / `(HV)`, versions with points and detectors counts, current marked); right panel (default fit combo, one override combo per detector with "default", axis label, diagnostics list clicking selects the cell); in-memory-differs banner when `bridge->table_copy(name) != store.load()`; `fieldTableChanged` on the open table reloads when unmodified, else shows banner `"table changed since you opened it (now <v>)"` and keeps edits; Save disabled with reason while `bridge->table_held()` (text `"table is held by <holder>"`) or diagnostics have an error (first error's text); first save over a version whose text ≠ `to_toml(parse(text))` asks `"comments in <v> are not kept"` once per version; a version that does not parse is shown read-only with its parse error; save/restore failures from the bridge go to `show_error`. Undo and Redo actions from `model()->undo_stack()->createUndoAction/createRedoAction` with `QKeySequence::Undo` / `QKeySequence::Redo` (Ctrl+Z / Ctrl+Shift+Z). Make Current applies Save's conflict rule: before restoring, it rereads the store's current version; when that differs from the current version the window last listed, it refuses with `show_error("table changed since you opened it (now <v>)")` and rereads the list. Poll `table_held()` on a 500 ms `QTimer` for enable state (UI convenience only; core refuses anyway).

- [ ] **Step 1: Write failing tests** (temp copy of `profiles/qtegra` data root, `SimSpectrometer` from the fixture):
  - `opensTheActiveTableAtItsCurrentVersion`
  - `editCellSaveMakesANewCurrentVersionTheBridgeSees` (`fieldTableChanged` received, `bridge.isotopes_for` / `table_copy` show it)
  - `duplicateIsotopeDisablesSave` (reason text contains "duplicate")
  - `removingARowBelowTheFitMinimumDisablesSave`
  - `conflictReloadDiscardsEdits`, `conflictSaveAnywayWrites`, `conflictCancelKeepsEdits` (another version saved through `FieldTableStore` between open and save; chooser injected)
  - `unsavedPromptOnSwitchingVersionAndOnClose` (Cancel keeps window and edits; Discard switches)
  - `saveAndMakeCurrentDisabledWhileHeld` (`spec->hold_table("queue")`; reasons name "queue"; enabled again after release and `check_now()`)
  - `makeCurrentDisabledOnVersionWithError` (hand-written bad version), `makeCurrentConfirmsAndMovesPointer`, `makeCurrentRefusedWhenCurrentMovedMeanwhile` (version saved through `FieldTableStore` after the list was read; `current` unchanged by the refused call)
  - `undoAndRedoShortcutsEditTheGrid` (Ctrl+Z through `QTest::keyClick` restores the cell; Ctrl+Shift+Z reapplies it)
  - `aTableChangedWhileEditingShowsTheBannerAndKeepsEdits`
  - `commentsLostIsAskedOnce`
  - `unparseableVersionIsReadOnly`
  - `withoutBridgeSavesThroughTheStore` (bridge null)
- [ ] **Step 2: Run** `cmake --build --preset dev-ui --target pychron_ui_test_field_table_editor && build/dev-ui/tests/ui/pychron_ui_test_field_table_editor`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same. Expected: PASS.
- [ ] **Step 5: Commit** `feat(ui): field table editor window`.

### Task 14: Fit plot

**Files:**
- Create: `apps/pychron-ui/src/field_table_plot.hpp`, `field_table_plot.cpp`; CMake source list
- Modify: `field_table_editor_window.cpp` (centre-bottom, follows the selected detector column)
- Test: `tests/ui/test_field_table_plot.cpp`

**Interfaces:**
- Produces:
  ```cpp
  class FieldTablePlot : public QWidget {
   public:
    explicit FieldTablePlot(QWidget* parent = nullptr);   // connects PlotStyleHub::instance().changed()
    void show_detector(const spectrometer::FieldTable& table, const std::string& detector);
    // For tests: what the last redraw drew.
    int point_count() const; int curve_sample_count() const;
    std::pair<double, double> curve_mass_range() const;
    QStringList point_tooltips() const;                   // "Ar40: value 5.00100, residual 0.00012"
  };
  ```
  Curve: 200 samples over `[min mass − FieldTable::kInverseMargin, max mass + kInverseMargin]`; none for Discrete or a failed fit (points only). Colours from `plot_series_color(0)` and `plot_style()`.

- [ ] **Step 1: Write failing tests**: `pointsAndCurveCoverTheMarginedRange`, `discreteDrawsPointsOnly`, `failedFitDrawsPointsOnly`, `tooltipsCarryResiduals`, `restylesOnPlotStyleChange` (emit hub `changed()`, colour of the point graph equals the new `plot_series_color(0)`).
- [ ] **Step 2: Run** `cmake --build --preset dev-ui --target pychron_ui_test_field_table_plot && build/dev-ui/tests/ui/pychron_ui_test_field_table_plot`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same, plus `pychron_ui_test_field_table_editor`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(ui): field table fit plot`.

### Task 15: Add Row, Set Value, Compare, Import CSV dialogs

**Files:**
- Create: `apps/pychron-ui/src/field_table_dialogs.hpp`, `field_table_dialogs.cpp` (Add Row, Set Value, Compare); CMake source list
- Modify: `field_table_editor_window.hpp/.cpp` (toolbar actions, `set_pick_csv` injection)
- Test: `tests/ui/test_field_table_editor.cpp`

**Interfaces:**
- Consumes: `diff_field_tables` (Task 3), `read_mftable_csv` (Task 4), `FieldTableModel` (Task 12).
- Produces:
  - `class FieldTableCompareDialog : public QDialog { FieldTableCompareDialog(const TableDiff& diff, QString old_version, QString new_version, QWidget* parent); int row_count() const; }` — fit changes above, grid columns `Isotope, Detector, Old, New, Δ`, added/removed rows marked.
  - `FieldTableEditorWindow::compare(const QString& v1, const QString& v2)` (v2 empty: open edit copy vs current); unparseable side compared as text (two `QPlainTextEdit`s).
  - `FieldTableEditorWindow::set_pick_csv(std::function<std::optional<std::filesystem::path>()>)`, `import_csv()`: result opens as unsaved edit of the selected table, base = its current; a read error goes to `show_error`.
  - Set Value… dialog: detector, isotope, value, Propagate (default config `[magnet] propagate`) → `FieldTableModel::set_value`. Add Row: editable combo of `MolecularWeights` names.

- [ ] **Step 1: Write failing tests**: `compareTwoVersionsByIsotope` (row count = changed cells + added/removed), `compareOpenEditsAgainstCurrent`, `compareUnparseableAsText`, `importCsvOpensAsUnsavedEdit` (melbourne fixture; `modified()` true, `base_version()` = current, H2 column header marked not in config), `importCsvErrorIsShown`, `setValueDialogDefaultsPropagateFromConfig`, `addRowFillsMass`.
- [ ] **Step 2: Run** `pychron_ui_test_field_table_editor` after building. Expected: new tests FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same. Expected: PASS.
- [ ] **Step 5: Commit** `feat(ui): compare, import and set value in the field table editor`.

### Task 16: Menus, user doc, full run

**Files:**
- Modify: `apps/pychron-ui/src/main_window.hpp/.cpp` (owns the one editor window; `Spectrometer > Field Table…`), `spectrometer_window.hpp/.cpp` (same menu item, calls a `std::function<void()>` set by `MainWindow`), `docs/user/03-spectrometer.md` (new "Field table editor" and "elctl spec table" sections)
- Test: `tests/ui/test_spectrometer_window.cpp`, `tests/ui/test_menu_hub.cpp` or a main window test that exists for menus

**Interfaces:**
- Consumes: Task 13 constructor. `MainWindow` builds it with `spectrometer_`, `spectrometer_->data_root()`, `config()`, `weights()`, `[this]{ return bridge_.wall_now(); }`; second open raises the existing window. With no spectrometer or empty data root, both menu items disabled with tooltip `"no spectrometer configured"`.

- [ ] **Step 1: Write failing tests**: `fieldTableMenuOpensOneEditor` (trigger twice → same window, raised), `fieldTableMenuDisabledWithoutSpectrometer` (tooltip text), `spectrometerWindowMenuOpensTheSameEditor`.
- [ ] **Step 2: Run** the two UI test binaries. Expected: FAIL.
- [ ] **Step 3: Implement**, then write the user doc sections.
- [ ] **Step 4: Run** the two binaries. Expected: PASS.
- [ ] **Step 5: Full checks**, all on the rebased branch:
  - `python3 tools/quality_check.py` → exit 0
  - `python3 tools/compiler_sweep.py --build-dir build/dev-ui` (whole tree) → no warnings
  - `cmake --build --preset dev-ui` then `ctest --preset dev-ui` → all pass (covers `dev`)
  - `build/dev/tests/integration/pychron_outage_tests` (Task 8 touched executor; libs/experiment AGENTS.md) → pass
- [ ] **Step 6: Commit** `feat(ui): field table editor in the spectrometer menus`.

## Open questions

None.
