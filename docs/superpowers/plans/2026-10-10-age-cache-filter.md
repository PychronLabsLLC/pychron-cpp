# Cached ages and the browser's age filter: plan

Spec: `docs/superpowers/specs/2026-10-09-age-cache-filter-design.md`
(approved 2026-10-09; acquisition deferred, its section 4.6).

Branch: `feat/age-cache-filter`, cut from `develop`.

Every task is written test first, built with the `dev-ui` preset, checked
with `python3 tools/quality_check.py`, and committed on its own. No task adds
a migration. Tasks 1 to 3 and 7 touch a query or the loader, so each of them
carries numbers in its commit message (AGENTS.md, "How to know"), taken on a
copy of a lab's store (`~/pychron-dev/legacy-db/store.db`, 8716 analyses;
work on a copy, never on the file itself).

Before Task 1, take the baseline once and keep it for the later commit
messages:

    PYCHRON_BENCH_DB=sqlite:/path/to/copy.db PYCHRON_BENCH_N=24 \
      build/dev-ui/tests/processing/pychron_processing_store_tests --gtest_filter='StoreLoadTiming.*'

at 24, 400 and 2000 analyses.

The order is from the store outwards: the cache's rule, the fingerprint on
the model, the sink, the writers, then the reads (browse) and the browser.
After Task 5 the cache fills itself; after Task 7 it can be searched; the
window changes only in Task 9.

## Task 1: one fingerprint per analysis in the cache

Spec 4.2.

- `libs/persistence/include/pychron/persistence/store.hpp`: `DerivedPut`
  (`Uuid analysis; Sha256Digest fingerprint; std::vector<DerivedRow> rows;`)
  and `IStore::put_derived_many(std::span<const DerivedPut>, const std::string& reduction_version)`.
- `libs/persistence/src/refs.cpp`: `put_derived_many` in one `WriteTx`: for
  each put, delete the analysis's rows whose fingerprint differs, then
  `insert_or_ignore` each row. `put_derived` becomes a call of it with one
  put.
- `libs/persistence/src/sql/statements.hpp`: `kDeleteOtherDerived`
  (`DELETE FROM derived_value WHERE analysis_uuid = ? AND fingerprint <> ?`).
- `libs/persistence/src/store.cpp`: `TinyStore` forwards.
- `tests/ingest/forwarding_store.hpp`: forwards the new method.
- Tests, `tests/persistence/test_references.cpp`:
  - `DerivedValuesAreServedOnlyForCurrentInputs` updated: after a put under
    a new fingerprint, `prune_derived` finds nothing left to remove.
  - new `APutReplacesAnotherFingerprintsRows`: two puts under two
    fingerprints leave the rows of the second only.
  - new `ManyPutsAreOneTransaction`: three analyses in one call; a put whose
    analysis does not exist fails the call and leaves none of the three.
- Commit: `feat(persistence): the derived cache keeps one fingerprint per analysis`.

## Task 2: the fingerprint is taken at load

Spec 4.3.

- `libs/persistence/src/refs.cpp` and its internal header: split
  `input_fingerprint` into the pure
  `fingerprint_of(const std::vector<Head>&, const std::vector<ResolvedRef>&, const std::string& reduction_version)`
  and the present function, which reads and calls it. Declared in
  `store.hpp` as a free function so the store source can call it with what
  it has read.
- `libs/processing/include/pychron/processing/model.hpp`:
  `Analysis::input_fingerprint` (`std::string`, lower-case hex; empty for a
  source that has none).
- `libs/processing/adapters/store/src/store_source.cpp`: `StoreSource::load`
  computes it from the heads and the resolved references it already holds
  (no statement added) with `reduction::kReductionVersion`.
- Tests:
  - `tests/persistence/test_references.cpp`, new
    `TheFingerprintOfWhatWasReadIsTheInputFingerprint`: `fingerprint_of`
    over `read_heads` and `resolve_refs` equals `input_fingerprint`, before
    and after a new flux head.
  - `tests/processing/test_store_source.cpp`, new
    `LoadCarriesTheInputFingerprint`: equals the store's
    `input_fingerprint` for that analysis; changes when its flux changes.
