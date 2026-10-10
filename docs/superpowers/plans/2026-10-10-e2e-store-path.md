# End-to-end store path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A fresh instrument install runs its shipped queue of blanks and airs on simulated hardware, each run is written to the install's database, and the data window shows it when it finishes.

**Architecture:** A new adapter library beside `libs/experiment` turns a finished `AnalysisRecord` into the store's ingest items and sends them through `IStore::ingest`; `libs/experiment` keeps seeing only `IAnalysisPersister`. The existing spool and `SavePipeline` are unchanged, so a failed save stays in the spool and the queue goes on. `elctl exp run` and `pychron-ui` open the install's database, hand the session a store persister, and the UI refreshes the data window on `RunFinished`.

**Tech Stack:** C++20, GoogleTest, TinyORM on QtSql (SQLite always, PostgreSQL when `PYCHRON_TEST_PG_URL` is set), Qt6 Widgets for the UI task, TOML profiles.

**Spec:** `docs/superpowers/specs/2026-10-10-e2e-sim-experiment-design.md`. This is plan 1 of 3 (spec section 13): sections 5 (without the step rule of 5.4), 7.1, 7.3 (`sim.toml` only), 8, and a shipped queue of rows 1 to 5. Simulated samples are plan 2; the example irradiation, the step rule and the unknowns are plan 3.

## Global Constraints

- `libs/experiment` must not depend on `libs/persistence`. The store is reached only through `persist::IAnalysisPersister`.
- Qt must not appear in the public headers of the new library.
- The conversion of a record to ingest items is pure: no clock, no random id, no store read. Every id is `Uuid::v5(<record uuid>, <label>)` with the labels of Task 3; they are a file format.
- A record is spooled before the store is tried and leaves the spool only after the store has acknowledged it. A save that fails never stops the queue.
- The persister never creates an identifier, sample, project, mass spectrometer or extract device.
- No migration. If any task finds it needs one, stop and ask the owner (AGENTS.md, "Before adding a migration").
- Both engines run every statement; a new statement gets a `SchemaIndexes` plan test.
- Errors are values (`pychron::Result<T>`); no exceptions in `libs/`.
- Simulated-time tests derive from `VirtualTimeTest` and follow AGENTS.md "Time"; real-time bounds are upper bounds of 5 s or more.
- After changing C++ and before running tests: `python3 tools/quality_check.py`. Status 1 means fix the code; status 2 means the check did not run.
- Without a database everything behaves as today (Windows CI builds without persistence).
- Commit messages: Conventional Commits with the component as scope, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Build directory in commands below: `build/dev` (`cmake --preset dev`; the UI task uses `build/dev-ui`).

## What reading the code settled (spec section 14)

These are decisions; the tasks rely on them and Task 10 writes them into the spec.

1. **Ids.** `persistence::Uuid::v5(ns, name)` exists (`libs/persistence/include/pychron/persistence/ids.hpp`). Namespace is the record's uuid.
2. **Time zero.** `Identity::timestamp` is the wall time taken when the run is prepared (`libs/experiment/src/run/run.cpp:184`), not the origin of `Data::time_zero`. `AnalysisIngest::time_zero` is left unset.
3. **Tray and queue.** The record carries no tray. `Identity::queue_uuid` holds the queue's *name* (`run.cpp:457`), not a uuid: no `QueueRow` is written; the name goes into `AnalysisMetaRow::queue_names_json` as a one-element JSON array.
4. **Scripts.** `ScriptRef::text_ref` is never set and no text is in the record. Only `ScriptBodies::measurement` is written, from `effective_plan_toml`.
5. **Client.** `register_client` is ensure-by-(hostname, role). The acquisition client is `{<HOSTNAME env or "localhost">, "acquisition", nullopt, <program version>}`; no file is kept in the install.
6. **SQLite busy timeout.** Already 10 s (`libs/persistence/src/tiny/db.cpp:88`), with WAL. The two-connection test still has to pass.
7. **Index.** `analysis` has `UNIQUE (identifier_uuid, aliquot, increment)`: `last_analysis` needs no migration.
8. **`--sim-speed`.** `apps/pychron-ui/src/command_line.cpp:76` refuses `--sim-speed` without `--sim` before the install is known. The check moves to `main.cpp`, after the install is chosen.
9. **Fit text.** The store reads a fit with `reduction::parse_fit_kind(lower(fit))`, error type `"sd"` else SEM, and outliers from JSON keys `filter_outliers`, `iterations`, `std_devs` (`store_source.cpp:39-55`). The conversion writes `to_string(FitKind)`, `"SD"`/`"SEM"`, and that JSON.
10. **Test location.** The top-level `CMakeLists.txt` adds one test folder per library. The new tests live in `tests/experiment/` as a second executable, `pychron_experiment_store_tests`, the way `tests/processing` builds `pychron_processing_store_tests`; not in `tests/experiment_store/` as the spec says.