- Measurement in the commit message: `StoreLoadTiming` at 24, 400, 2000,
  before and after. The load must not have gained a statement; a time that
  grew is looked into before the commit, not after.
- Commit: `feat(processing): an analysis loaded from the store carries its input fingerprint`.

## Task 3: the sink

Spec 4.1, 4.4, and the "not written when nothing changed" rule of 4.5.

- `libs/processing/include/pychron/processing/source.hpp`: `DerivedAge`,
  `IDerivedSink`, `IAnalysisSource::derived()` (default null).
- `libs/processing/include/pychron/processing/derived.hpp`,
  `src/derived.cpp`: `derived_age(const ReducedAnalysis&, const ReductionSettings&)`.
  Null when the analysis has no fingerprint or the settings are not the
  default. Age absent in the result when the reduction gave none. Ages
  converted to Ma whatever `ReductionConstants::age_units` says.
- `libs/processing/adapters/store`: `StoreSource` implements the sink.
  `put` posts one task to the pool and returns; the task reads the cached
  fingerprints of the batch in one statement, drops the analyses whose
  fingerprint is already the cached one, and calls `put_derived_many` for
  the rest. A failure is logged and dropped. Rows written: `age`,
  `age_w_j`, `kca`; an analysis with no age gets `age` with null value and
  error.
- `libs/persistence`: `IStore::derived_fingerprints(std::span<const Uuid>)`
  (analysis to fingerprint of its `age` row) and its statement
  `kDerivedFingerprints`; forwarded in `forwarding_store.hpp`.
- `StoreSource` gains `flush_derived()` for tests and for `elctl`: returns
  when every queued write is done.
- Tests:
  - `tests/processing/test_derived.cpp`: no fingerprint, null; settings
    other than default, null; an air (no age), a `DerivedAge` without age;
    an unknown with flux, age and `age_w_j` in Ma, also when the constants
    say ka.
  - `tests/processing/test_store_source.cpp`: `TheSinkFillsTheCache` (rows
    as in spec 4.1), `ACurrentAgeIsNotWrittenAgain` (`computed_utc`
    unchanged by a second put), `AChangedFluxReplacesTheAge`,
    `AnAnalysisWithNoAgeGetsANullAge`.
  - `tests/persistence/test_schema.cpp`, `SchemaIndexes`, new
    `CachedFingerprintsAreFoundByAnalysis`: the plan of
    `kDerivedFingerprints` uses the primary key of `derived_value` and has
    no `SCAN`.
- The sink adds work to StoreSource's own threads: run
  `tests/processing/pychron_processing_store_tests` under
  `-DPYCHRON_SANITIZE=thread` before the commit.
- Commit: `feat(processing): a sink that keeps reduced ages in the store's cache`.

## Task 4: every reduction the application makes writes the cache

Spec 4.5, writer 1.

- `libs/processing/src/units_builtin.cpp`, `ReduceUnit::execute`: after
  reducing, one `put` with every cacheable analysis, when the run's source
  has a sink.
- `apps/pychron-ui/src/recall_window.cpp`, `RecallWindow::display`: one
  `put` for its analysis.
- `libs/processing/adapters/store/src/flux_store.cpp`, `reduce_monitor`:
  unchanged, with a comment saying why it never caches (it reduces without
  flux on purpose).
- Tests:
  - `tests/processing/test_store_source.cpp`: `ReducingThroughAUnitFillsTheCache`,
    `AUnitWithOtherSettingsWritesNothing`, `AMonitorReducedForFluxIsNotCached`.
  - `tests/processing/test_units.cpp`: a `ReduceUnit` over a `MemorySource`
    (no sink) runs as before.
  - `tests/ui/test_data_windows.cpp`: a recall over a source with a
    recording sink puts once, with the analysis's uuid.
- Measurement in the commit message: `StoreLoadTiming` at 24, 400, 2000 on
  a store whose cache is already current (the unit now makes one read per
  run and no write).
- Commit: `feat(processing): reducing an analysis keeps its age in the cache`.

## Task 5: the backfill

Spec 4.5, writer 2.

- `libs/processing/adapters/store/include/pychron/processing/derive.hpp`,
  `src/derive.cpp`: `DeriveOptions`, `DeriveStats`, `derive_ages`. Walks by
  `browse` pages of 200 with no filter (invalid analyses included); for
  each page, cached fingerprints in one statement, `input_fingerprint` per
  analysis, and load, reduce and put only where the two differ or
  `options.all`. `keep_going` is asked after each page. A load that fails
  is counted and named, and the walk goes on.
- `apps/elctl/src/db.cpp`: `db derive --db <url> [--all] [--limit N] [--quiet]`;
  usage text; a line per page unless `--quiet`, a summary line always; exit
  status 0, or 1 when any analysis failed. Opens with `migrate = false`.
  `db.hpp`'s header comment lists the subcommand.
- Tests:
  - `tests/processing/test_derive.cpp` (built into
    `pychron_processing_store_tests`; add to the list in
    `tests/processing/CMakeLists.txt`): the cases of spec section 6.
  - `apps/elctl/tests/test_db_cmd.cpp`: output and status of `db derive` on
    a small store, a second run reporting everything current, `--limit`,
    a store that is behind its migrations refused.
- Measurement in the commit message: `elctl db derive` over the whole copy
  of the lab's store, first run and second, wall time and the stats line.
- Commit: `feat(elctl): db derive fills the cache of ages`.

## Task 6: import derives

Spec 4.5, writer 3.

- `apps/elctl/src/import.cpp`: after `import run` ends its walk with nothing
  left to do, call `derive_ages` on the same database unless `--no-derive`;
  its summary line follows the import's. An import stopped by
  `--max-batches` or an interrupt does not derive. A failure of the derive
  pass is reported and does not change the import's exit status. Usage text.
- Nothing in `libs/ingest` or `libs/dvc` changes; check with
  `git diff --stat` before the commit.