Not stored by this plan (spec 5.2's closing rule): sample/project/material/irradiation text, `time_zero`, tray, load, queue row, extraction/post-equilibration/post-measurement script texts, peak centers, events and notes, `pid_params`, extraction `series`, `snapshot_refs`, `cryo_measured`, `manometer_pressure`, `data.counts`, `spectrometer.state_hash` and `field_table_version`, the hook reference, plan overrides.

## Review Focus

1. A record whose fit failed (an intercept that is NaN): the run must still be saved, with the value stored as unknown, not refused and not written as NaN. Test in Task 3.
2. A draft record in the spool (`RecordBuilder::finalize` failed, so `provenance.sha` is empty or a required field is missing): conversion returns an error, the record stays in the spool, nothing crashes, and the next queue still starts. Test in Task 4.
3. A run that measured nothing usable (no series: aborted in measurement, truncated early): saved as an analysis with no blobs, `signals_state = complete`. Test in Task 3 and Task 4.
4. The install's database has a schema behind the build (opened with `migrate = false`): the queue start is refused with a message that names `elctl db migrate --db <url>`; the UI still starts. Tests in Task 7 and Task 9.
5. A queue whose identifier differs from the catalog's only by case or spaces (`BU`, `bu `): preflight names the identifier exactly as written in the queue, so the operator can see the difference. Test in Task 4.

---

### Task 1: `IStore::last_analysis`

**Files:**
- Modify: `libs/persistence/include/pychron/persistence/store.hpp` (beside `find_analysis`, about line 612)
- Modify: `libs/persistence/src/sql/statements.hpp`, `libs/persistence/src/store.cpp`
- Modify: every other `IStore` implementation and fake (`grep -rn "find_analysis(" --include=*.hpp --include=*.cpp libs tests apps`)
- Test: `tests/persistence/test_catalog.cpp` (or the file that tests `find_analysis`), `tests/persistence/test_schema.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct LastAnalysis { int aliquot = 0; int increment = -1; };
  // The highest aliquot among the analyses of `identifier`, and the highest
  // increment within it; nullopt when it has none or the identifier is unknown.
  virtual Result<std::optional<LastAnalysis>> last_analysis(const std::string& identifier) = 0;
  ```

- [ ] **Step 1: Write the failing tests.** In the engines-parameterised store fixture (`tests/persistence/store_fixture.hpp`, `seed_lab` gives identifier `66573`):
  - `LastAnalysisIsNulloptForAnIdentifierWithNone`: `last_analysis("66573")` is `nullopt`; `last_analysis("nope")` is `nullopt`.
  - `LastAnalysisIsTheHighestAliquotAndItsHighestIncrement`: ingest `66573` aliquot 1 increment -1, aliquot 2 increments 0 and 1, aliquot 3 increment 0 (use `analysis_item`, setting `increment`); expect `{3, 0}`.
  In `test_schema.cpp`, `SchemaIndexes.TheLastAnalysisOfAnIdentifierIsASeek`: `EXPLAIN QUERY PLAN` of the statement with a real identifier; the plan text names the unique index of `analysis (identifier_uuid, aliquot, increment)` and contains no `SCAN`. Follow `AnAnalysissReferencesAreFoundByScope` for how the plan is read.
- [ ] **Step 2: Run them; expect a compile failure** (`last_analysis` is not a member). `cmake --build build/dev --target pychron_persistence_tests`
- [ ] **Step 3: Implement.** One statement, standard SQL for both engines:
  `SELECT a.aliquot, a.increment FROM analysis a WHERE a.identifier_uuid = (SELECT uuid FROM identifier WHERE identifier = ?) ORDER BY a.aliquot DESC, a.increment DESC LIMIT 1`.
  If the plan test shows a scan or a temp B-tree, stop and report rather than add an index.
- [ ] **Step 4: Run.** `ctest --test-dir build/dev -R "LastAnalysis|SchemaIndexes" --output-on-failure`. Expected: PASS. With `PYCHRON_TEST_PG_URL` set, run again.
- [ ] **Step 5: `python3 tools/quality_check.py`, then commit.** `feat(persistence): the last analysis of an identifier`

---

### Task 2: The session takes its persister, and a queue is checked against it

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/persist/persister.hpp`, `libs/experiment/src/persist/persister.cpp`
- Modify: `libs/experiment/include/pychron/experiment/lab/session.hpp`, `libs/experiment/src/lab/session.cpp`
- Modify: test fakes that implement `IAnalysisPersister` (`tests/experiment/test_persist.cpp` `FlakyPersister`, `tests/experiment/test_executor.cpp` `StampingPersister`, `tests/experiment/run_fakes.hpp` if it has one)
- Test: `tests/experiment/test_persist.cpp`, `tests/integration/test_lab_session.cpp`

**Interfaces:**
- Produces, on `IAnalysisPersister`:
  ```cpp
  // What would stop this queue's runs from being saved, one line each;
  // empty when nothing would.
  virtual Result<std::vector<std::string>> check_queue(const QueueSpec& queue) = 0;
  ```
  `FilePersister::check_queue` returns an empty vector.
- Produces, on `lab::SessionOptions`:
  ```cpp
  persist::IAnalysisPersister* persister = nullptr;  // not owned; nullptr: files under <data>/records
  ```
- `LabSession::start` returns a `Config` error whose text is `the queue cannot be saved:` followed by the lines, one per line, when `check_queue` is non-empty, and `the queue cannot be saved: <error text>` when `check_queue` itself fails.

- [ ] **Step 1: Write the failing tests.**
  - `test_persist.cpp`, `FilePersisterChecksNothing`: `check_queue` of any queue is empty.
  - `test_lab_session.cpp`, `AQueueThePersisterRefusesIsNotStarted`: a fake persister whose `check_queue` returns `{"identifier 'zz' is not in the database"}`; `session.start(queue)` fails, the error text contains that line, no `QueueStarted` was published, `session.running()` is false.
  - `test_lab_session.cpp`, `RunsAreSavedThroughTheGivenPersister`: a fake persister that records `save_analysis` calls, given in `SessionOptions::persister`; run the example queue; the fake saw three records, `<data>/records/bu/bu-1.json` does not exist, `<data>/spool` is empty, `pending_saves() == 0`.
- [ ] **Step 2: Run; expect compile failure** on `check_queue` and `persister`.
- [ ] **Step 3: Implement.** In `LabSession::Services`, `save`, `aliquots` and `ctx.services.persister` use the given persister when there is one, else the `FilePersister` it still builds. In `LabSession::start`, call `check_queue` immediately after `check(queue)` and before the laser checks. The spool stays `<data>/spool`.
- [ ] **Step 4: Run.** `ctest --test-dir build/dev -R "FilePersister|LabSessionTest" --output-on-failure`. Expected: PASS, including the existing session tests unchanged.
- [ ] **Step 5: Quality check, commit.** `feat(experiment): a session saves through the persister it is given and checks a queue against it`

---

### Task 3: The adapter library and the conversion

**Files:**
- Create: `libs/experiment/adapters/store/include/pychron/experiment/store/record_ingest.hpp`
- Create: `libs/experiment/adapters/store/src/record_ingest.cpp`
- Modify: `libs/experiment/CMakeLists.txt` (target `pychron_experiment_store`, alias `pychron::experiment_store`, inside `if(TARGET pychron_persistence)`, modelled on `pychron_processing_store` in `libs/processing/CMakeLists.txt:34-45`; links `pychron::experiment pychron::persistence` PUBLIC, `nlohmann_json::nlohmann_json` PRIVATE; compile definition `PYCHRON_EXPERIMENT_HAS_STORE=1` PUBLIC)
- Modify: `tests/experiment/CMakeLists.txt` (executable `pychron_experiment_store_tests`, only `if(TARGET pychron_experiment_store)`, modelled on `tests/processing/CMakeLists.txt:20-30`; exclude its sources from the main experiment test glob)
- Test: `tests/experiment/test_record_ingest.cpp`

**Interfaces:**
- Produces, in namespace `pychron::experiment::store`:
  ```cpp
  struct RunIngest {
    std::vector<persistence::IngestItem> blobs;  // one per distinct raw series, in first-use order
    persistence::IngestItem analysis;
  };
  Result<RunIngest> to_ingest(const record::AnalysisRecord& record, persistence::Uuid client);
  // "" -> -1, "A" -> 0, "Z" -> 25, "AA" -> 26; anything else nullopt.
  std::optional<int> increment_of(std::string_view step);
  ```

**Mapping (the decisions; spec 5.2 and "What reading the code settled"):**

| Target | Source |
|---|---|
| `analysis` | `Uuid::parse(record.identity.uuid)`; unparseable is a `Config` error |
| `changeset` | `Uuid::v5(analysis, "changeset")` |
| `roots.signals`, `.intercepts`, `.baselines`, `.blanks`, `.icfactors`, `.tags` | `Uuid::v5(analysis, "signals")`, `"intercepts"`, `"baselines"`, `"blanks"`, `"icfactors"`, `"tags"` |
| analysis `IngestItem::item` | `Uuid::v5(analysis, "item")` |
| blob `IngestItem::item` | `Uuid::v5(analysis, "blob:" + <sha hex>)` |
| analysis `payload_sha256`, `record_sha256` | `record.provenance.sha` (hex to digest); empty or not 64 hex chars is a `Config` error ("the record was not finalized") |
| blob `payload_sha256` | the blob's own sha |
| `IngestItem::client` | the `client` argument |
| `created`, `timestamp` | `UtcTime::parse(record.identity.timestamp)`; unparseable is a `Config` error |
| `time_zero` | unset |
| `identifier`, `aliquot`, `analysis_type`, `run_index` | `record.identity` |
| `increment` | `increment_of(step)`; nullopt is a `Config` error naming the step |
| `mass_spectrometer`, `laboratory` (unset when empty), `analyst` | `record.instrument` |
| `extract_device` | unset when empty or `"(none)"` |
| `record_schema_version` | `record.provenance.schema_version` |
| `extraction.extract_value`, `extract_units`, `extract_duration`, `cleanup_duration`, `pattern`, `cryo_temperature` | `record.extraction.spec` (units and pattern unset when empty) |
| `extraction.beam_diameter` | `record.extraction.actuals.beam_diameter` when > 0 |
| `measured_positions` | one row per `spec.positions`, `position` set, nothing else |
| blob per `DataSeries` | `encode_tvs` + `kCodecTvs` when `trace.sigma` is non-empty, else `encode_tv` + `kCodecTv`; `n_points = t.size()`; identical (codec, bytes) is one item |
| `roots.signal_refs` | one per series: `series_kind = kind`, `series_key = (kind == "baseline") ? det : iso`, `detector = det`, `blob_sha`, `n_points` |
| `isotopes` | one per distinct (iso, det) among series whose `kind == "signal"`, sorted by isotope |
| `detectors` | one per distinct detector among all series, sorted; `deflection`, `gain_used` from `record.spectrometer.deflections`/`gains` when present |
| `roots.intercepts_rows` | one per `results.intercepts` entry: `isotope`, `detector` (the signal series' detector for that isotope), `value`, `error`, `n = intercept.n_used`, `fit = to_string(fit.kind)`, `error_type = "SD"` or `"SEM"`, `filter_outliers_json = {"filter_outliers":0|1,"iterations":N,"std_devs":X}` |
| `roots.baselines_rows` | one per `results.baselines` entry, same fit fields |
| a value or error that is not finite | stored as `nullopt` (unknown) |
| `roots.blanks_rows` | empty |
| `roots.icfactors_rows` | one per `results.icfactors` entry: `detector`, `value`, `error = 0` |
| `roots.tag` | `{"ok", nullopt, nullopt}` |
| `scripts.measurement` | `record.measurement.plan.effective_plan_toml` when non-empty; the other four unset |
| `spectrometer_snapshot` | `gains_json`, `deflections_json` from the maps; `settings_json` from `source_params` plus `integration_time`; `spectrometer_json = "{}"`; nlohmann with sorted keys (its default `std::map` object) |
| `meta` | `conditionals_json` (installed), `tripped_conditional_json`, `whiff_result_json` (when non-empty), `grain_polygons_json`, `pipette_counts_json` (when > 0), `software_json` (`software_version`, `software_git_sha`), `queue_names_json` (`["<queue name>"]` when non-empty); each unset when the record has nothing for it; `meta` itself unset when all are |
| `queue`, `load_name`, `peak_centers`, `monitor_checks`, `artifacts` | unset / empty |

- [ ] **Step 1: Write the failing tests** in `test_record_ingest.cpp`, with a helper `record::AnalysisRecord sample_record()` built through `record::RecordBuilder` (two signal series, Ar40 on H1 and Ar36 on CDD with sigma, one baseline on H1; intercepts and baselines for them; a canonical uuid; `finalize()` called so `provenance.sha` is set):
  - `TheSameRecordConvertsToTheSameItems`: `to_ingest(r, c)` twice; analysis bodies, every id and every blob's bytes are equal.
  - `TwoRecordsShareNoId`: change only the uuid; the analysis, changeset, six roots and item ids all differ.
  - `IdsAreTheDocumentedRecipe`: `changeset == Uuid::v5(analysis, "changeset")`, `roots.signals == Uuid::v5(analysis, "signals")`, item id `== Uuid::v5(analysis, "item")`.
  - `StepsBecomeIncrements`: `increment_of("") == -1`, `"A" == 0`, `"Z" == 25`, `"AA" == 26`; `"a"`, `"A1"`, `"-"` are nullopt; a record with step `"A1"` fails to convert and the error names `A1`.
  - `SeriesTakeTheirCodec`: the sigma series is `kCodecTvs`, the others `kCodecTv`; `decode_tv`/`decode_tvs` of the bytes give back the trace.
  - `IdenticalSeriesAreOneBlob`: two series with the same trace and no sigma give one blob item and two signal refs with one sha.
  - `ABaselineIsKeyedByDetector`: its `series_key` is `"H1"`.
  - `IdentityInstrumentAndExtractionAreCarried`: a row per field of the table above.
  - `AnEmptyExtractDeviceIsNone`, and `"(none)"` likewise.
  - `FitsAreWrittenAsTheStoreReadsThem`: a parabolic fit with SD error and outliers on (2 iterations, 2.5 sd) gives `fit == "parabolic"` (whatever `to_string(FitKind::Parabolic)` is), `error_type == "SD"`, and JSON that parses to those three numbers.
  - `ANonFiniteInterceptIsUnknown` (Review Focus 1): value NaN gives `value == nullopt`; conversion succeeds.
  - `ARecordWithNoSeriesConverts` (Review Focus 3): no blobs, no signal refs, no isotopes; conversion succeeds.
  - `ADraftRecordIsRefused`: empty `provenance.sha` is an error containing "not finalized".
  - `AUuidThatDoesNotParseIsRefused`, `ATimestampThatDoesNotParseIsRefused`.
  - `TheSampleTextIsNotCarried`: changing `record.sample` changes nothing in the analysis body except `record_sha256`/`payload_sha256` (rebuild both records through the builder).
- [ ] **Step 2: Run; expect compile failure** (no header). `cmake --build build/dev --target pychron_experiment_store_tests`
- [ ] **Step 3: Implement `to_ingest` and `increment_of`** per the table.
- [ ] **Step 4: Run.** `ctest --test-dir build/dev -R RecordIngest --output-on-failure`. Expected: PASS. Also build with `-DPYCHRON_PERSISTENCE=OFF` in a scratch build directory and confirm the configure and build succeed without the new target.
- [ ] **Step 5: Quality check, commit.** `feat(experiment): turn a finished record into what the store ingests`

---

### Task 4: `StorePersister`

**Files:**
- Create: `libs/experiment/adapters/store/include/pychron/experiment/store/store_persister.hpp`
- Create: `libs/experiment/adapters/store/src/store_persister.cpp`
- Create: `tests/experiment/failing_store.hpp` (an `IStore` decorator for tests)
- Test: `tests/experiment/test_store_persister.cpp` (in `pychron_experiment_store_tests`; uses `tests/persistence/store_fixture.hpp` for the engines, as `tests/processing` does)

**Interfaces:**
- Consumes: `to_ingest` (Task 3), `IStore::last_analysis` (Task 1), `check_queue` (Task 2).
- Produces:
  ```cpp
  class StorePersister final : public persist::IAnalysisPersister {
   public:
    // `store` must outlive the persister. `data` is the session's data folder:
    // the spool is <data>/spool, partial records and artifacts go under <data>/records.
    StorePersister(persistence::IStore& store, persistence::Uuid client, std::filesystem::path data);
    Result<int> next_aliquot(const std::string& identifier) override;
    Result<void> begin_run(const RunIdentity& id, const QueueSpec& queue) override;
    Result<void> save_extraction(const record::AnalysisRecord& record) override;
    Result<void> save_analysis(const record::AnalysisRecord& record) override;
    Result<void> save_artifact(const std::string& uuid, const std::string& name,
                               const std::vector<std::uint8_t>& bytes) override;
    Result<void> flush() override;
    Result<std::vector<std::string>> check_queue(const QueueSpec& queue) override;
  };

  // The store of an acquisition program and its persister. Opens with
  // migrate = false and registers the acquisition client.
  struct Acquisition {
    std::unique_ptr<persistence::IStore> store;
    std::unique_ptr<StorePersister> persister;  // refers to *store
  };
  Result<Acquisition> open_acquisition(const std::string& url, std::filesystem::path data,
                                       const std::string& software_version);
  ```

**Behaviour (the decisions):**
- `save_analysis`: `to_ingest`; `store.ingest` each blob, then the analysis. A `duplicate` acknowledgement is success. The first error is returned, with the text prefixed for a catalog refusal (`is_unknown_catalog_reference(error)`): `not in the database: <store's text>. Add it (elctl entry ...) and start a queue to send the run again`.
- `next_aliquot(identifier)`: `1 + max(store, spool, issued)` where `store` is `last_analysis(identifier)->aliquot` (0 when nullopt), `spool` is the highest aliquot among records in `<data>/spool` whose identifier matches (read with `persist::Spool::pending()`/`get()`), and `issued` is this persister's own map. If `last_analysis` fails, use the value cached from the last successful read for that identifier; with none cached, return the error.
- `begin_run`, `save_extraction`, `save_artifact`: delegate to a `persist::FilePersister(data / "records")` member.
- `flush`: success.
- `check_queue`, lines in this order and wording, each at most once:
  - `the database cannot be reached: <text>` (and nothing else) when the first read fails;
  - `mass spectrometer '<name>' is not in the database` (`find_catalog_row` on the mass spectrometer table; an empty name is `the queue names no mass spectrometer`);
  - `extract device '<name>' is not in the database` for the queue's device and each run's own, skipping empty;
  - `identifier '<text>' is not in the database` for each distinct identifier of a run that is not skipped, in queue order, via `find_identifier`, the text exactly as the queue has it;
  - `<runid> is already in the database` for each run with a fixed aliquot whose (identifier, aliquot, increment) `find_analysis` finds.
- `open_acquisition`: `persistence::open_store({url, false})`; on failure, when the error is the outdated-schema refusal, append `\nRun: elctl db migrate --db <url>` with any password removed from the url; `register_client({env HOSTNAME or "localhost", "acquisition", nullopt, software_version})`.
- Thread use: one thread at a time (the queue's). The cache and the issued map are guarded by a `std::mutex` held only around map access, never across a store call.

- [ ] **Step 1: Write `failing_store.hpp`**: `class FailingStore final : public persistence::IStore` holding an `IStore&`, forwarding everything, with `fail_ingest(int times)`, `fail_reads(bool)`, and `fail_after_commit(int times)` (calls the real `ingest`, then returns an `Io` error). Keep it header-only.
- [ ] **Step 2: Write the failing tests**, engines-parameterised (`seed_lab` gives mass spectrometer `jan`, identifiers `66573`, `66574`; records come from Task 3's `sample_record()` moved into a shared `tests/experiment/store_records.hpp`, with identifier, aliquot and mass spectrometer settable):
  - `ASavedRecordIsAnAnalysisWithItsSignals`: after `save_analysis`, `find_analysis("66573", 1, -1)` is set, `load_analysis` shows heads for signals, intercepts, baselines and tags, and `find_analyses({.identifier = "66573"})[0].signals_state == "complete"`.
  - `SavingTwiceIsOneAnalysis`: second `save_analysis` succeeds; one analysis.
  - `ARecordWithNoSeriesIsComplete` (Review Focus 3).
  - `AnUnknownIdentifierIsRefusedAndSavesOnceItExists`: identifier `77000` fails, the error text contains `not in the database` and `77000`, nothing was written; after `add_identifier`, the same record saves.
  - The same for an unknown mass spectrometer and an unknown extract device.
  - `AliquotsFollowTheStore`: store has `66573` aliquots 1 and 2; a new persister gives 3, then 4.
  - `AliquotsCountARecordWaitingInTheSpool`: put a record for `66573` aliquot 5 in `<data>/spool` with `persist::Spool::put`; `next_aliquot` gives 6.
  - `AliquotsSurviveAnUnreadableStore`: one successful `next_aliquot` (1), then `fail_reads(true)`: the next is 2. A fresh persister with `fail_reads(true)` from the start returns an error.
  - `CheckQueueNamesExactlyWhatIsMissing`: a queue with mass spectrometer `nope`, device `laser9`, identifiers `66573`, `BU`, `BU`, `66574`: three lines, in the order above, `identifier 'BU'` once (Review Focus 5); a queue of `66573` on `jan` gives none.
  - `CheckQueueNamesATakenRunId`: store has `66573-01`; a row fixing aliquot 1 gives `66573-01 is already in the database`.
  - `CheckQueueSaysWhenTheDatabaseCannotBeReached`: `fail_reads(true)`: one line starting `the database cannot be reached:`.
  - With `persist::Spool` and `persist::SavePipeline` over the persister:
    - `AFailedSaveStaysInTheSpoolAndIsSentLater`: `fail_ingest(10)`; `pipeline.save(record)` succeeds, `pending() == 1`, `last_error()` set; clear the failure; `recover()` returns 1; `pending() == 0`; one analysis.
    - `ACrashAfterTheStoresCommitLeavesOneAnalysis`: `fail_after_commit(n)` for every item of one save; `pending() == 1`; `recover()`; one analysis, `pending() == 0`.
    - `ADraftRecordStaysInTheSpool` (Review Focus 2): a record from `RecordBuilder::draft()`; `pending() == 1` after `save` and after `recover()`; no analysis; no crash.
  - `OpenAcquisitionRefusesASchemaThatIsBehind` (SQLite file made by opening with `migrate = false` on an empty file, or by the helper `SchemaTest.RefusesAnOutdatedSchemaWithoutMigrate` uses): the error text contains `elctl db migrate --db`.
  - `OpenAcquisitionIsOneClientPerHost`: two calls on one database ingest analyses stamped with the same `acquisition_client_uuid`.
  - `AReaderDoesNotLockOutTheWriter` (spec 8.5), on a file-backed SQLite and on PostgreSQL when set: a second store handle opened on the same url; one thread saves 50 records through the persister while another calls `browse` on the second handle in a loop until the writer is done; both finish without an error; 50 analyses. Real-time upper bound 30 s.
- [ ] **Step 3: Run; expect compile failure.**
- [ ] **Step 4: Implement** `StorePersister` and `open_acquisition`.
- [ ] **Step 5: Run.** `ctest --test-dir build/dev -R "StorePersister|OpenAcquisition" --output-on-failure`; again with `PYCHRON_TEST_PG_URL` set. Expected: PASS. Then build with `-DPYCHRON_SANITIZE=thread` in a scratch directory and run `AReaderDoesNotLockOutTheWriter`.
- [ ] **Step 6: Quality check, commit.** `feat(experiment): a persister that writes finished runs to the store`

---

### Task 5: The seed names the mass spectrometer and extract devices

**Files:**
- Modify: `libs/entry/include/pychron/entry/seed.hpp`, `libs/entry/src/seed.cpp`
- Modify: `profiles/instrument-common/seed.toml`
- Test: `tests/entry/test_seed.cpp` (the existing seed tests' file)

**Interfaces:**
- Produces: `Seed::mass_spectrometers`, `Seed::extract_devices` (`std::vector<std::string>`, file order); `SeedReport::mass_spectrometers`, `SeedReport::extract_devices` (ints), counted by `changed()`; `kept` entries `"mass spectrometer <name>"`, `"extract device <name>"`.
- `seed.toml` keys, both optional: `mass_spectrometers = ["argus"]`, `extract_devices = []`.

- [ ] **Step 1: Write the failing tests.**
  - `ParsesTheInstrumentNames`: both arrays read in order; absent keys give empty vectors.
  - `RefusesAnEmptyOrRepeatedInstrumentName`: `mass_spectrometers = [""]`, `["a", "a"]`, and the same for `extract_devices`: `Config` errors naming the key.
  - `SeedsTheInstrumentsOnce`: `apply_seed` on an empty store reports 1 mass spectrometer and 1 extract device; a second call reports 0 and lists both in `kept`; `find_catalog_row` finds both.
  - `KeepsAMassSpectrometerThatIsThere`: `add_mass_spectrometer({"argus", "argus", "j"})` first; after the seed its kind and code are unchanged.
  - `DryRunWritesNoInstrument`.
  - `DescribeCountsTheInstruments`: text contains `1 mass spectrometer` and `1 extract device`.
- [ ] **Step 2: Run; expect failure.**
- [ ] **Step 3: Implement.** Ensure mass spectrometers and extract devices after the project and before the reactors, with `add_mass_spectrometer(client, {name})` and `add_extract_device(client, name)`; "made" versus "kept" is decided by `find_catalog_row` before the add. Add to `profiles/instrument-common/seed.toml`, above `[[samples]]`:
  ```toml
  # The instruments runs are recorded under. A queue's mass_spectrometer and
  # extract_device must be named here (or added to the database some other way).
  mass_spectrometers = ["{{ family }}"]
  extract_devices = []
  ```
- [ ] **Step 4: Run.** `ctest --test-dir build/dev -R Seed --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Quality check, commit.** `feat(entry): the seed names the mass spectrometer and extract devices`

---

### Task 6: The shipped queue, `sim.toml`, and the test that holds them together

**Files:**
- Modify: `profiles/instrument-common/experiment.toml`
- Modify: `profiles/extraction-line-starter/profile.toml` (bump `version` to 4)
- Modify: `tests/setup/test_profiles.cpp`
- Possibly modify: `profiles/instrument-common/defaults.toml` (only if Step 1 finds a mismatch)

**Interfaces:**
- Produces: an install whose `experiment.toml` is `ba`, `a`, `a`, `a`, `ba` and whose folder has `sim.toml` beside `extraction_line.toml` when the line is the example line.

- [ ] **Step 1: Find out what runs.** In a scratch root: `PYCHRON_SITE_CONFIG=<scratch>/site.toml build/dev/apps/elctl/elctl init argus --root <scratch>/argus --yes`, write a five-run queue there by hand (as Step 3), and run `elctl --install argus --sim exp run <scratch>/argus/experiment.toml --sim-speed max`. Record in the task notes: whether `sim_blank` and `sim_air` run on the installed line with plan `multicollect`, the simulated duration of the queue, and (from the five record files) each run's Ar40 and Ar36 intercepts. Repeat for `helix` and `ngx`. If a profile cannot run it, stop and report which and why before going on.
- [ ] **Step 2: Write the failing test changes** in `test_profiles.cpp`:
  - In `ShipsASeedThatTheDefaultsAndTheExampleQueueAgreeWith`, for each instrument profile with default answers: every identifier of the shipped queue is a seed identifier; the queue's `mass_spectrometer` is in the seed's `mass_spectrometers`; the queue's `extract_device` (when set) and each run's device are in the seed's `extract_devices`; each run's extraction script exists as `scripts/extraction/<name>.py`, its plan as `plans/<name>.toml`, its post-measurement script as `scripts/post_measurement/<name>.py`; each run's `sample` table equals the seed's sample and material for that identifier with project `references`.
  - The shipped queue's identifiers are exactly `ba, a, a, a, ba`.
  - `sim.toml` is in the list of expected files, and its bytes equal `configs/examples/sim.toml`.
  - A profile applied with `line_source = import` (the test already has such a case, or add one with the example files as the imported ones) has no `sim.toml`.
- [ ] **Step 3: Run; expect failure. Then change the profile files.** `experiment.toml`:
  ```toml
  # Example queue (written by pychron setup):
  #   elctl --install {{ install_name }} exp run experiment.toml
  # Three air shots between two blanks.

  [queue]
  name = "first-queue"
  mass_spectrometer = "{{ family }}"
  username = "pychron"

  [queue.delays]
  before_analyses = 5
  between_analyses = 10
  after_blank = 10
  ```
  followed by five `[[runs]]`: `ba` (`script = "sim_blank"`, `sample = { sample = "blank_air", material = "blank", project = "references" }`), three `a` (`script = "sim_air"`, `sample = { sample = "air", material = "air", project = "references" }`), `ba`; each `extraction = { script = ..., duration = 30 }`, `measurement = { plan = "multicollect" }`, `post_measurement = "sim_pump"`. Add overrides to the measurement only if Step 1 showed the airs' 40/36 scatter is above 2 %; then use the override keys Step 1 found the installed `multicollect` to have, and write the measured scatter in the commit message.
  In `extraction-line-starter/profile.toml` add:
  ```toml
  [[files]]
  copy = "@examples/sim.toml"
  to = "sim.toml"
  when = "line_source == \"starter\""
  ```
- [ ] **Step 4: Run.** `ctest --test-dir build/dev -R Profiles --output-on-failure`, and the setup tests that list an install's files (`ctest -R "Setup|Installer|Doctor"`). Expected: PASS.
- [ ] **Step 5: Commit.** `feat(setup): an install ships a queue of airs and blanks its seed covers, and the example lab's sim.toml`

---

### Task 7: `elctl exp run` writes the store

**Files:**
- Modify: `apps/elctl/src/exp.cpp`, `apps/elctl/src/cli.cpp` (usage text, `--install` supplying the database)
- Create: `apps/elctl/src/exp_store.hpp`, `apps/elctl/src/exp_store.cpp`, `apps/elctl/src/exp_store_stub.cpp`
- Modify: `apps/elctl/CMakeLists.txt` (the stub pattern of lines 14-23: `exp_store.cpp` with persistence, `exp_store_stub.cpp` without; link `pychron::experiment_store`)
- Test: `apps/elctl/tests/test_exp_commands.cpp`
- Modify: `docs/user/08-elctl-reference.md`

**Interfaces:**
- Consumes: `open_acquisition` (Task 4), `SessionOptions::persister` (Task 2).
- Produces, in `exp_store.hpp`:
  ```cpp
  // The persister for --db / the install's database; the returned object owns
  // the store. Without persistence: a Config error "built without the DVC store
  // (PYCHRON_PERSISTENCE=OFF or no Qt Sql)".
  struct ExpStore {
    virtual ~ExpStore() = default;
    virtual experiment::persist::IAnalysisPersister& persister() = 0;
  };
  Result<std::unique_ptr<ExpStore>> open_exp_store(const std::string& url, const std::filesystem::path& data);
  ```
- CLI: `exp run ... [--db <url>]`. The database is `--db`, else the install's (`setup::database_url(install)`, as `seed_instrument_database` gets it), else none.

- [ ] **Step 1: Write the failing tests** (the file's existing harness runs `elctl` in-process with captured output; `--sim-speed max`):
  - `AnInstallsQueueGoesToItsDatabase`: `init argus --yes` into a temp root with `PYCHRON_SITE_CONFIG` there; `--install argus --sim exp run <root>/experiment.toml --sim-speed max`; exit 0; the last summary line is `5/5 run(s) succeeded; in <the database as the install names it>`; opening the database with `persistence::open_store` and `find_analyses({})` gives five, with run ids `ba-01`, `a-01`, `a-02`, `a-03`, `ba-02`; `<root>/data/records/ba/ba-1.json` does not exist; `<root>/data/spool` is empty. Under 60 s real.
  - `DbOverridesTheInstall`: `--db sqlite:<other file>` seeded with `elctl entry seed`; the runs are in the other file.
  - `AQueueTheDatabaseCannotTakeIsNotRun`: a queue with identifier `zz9`; exit non-zero; stderr contains `identifier 'zz9' is not in the database`; no `run 0` line; no analysis written.
  - `DryRunSaysWhatTheDatabaseLacks`: the same queue with `--dry-run` prints the same line.
  - `ADatabaseThatCannotBeOpenedIsFatal`: `--db sqlite:/nonexistent/dir/x.db`; exit non-zero before `queue` is printed; with a schema that is behind (Review Focus 4), stderr contains `elctl db migrate --db`.
  - `WithoutADatabaseRecordsAreWrittenAsBefore`: the existing example-queue test still passes unchanged, and its last line still says `records in`.
  - Guarded by `#ifndef PYCHRON_ELCTL_HAS_STORE`: `--db` exits non-zero with `built without the DVC store`.
- [ ] **Step 2: Run; expect failure.**
- [ ] **Step 3: Implement.** Open the store after the arguments are checked and before the line is loaded; pass `&store->persister()` in `SessionOptions`; the `ExpStore` is declared before the session so it outlives it. The summary line: with a store, `; in <url with any password removed>`, and `warning: K record(s) waiting in <data>/spool` when pending. `--dry-run` calls `check_queue` and prints its lines as `error: <line>`.
- [ ] **Step 4: Run.** `ctest --test-dir build/dev -R ElctlExp --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Quality check, commit.** `feat(elctl): exp run writes an install's runs to its database`

---

### Task 8: The end-to-end test

**Files:**
- Create: `tests/integration/test_install_e2e.cpp`
- Modify: `tests/integration/CMakeLists.txt` (its own executable `pychron_install_e2e_tests`, only `if(TARGET pychron_experiment_store AND TARGET pychron_processing_store AND TARGET pychron_entry)`; links those, `pychron::setup`, and what `test_lab_session.cpp` links)

**Interfaces:**
- Consumes: everything above; `setup` (`plan_install`/`apply_install`/`register_install`, as `apps/elctl/src/setup.cpp:201-360` calls them), `entry::parse_seed`/`apply_seed`, `processing::StoreSource::open`, `processing::analysis_from_record` (`libs/processing/adapters/records`).

- [ ] **Step 1: Write the test** `InstallE2E.AFreshInstallsQueueLandsInItsDatabase`, deriving from `VirtualTimeTest` (clock declared first; the test thread a `Clock::Participant`):
  1. Temp root; `PYCHRON_SITE_CONFIG` set into it for the test's duration; plan and apply profile `argus` with default answers; open the install's database with `migrate = true`; apply `<root>/seed.toml`.
  2. Bring the install's line and spectrometer up simulated on the `VirtualClock`, the way `test_lab_session.cpp` brings up the example lab but from the install's files; `open_acquisition(url, <root>/data, "test")`; wrap its persister in a test decorator that keeps a copy of every record passed to `save_analysis`; `LabSession` with that persister.
  3. Load `<root>/experiment.toml`, `start`, `wait`.
  Assertions:
  - the result is `Completed` with five `Success` runs; `pending_saves() == 0`;
  - a `StoreSource` opened on the same url with `migrate = false`: `browse` gives five rows, newest first, run ids `ba-02`, `a-03`, `a-02`, `a-01`, `ba-01`; each row's project is `references` and its sample the seed's for its identifier;
  - for each captured record: `source.load(uuid)` and `load_raw(uuid)` succeed; the loaded analysis's isotopes, each isotope's intercept value and error, each detector's baseline value and error, each fit, and each raw series' points equal those of `analysis_from_record(record)`; `signals_state` complete;
  - each air's Ar40 intercept is more than 100 times each blank's;
  - each air's Ar40/Ar36 is within 2 % of `sim::air_ratios` Ar40 (298.56), or the tolerance Task 6 Step 1 measured, stated in a comment with the measured scatter;
  - the test took under 30 s of real time.
  Second test, `RunningItAgainNumbersOn`: a second session on the same database runs the queue again; run ids `ba-03`, `a-04`, `a-05`, `a-06`, `ba-04` are added; ten analyses.
  Both tests are parameterised over the engines (`tests/persistence/store_fixture.hpp`), the PostgreSQL case replacing the install's database url with the test schema's.
- [ ] **Step 2: Run.** `ctest --test-dir build/dev -R InstallE2E --output-on-failure`. Expected: PASS (the pieces exist). If the comparison with `analysis_from_record` fails on a field, fix the conversion (Task 3) or the store source's read of it, and add the case to `test_record_ingest.cpp`; do not weaken the comparison.
- [ ] **Step 3: Measure the save.** Time `save_analysis` over the five runs (a `SteadyClock` reading around the decorated call, printed with `std::printf`), on SQLite and, when available, PostgreSQL. Put the per-run mean in the commit message.
- [ ] **Step 4: Run the whole suite with sanitizers.** A scratch build with `-DPYCHRON_SANITIZE=address,undefined`; `ctest -R "InstallE2E|StorePersister|RecordIngest|LabSessionTest"`.
- [ ] **Step 5: Commit.** `test(integration): a fresh install's queue lands in its database`

---

### Task 9: `pychron-ui` reads and writes the install's database

**Files:**
- Create: `apps/pychron-ui/src/database_choice.hpp`, `apps/pychron-ui/src/database_choice.cpp`
- Modify: `apps/pychron-ui/src/main.cpp` (about lines 170-200, 324-350, 439-460, 495-505), `apps/pychron-ui/src/command_line.cpp:76`, `apps/pychron-ui/CMakeLists.txt` (link `pychron::experiment_store` inside the `PYCHRON_UI_HAS_STORE` block)
- Modify: `apps/pychron-ui/src/main_window.hpp`/`.cpp` (a slot that refreshes the data window), `apps/pychron-ui/src/data_browser_window.hpp`/`.cpp` (an empty-reason line; refresh keeps selection), `apps/pychron-ui/src/executor_pane.cpp` (the pending text)
- Test: `tests/ui/test_database_choice.cpp`, `tests/ui/test_data_browser.cpp` (or the existing browser test file), `tests/ui/test_command_line.cpp`

**Interfaces:**
- Consumes: `open_acquisition`, `SessionOptions::persister`, `ExperimentBridge::runFinished` (`experiment_bridge.hpp:67`).
- Produces:
  ```cpp
  // --db when given; else the install's database with its password; else nullopt.
  // An install whose password cannot be read is an error.
  Result<std::optional<std::string>> choose_database(const CommandLine& cli, const setup::SiteInstall* install);
  ```
  `ExperimentBridge` gains `void queueStarted(const pychron::experiment::lab::QueueStarted&)` if it does not relay it already. `DataBrowserWindow::set_unavailable(const QString& reason)` shows `reason` in place of the list's empty text. `MainWindow::refresh_data()` calls the browser's `refresh()` when the data window exists.

**Behaviour (the decisions):**
- One url, from `choose_database`, is used for the data window's `StoreSource`, `EntryActions`, `FitActions`, `StoreIdentifierSource` and `open_acquisition`. Today the last three look at `cli->db` only.
- Instrument run (not `run_data_reduction`): when the url is set and the `StoreSource` opens, it is the data source; when it does not open, the program still starts, the data source is the records folder source left empty of a store, the browser shows `The database could not be opened: <text>` and the log dock gets an `ERROR [ui]` line with the same text. `--db` given explicitly and failing stays fatal (exit 2), as today.
- When `open_acquisition` fails the session gets a persister whose `check_queue` returns `the database cannot be reached: <text>` (a small class in `main.cpp`'s anonymous namespace, forwarding nothing else: `save_analysis` returns the same error, so nothing is lost if it is ever reached), so a queue cannot start and the operator is told why.
- The acquisition store and persister are declared before the session and destroyed after it.
- `bridge.runFinished` and `bridge.queueStarted` are connected to `MainWindow::refresh_data` (queued: the bridge already delivers on the UI thread).
- `DataBrowserWindow::refresh()` keeps the current row's uuid selected when it is still listed, and does not move the scroll position when the selection is unchanged.
- `--sim-speed` without `--sim` is no longer refused in `parse`; `main.cpp` refuses it after the install is chosen when neither `cli.sim` nor `install.simulation` is true, with the same message.
- The executor pane's pending text reads `N run(s) waiting to be saved to the database` when a store persister is in use, and is unchanged otherwise.

- [ ] **Step 1: Write the failing tests.**
  - `DatabaseChoice.DbWinsOverTheInstall`, `.TheInstallsDatabaseIsUsedWithoutDb`, `.NoneWithoutEither`, `.AnInstallWithNoDatabaseHasNone`.
  - `CommandLine.SimSpeedAloneParses` (replaces the test that expects the refusal, if there is one).
  - `DataBrowser.RefreshShowsARunThatJustLanded`: a `StoreSource` on a temp SQLite store seeded with `seed_lab`; window built (0 rows); ingest one analysis through a second store handle; call `refresh()`; the model has 1 row. Ingest another with one row selected and a facet filter set; after `refresh()` the same uuid is selected and the filter is unchanged.
  - `DataBrowser.SaysWhyItIsEmpty`: `set_unavailable("x")` shows the text.
  - `MainWindow.RefreshesDataWhenARunFinishes`: emit the bridge's `runFinished` signal; the browser's source generation was read again (use a counting `IAnalysisSource` fake).
- [ ] **Step 2: Run; expect failure.** `cmake --build build/dev-ui && ctest --test-dir build/dev-ui -R "DatabaseChoice|DataBrowser|CommandLine|MainWindow"`
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run the tests; expected PASS. Then check it by hand** (the real path no unit test drives): in a scratch root, `elctl init argus --yes`, `pychron-ui --install argus --sim-speed 20`, open the experiment window, open `experiment.toml`, start; open the data window and watch five rows arrive, one per run; open one in recall. Then rename the database file, start the program again: it starts, the data window says why it is empty, and starting the queue is refused with the same reason. Report what was seen.
- [ ] **Step 5: Quality check, commit.** `feat(ui): an instrument install reads and writes its database, and the data window shows each run as it ends`

---

### Task 10: Documents

**Files:**
- Modify: `docs/superpowers/specs/2026-10-10-e2e-sim-experiment-design.md` (compressed, per AGENTS.md)
- Modify: `docs/superpowers/specs/2026-09-29-experiment-system-design.md` (section 8), `2026-10-01-dvc-schema-design.md` (section 8.1), `2026-09-29-persistence-adr.md` (ADR-0002), `2026-10-07-install-defaults-design.md` (decision 2, seed keys), `docs/superpowers/plans/2026-10-02-dvc-persistence.md` (D3)
- Modify: `docs/user/04-experiments.md` ("What is written"), `docs/user/01-getting-started.md`, `README.md` ("Try it in simulation"), `docs/dev_setup.md` (sections 5 and 6), `docs/installation_runbook.md` (2.3, 2.4), `docs/entry.md` (the seed), `docs/user/08-elctl-reference.md` (if Task 7 left anything), `AGENTS.md`

- [ ] **Step 1: The spec.** Replace section 14 items 1 to 6 and 14 with what "What reading the code settled" says, and item 15 with the not-stored list; correct section 5.2 where the code differed (no `QueueRow`, `time_zero` unset, scripts), section 5.4's interface for this plan (`next_aliquot` stays until plan 3 brings `last_run`), section 7.1 (`extract_devices` shipped empty until plan 3), and the test paths of sections 10.3 and 10.4 (`tests/experiment/`). Set the status line to say plan 1 is implemented.
- [ ] **Step 2: The earlier specs.** One short note in each, as spec section 12 lists.
- [ ] **Step 3: The user documents.** State what is true after this plan and nothing more: an install's runs go to its database; without one, record files as before; what is and is not stored; the spool and what a pending run means; the quick start (`elctl init argus --yes`, `pychron-ui --install argus --sim-speed 20`, `elctl --install argus --sim exp run ... --sim-speed max`) with a queue of airs and blanks; that unknowns and ages come later.
- [ ] **Step 4: `AGENTS.md`**, compressed, under "Build and test": the adapter library and when it builds; the ids are a file format; the persister never makes catalog rows; `check_queue` refuses a queue the catalog cannot take; one acquisition program per database; the seed, the defaults, the shipped queue and `sim.toml` are held together by `tests/setup/test_profiles.cpp`.
- [ ] **Step 5: Check every command in the documents by running it** in a scratch root, then commit. `docs: runs go to an install's database; the quick start from a fresh install`

---

## Landing

Per AGENTS.md: rebase on `origin/develop`, `python3 tools/quality_check.py`, the full `ctest` on the rebased branch (and with `PYCHRON_TEST_PG_URL` set, since queries were added), merge into `develop`, push. No commit here needs a `BREAKING CHANGE:` footer: no migration is added.

---

## As built

Written by Task 10, after Tasks 1 to 9 landed on the branch. Where this
section and the tasks above disagree, this section is what the code does; the
spec has been corrected to match.

### Departures from the plan

**The store path**

- A store handle works only on the thread that opened it, and the queue's
  thread is not that thread. Task 4's `open_acquisition` therefore returns
  `Result<std::unique_ptr<store::Acquisition>>`, not a struct holding a store
  and a persister. `Acquisition::persister()` is the `IAnalysisPersister&` a
  session is given. The object owns one worker thread, which opens the store
  and runs every persister call while the caller waits. The worker is not a
  clock participant, so simulated time stands still for the length of a call.
  `open_acquisition` is called from the program's main thread, because the Qt
  application object the store needs is made there
  (`persistence::ensure_host_application()`, new). No call may be in progress
  or made once the `Acquisition` is being destroyed, so it is declared before
  the session. `StorePersister(IStore&, client, data)` remains as the direct,
  one-thread class. This is the one thread the plan added; the spec had said
  the design adds none.
- The conversion refuses more than Task 3 listed: series whose `t`, `v` and
  sigma differ in length, two series with the same kind and key (one isotope
  on two detectors, or two baseline series on one detector, since the store's
  keys hold one), and an intercept whose isotope has no series. Step letters
  fold case (`a` is `A`), where Task 3 had lower case refused. The queue's
  name is carried in `queue_names_json`.
- `next_aliquot` fails when the spool cannot be listed, and does not count a
  spooled record that cannot be read.
- `check_queue` names a taken run id once, and answers with the single line
  `the database cannot be reached: ...` when any read fails, not only the
  first.
- Blobs ingested before an analysis that the catalog then refuses stay in the
  store as unreferenced content-addressed rows. Nothing removes them; the
  re-send finds them there.
- `without_password(url)` is public in `store_persister.hpp`; both programs
  use it to show a database.

**What an install ships**

- The placeholder field tables that the profiles copied from
  `configs/examples/tables/` disagreed with the installed one-hop
  `multicollect` plan, so a fresh simulated install never measured Ar36. This
  was not in the spec or the plan. `profiles/qtegra` and `profiles/ngx` now
  ship their own placeholder tables, built so that every isotope of the
  multicollection sits on its detector (collectors one mass unit apart on the
  reference detector's own position-against-mass curve). The example tables
  are unchanged. `EveryInstalledPlanPutsItsIsotopesOnItsDetectors` in
  `tests/setup/test_profiles.cpp` holds each installed plan and the table
  together within 0.01 (the simulated peak is flat within 0.02 of its
  centre).
- The shipped queue runs `multicollect` with no overrides, at the plan's own
  cycles: 19 minutes 15 seconds of simulated time.
- Profile versions are now `qtegra` 2, `ngx` 2, `extraction-line-starter` 4
  and `instrument-common` 3.
- `extract_devices` ships empty; `co2` comes with plan 3.

**The programs**

- `elctl exp run`: an install's database whose password cannot be read is
  fatal (`error: the installation's database: ...`) rather than a fall back
  to record files; `--db` overrides it.
- `pychron-ui`: one database for the whole program, so
  `pychron-ui --sim --db <url>` with no install now also saves runs to that
  database, where before `--db` only browsed. When an instrument install's
  database cannot be opened the session is given a persister that refuses
  every queue with the reason (`NoDatabasePersister`), the data window shows
  a banner naming the records folder it shows instead, and the log has an
  error line.
- The data window refreshes when a run ends and when a queue starts, but only
  if the store changed; hidden, it refreshes when next shown; it keeps the
  rows loaded, the selection, the scroll position and the column widths.

### Measured

On a fresh argus install with the shipped queue (`pychron_install_e2e_tests`
prints these): air Ar40 about 94 500 fA; Ar36 about 309 cps on the CDD; raw
Ar40/Ar36 about 305.8 with 0.54 % scatter across the three airs (air's 298.56
over the simulated counter's yield of 0.9847 is 303.2; the test allows 3 %);
blanks about 11 fA; a save takes about 4 ms a run on SQLite.

### Not done

- PostgreSQL has not been run for anything in this plan. On this machine's
  Qt 6.11 the PostgreSQL store fails in tests that predate this work; a
  separate fix is pending. The Landing step's run with `PYCHRON_TEST_PG_URL`
  is therefore still owed, including the ingest time on PostgreSQL.
- The UI path of Task 9, Step 4 has not been driven by a person at the
  screen. The options were checked to parse and the program to start on
  `--install argus --sim-speed 20` without a display; what the windows show
  was not.
- Unknowns, ages, simulated samples, the step rule and the example
  irradiation are plans 2 and 3.

### Open, for the owner

- Every machine registers as the acquisition client `localhost`, because
  `HOSTNAME` is rarely exported to a program's environment. Two instruments
  writing to one server would be one client.
- A peak center after a blank centres on a peak of about 11 fA and updates
  the live field table.
- Whether the `ngx` placeholder table, or the profile's detector layout,
  matches a real NGX is unconfirmed.