- Tests, `apps/elctl/tests` (the import command's test file): a run to the
  end leaves ages in `derived_value`; `--no-derive` leaves none; a run cut
  short by `--max-batches` leaves none.
- `docs/legacy_import.md`: the derive pass and `--no-derive`.
- Commit: `feat(elctl): import run fills the cache of ages when the walk ends`.

## Task 7: browse reads and filters by the cached age

Spec 4.7, store and processing.

- `libs/persistence/include/pychron/persistence/store.hpp`:
  `BrowseFilter::age_min`, `age_max`; `BrowseRow::age`, `age_err`.
- `libs/persistence/src/sql/browse.hpp`: `kBrowseAgeJoin`, `kBrowseAgeMin`,
  `kBrowseAgeMax`; `kBrowseSelect` gains `dv.value AS age, dv.error AS age_err`.
- `libs/persistence/src/browse.cpp`: `build_where` adds the bounds;
  `browse` always joins for the page and joins for the count only when a
  bound is set; `facet` joins only when a bound is set; `row_from` reads the
  two columns; `load_analysis_detail` joins.
- `libs/processing/include/pychron/processing/source.hpp`:
  `BrowseQuery::age_min`, `age_max`; `AnalysisSummary::age`, `age_err`.
  `src/source.cpp`: `matches()` applies them (an absent age matches no
  bound); `MemorySource::set_age(uuid, age, err)`.
- `libs/processing/adapters/store/src/store_source.cpp`: `to_store_filter`
  and the row mapping carry the four fields.
- Tests:
  - `tests/persistence/test_browse.cpp`: `AgeBounds` (each end, both, a
    null age not matched, an analysis with no row not matched),
    `RowsCarryTheCachedAge`, `PagingAndTotalsUnderAnAgeBound`,
    `FacetsUnderAnAgeBound`.
  - `tests/persistence/test_schema.cpp`, `SchemaIndexes`:
    `ABrowsePageFindsItsAgesByPrimaryKey` (no `SCAN dv`),
    `ACountWithoutAnAgeBoundLeavesTheCacheAlone` (the statement does not
    name `derived_value`), `ACountWithAnAgeBoundScansNeitherTable`.
  - `tests/processing/test_store_source.cpp`:
    `StoreSourceMapping.QueryAndCursorRoundTrip` extended; a summary carries
    the age the sink wrote.
  - `tests/processing/test_units.cpp` or the source's own tests: `matches()`
    with age bounds on a `MemorySource`.
- Measurement in the commit message, on the copy of the lab's store after
  `elctl db derive`: page, count and each facet statement, with and without
  an age bound (`sqlite3`, `.timer on`, as in section 5.1 of the first
  spec), and their plans.
- Run the persistence and store-source tests on PostgreSQL as well
  (`PYCHRON_TEST_PG_URL`) before the commit: a new join and a new filter.
- Commit: `feat(persistence): browse carries the cached age and filters by it`.

## Task 8: reference saves say when ages go out of date

Spec 4.5, item 4.

- `libs/processing` `flux_view.hpp` (the flux window's status line, shared
  with `elctl flux`): after a level's fit is saved, the line gains
  `cached ages of <n> analyses are now out of date; run elctl db derive`,
  `n` from the level's positions the save already holds.
- The blank and IC factor fit windows save through revisions and do not
  hold the count of analyses that depend on what they saved: no message
  there (the spec allows it). Recorded in the spec's section 4.5 when this
  task lands.
- Tests: `tests/processing/test_flux_view.cpp` (or where the status line is
  tested now), the line after a save, and no such line when nothing was
  saved; `apps/elctl/tests`, `elctl flux` prints it.
- Commit: `feat(processing): saving a flux fit says which cached ages it outdates`.

## Task 9: the browser

Spec 4.7, browser.

- `apps/pychron-ui/src/analysis_table_model.{hpp,cpp}`: column `Age` after
  `Extract`, header `Age (Ma)`, text `value ± error` to the error's scale,
  empty without an age; header tooltip says the age is the cached one.
  Tests that name columns by number are moved to the enum.
- `apps/pychron-ui/src/data_browser_window.{hpp,cpp}`: under the date
  controls, `Age from` and `to` (a checkbox and a `QDoubleSpinBox` each,
  Ma, `editingFinished`); `query()` sets `age_min`/`age_max`; low above
  high is no query and `Age from is above to` in the status line; with a
  bound set the status line reads
  `<shown> of <total> analyses with a cached age in range`.
- Tests, `tests/ui/test_data_windows.cpp`: `browser_shows_the_cached_age`,
  `browser_age_range` (each end, both, low above high, the status line),
  using `MemorySource::set_age`.
- Commit: `feat(ui): the data browser shows the cached age and filters by it`.

## Task 10: documents

- `docs/user/06-data-analysis.md`, "The data browser": the Age column and
  the age range; that the age is cached, when it is computed, when it goes
  out of date and how to bring it up to date; that a figure always
  recomputes.
- A user page for `elctl db derive` where `elctl db` is documented
  (`docs/dev_setup.md` or the installation runbook, wherever
  `elctl db migrate` is described now).
- `AGENTS.md`, under "Build and test", compressed like the rest: the cache's
  three rules (fingerprint from load; one fingerprint per analysis; count
  and facet statements join `derived_value` only under an age bound), and
  that `libs/ingest`, `libs/dvc` and `libs/experiment` do not reduce.
- Spec: status "implemented"; anything that departed from it in Tasks 1 to
  9 written into it in the same branch.
- Commit: `docs: cached ages, the age filter and elctl db derive`.

## Before landing

- `ctest` whole, `python3 tools/quality_check.py`, on the branch rebased on
  `origin/develop`.
- Persistence, store-source and derive tests on PostgreSQL
  (`PYCHRON_TEST_PG_URL`).
- `pychron_processing_store_tests` under `-DPYCHRON_SANITIZE=thread`.
- On a copy of the lab's store: `elctl db derive`, then the browser opened
  on it and looked at, an age range typed in, a figure opened from the
  result.
