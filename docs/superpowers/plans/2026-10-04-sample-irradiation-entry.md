# Sample and Package Entry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Enter and edit PIs, projects, materials, samples, packages (an
irradiation is a package of kind `irradiation`), chronologies, levels, productions and positions in the DVC store,
and generate identifiers, from `elctl entry` and two `pychron-ui` windows.

**Architecture:** `libs/persistence` gains catalog reads, an all-or-nothing
catalog edit batch with optimistic concurrency on field values, and
identifier allocation over `identifier_counter`. A new Qt-free `libs/entry`
holds validation, CSV import, the level-sheet edit model and the identifier
planner. `apps/elctl` and `apps/pychron-ui` are thin layers over it.

**Tech Stack:** C++20, TinyORM over QtSql (private to persistence),
nlohmann_json, Qt 6 Widgets (UI only; `QPdfWriter` for the level sheet),
GoogleTest (+ QtTest in `tests/ui`).

**Status (2026-10-04):** implemented; deviations are recorded in spec section 14.

**Spec:** `docs/superpowers/specs/2026-10-04-sample-irradiation-entry-design.md`
(section numbers below refer to it). Schema background:
`docs/superpowers/specs/2026-10-01-dvc-schema-design.md`.

## Owner decisions (spec section 11)

Legacy Python acquisition is not supported (labs migrate first); identifiers
are one sequential counter (no NMGRL streams or offsets); changing the sample
of an analyzed position stays behind a confirmation; the package kind
(`irradiation` or `package`) is per package, stored in the new
`irradiation.kind` column. Entry, `elctl entry` and the UI say "package";
store names (`irradiation` table, `IrradiationSpec`, `irradiations()`) stay.
Nothing blocks any task.

## Global Constraints

- Work on a branch; no pull requests; merge to `main` when done (AGENTS.md).
- Public headers of `libs/persistence` and `libs/entry` are std-only. No Qt
  outside `libs/persistence/src` and `apps/pychron-ui`.
- All SQL text in `libs/persistence/src/sql/` or the migrations (P6).
- Never edit `0001_init.sql` or `0002_import_detail.sql`. Schema changes go in
  `0003_entry.sql`, then `python3 tools/ddl_sqlite.py` and commit the
  regenerated SQLite file.
- Every new `IStore` method needs a forwarder in
  `tests/ingest/forwarding_store.hpp`.
- Persistence tests run on SQLite always and on PostgreSQL when
  `PYCHRON_TEST_PG_URL` is set. Run both before merging the store tasks.
- `libs/entry` and `elctl entry` build only when `PYCHRON_PERSISTENCE_ENABLED`
  (Windows CI skips them, like `libs/ingest`).
- Ordering is never by address or by database row order (E7).
- Never skip or disable a failing test.
- Build and test: `cmake --build --preset dev && ctest --preset dev -R <pattern>`;
  UI: `--preset dev-ui`.
- Commit messages end with the session's Co-Authored-By line.

## Review Focus

1. A batch with one stale row and one refused row: nothing written, both
   reported, and `change_log` has no entry (Task 2 test
   `MixedFailuresWriteNothing`).
2. Two writers on different fields of one sample both succeed; on the same
   field exactly one does (Task 2 tests `DisjointFieldsBothApply`,
   `SameFieldRace`).
3. An identifier with an analysis, a load position or a lease cannot be
   updated, deleted or replaced, through either `apply_catalog_edits` or
   `allocate_identifiers` (Task 2 `AnalyzedIdentifierProtected`, Task 4
   `ReplaceAnalyzedRefused`).
4. Counter seeding ignores `01234`, `bu-FD-J`, `12a` and 19-digit text, and
   an allocation whose numbers are not exactly `last + 1 ... last + k` is an
   error (Task 4 `SeedRules`, `NonSequentialIsError`).
5. The planner's preview and its commit are the same assignments, and
   re-planning a numbered irradiation assigns nothing (Task 7 property test).
6. Clearing a position's sample keeps its identifier (Task 8
   `ClearKeepsIdentifier`), the opposite of legacy `labnumber_entry.py:705-707`.
7. Renaming a level rewrites the `ref_object.key` of its `level_geometry`,
   `level_production` and every `flux_position`, and is refused after the
   first analysis (Task 2 `RenameRewritesRefKeys`, `AnalyzedRenameRefused`).

---

## File structure

| File | Responsibility |
|---|---|
| `libs/persistence/migrations/pg/0003_entry.sql` (create), `migrations/sqlite/0003_entry.sql` (generated) | `irradiation.kind` (package kind); lower(name) indexes on sample and project |
| `libs/persistence/include/pychron/persistence/catalog.hpp` (create) | row structs, `SampleQuery`, `LevelSheet`, `CatalogEdit*`, `CatalogOutcome`, `IdentifierAllocation`, `AllocationOutcome` |
| `libs/persistence/include/pychron/persistence/store.hpp` (modify) | the new `IStore` methods |
| `libs/persistence/src/sql/catalog.hpp` (create) | read statements, editable-column allowlist, rule queries |
| `libs/persistence/src/catalog_read.cpp`, `catalog_edit.cpp`, `identifier_alloc.cpp` (create) | implementations |
| `libs/persistence/src/store_impl.hpp`, `store.cpp`, `CMakeLists.txt` (modify) | wiring |
| `libs/dvc/include/pychron/dvc/meta_files.hpp` (create), `libs/dvc/src/meta_layout.hpp` (modify) | `parse_holder`, `parse_chronology` made public |
| `libs/entry/` (create) | `names`, `sample_fields`, `csv`, `sample_import`, `sample_search`, `settings`, `identifier_plan`, `level_sheet`, `package_edit`, `holder_import`, `export` |
| `CMakeLists.txt` (modify) | add `entry` to `PYCHRON_LIBS` after `dvc` |
| `tests/persistence/test_catalog_read.cpp`, `test_catalog_edit.cpp`, `test_identifier_allocation.cpp` (create) | store tests |
| `tests/entry/` (create) | one `test_<unit>.cpp` per unit |
| `apps/elctl/src/entry*.cpp` (create), `apps/elctl/tests/test_entry*.cpp` (create) | `elctl entry` |
| `apps/pychron-ui/src/entry_bridge.*`, `samples_window.*`, `sample_import_dialog.*`, `packages_window.*`, `level_grid_model.*`, `holder_view.*`, `package_dialogs.*`, `identifier_dialog.*`, `level_sheet_pdf.*` (create) | UI |
| `docs/entry.md` (create), `AGENTS.md`, `docs/superpowers/plans/2026-09-30-implementation-priorities.md` (modify) | docs |

---

### Task 1: Migration and catalog reads (spec 5.1, 5.5)

**Files:** `0003_entry.sql`, `catalog.hpp`, `store.hpp`, `src/sql/catalog.hpp`,
`src/catalog_read.cpp`, `forwarding_store.hpp`; test `tests/persistence/test_catalog_read.cpp`.

- [x] Write `0003_entry.sql`: `irradiation.kind text NOT NULL DEFAULT 'irradiation'
      CHECK (kind IN ('irradiation','package'))`, `CREATE INDEX sample_name_lower_ix
      ON sample (lower(name));` and `project_name_lower_ix`. Run
      `python3 tools/ddl_sqlite.py` (check it handles `ADD COLUMN ... CHECK`;
      extend it if not). Confirm the schema parity test passes on both
      engines. Add `kind` to `IrradiationSpec` and `add_irradiation`, with a
      test that an existing irradiation keeps its kind (ensure semantics).
- [x] Declare the row structs and the read methods of spec 5.1 in `catalog.hpp`
      and `IStore`. `SampleFields` reuses the optional members of `SampleSpec`.
      Move them into a shared struct that `SampleSpec` embeds, without
      breaking callers.
- [x] Failing tests first, on a store seeded with two PIs, three projects, two
      materials, five samples, one irradiation with levels B and A (inserted
      in that order), positions, identifiers, one analysis and one flux
      revision:
  - `SamplesFilterAndCount`: text, PI, project and material filters; counts
    of positions and analyses.
  - `LevelsByName`: A before B whatever the insert order.
  - `LevelSheetJoinsEverything`: the sample, project, PI, material,
    identifier, analysis count, `in_load` and head J of each position; `z`
    and production from the ref heads.
  - `IrradiationCounts` (and `kind`).
  - `CounterAbsentIsNullopt`.
  - `MaxNumericIdentifier`, with `01234`, `bu-FD-J`, `12a`, `999`,
    `50001` and a 19-digit text: 50001. An empty store gives 0.
- [x] Implement. One statement per method, no N+1. J comes from the
      `flux_value` of the head revision of the position's `flux_position`
      ref. Numeric identifiers: PostgreSQL `identifier ~ '^[1-9][0-9]*$'`,
      SQLite `identifier NOT GLOB '*[^0-9]*' AND identifier NOT LIKE '0%' AND
      identifier <> ''`, cast to bigint. Lengths over 18 digits are ignored.
- [x] Add forwarders. Run persistence and ingest tests on SQLite and
      PostgreSQL. Commit.

### Task 2: Catalog edit batch (spec 5.2)

**Files:** `catalog.hpp`, `store.hpp`, `src/sql/catalog.hpp`,
`src/catalog_edit.cpp`; test `tests/persistence/test_catalog_edit.cpp`.

- [x] Declare `CatalogValue`, `CatalogFields`, the three edit structs,
      `CatalogEditBatch`, `StaleRow`, `Refusal`, `CatalogApplied`,
      `CatalogOutcome` and `apply_catalog_edits(Uuid client, const CatalogEditBatch&)`.
- [x] Write the allowlist in `src/sql/catalog.hpp`: table name, then the
      editable columns and their value types:

  | Table | Editable columns |
  |---|---|
  | `principal_investigator` | `last_name`, `first_initial`, `affiliation`, `email` |
  | `project` | `name`, `pi_uuid`, `checkin_date`, `comment`, `lab_contact`, `institution` |
  | `material` | `name`, `grainsize` |
  | `sample` | `name`, `project_uuid`, `material_uuid` and the `SampleFields` columns |
  | `irradiation` | `name`, `kind` |
  | `level` | `name`, `holder_ref_uuid`, `note` |
  | `irradiation_position` | `level_uuid`, `position`, `sample_uuid`, `weight`, `packet`, `note` |
  | `identifier` | `identifier`, `position_uuid` (update and delete only; inserts go through Task 4) |

- [x] Failing tests:
  - `InsertUpdateDeleteRoundTrip`: one test per table, parameterized.
  - `UnknownColumnIsError`, `WrongTypeIsError`.
  - `StaleWhenExpectedDiffers`: the stale row's `actual` holds the current
    values.
  - `DisjointFieldsBothApply`, and `SameFieldRace`: two threads with their
    own stores, 50 rounds, exactly one applied per round.
  - `MixedFailuresWriteNothing`: no rows changed and no `change_log` row.
  - `ReportsEveryStaleRow`.
  - `UniqueViolationIsRefusal`: a duplicate sample natural key and a taken
    position number.
  - `AnalyzedIdentifierProtected`: an identifier with an analysis, one with
    a load position, and one with a lease.
  - `AnalyzedSampleChangeNeedsFlag`.
  - `DeletePositionWithIdentifierRefused`.
  - `InUseDeleteNamesReferrers`.
  - `RenameRewritesRefKeys`: an irradiation rename and a level rename, both
    with flux positions.
  - `AnalyzedRenameRefused`.
  - `SampleUpdatedUtcAdvances`.
  - `AuditDetailHasBeforeAfter`: only the changed columns; one
    `change_log` row of kind `catalog` per batch.
  - `LaterEditNamesEarlierInsert`: a new project then a sample in it, in one
    batch.
- [x] Implement in one transaction (`BEGIN IMMEDIATE` on SQLite):
      validate every edit against the allowlist before any SQL; apply in
      order; collect stale rows and refusals instead of stopping at the
      first; check the rule queries before each statement they guard; roll
      back if anything was collected; else write the audit rows and take the
      change cursor. `IS NOT DISTINCT FROM` on PostgreSQL, `IS` on SQLite.
- [x] Map unique violations with the existing error classification in
      `src/sql/errors.cpp` (SQLSTATE 23505, SQLITE_CONSTRAINT_UNIQUE) to
      `Refusal{rule = "unique"}`. Other errors stay `Error`s.
- [x] Both engines; forwarders; commit.

### Task 3: Catalog edits with reference revisions (spec 5.3)

**Files:** `store.hpp`, `src/catalog_edit.cpp`, `src/unit_of_work.cpp`; test
`tests/persistence/test_catalog_edit.cpp` (add).

- [x] Add the `Conflict` alternative to `CatalogOutcome` and the overload
      `apply_catalog_edits(const Actor&, const CatalogEditBatch&, IUnitOfWork&, ChangesetKind, std::string)`.
- [x] Refactor `UnitOfWork::commit` so its body (payload inserts, ordered CAS,
      `head_move`) can run inside a transaction it did not open. Keep
      `commit()` behaviour identical. The existing revision and property
      tests are the guard.
- [x] Failing tests:
  - `LevelSaveWithRefs`: a position sample change plus a new
    `level_geometry` revision, one `change_log` row.
  - `RefConflictRollsBackCatalog`: a stale level_geometry head leaves the
    position unchanged.
  - `StaleCatalogRollsBackRefs`.
- [x] Implement: catalog edits, then the unit of work's staged revisions and
      CAS, then one cursor entry of kind `changeset` that names both. Commit.

### Task 4: Identifier allocation (spec 5.4)

**Files:** `catalog.hpp`, `store.hpp`, `src/identifier_alloc.cpp`; test
`tests/persistence/test_identifier_allocation.cpp`.

- [x] Declare `IdentifierAssignment`, `IdentifierAllocation`,
      `AllocationStale`, `AllocationOutcome`, `allocate_identifiers`.
- [x] Failing tests:
  - `SeedRules`: an absent counter seeds from the numeric maximum (Task 1's
    set of identifiers), or 0 in an empty store.
  - `StaleCounter`: `expected_last` lower than the counter returns the
    actual value and writes nothing.
  - `RaceOneWins`: two threads, the same `expected_last`.
  - `NonSequentialIsError` (a gap, a repeat, a number at or below last),
    `DuplicateTextRefused` (a hand-entered numeric identifier in the way).
  - `ReplaceInPlaceKeepsUuid`, `ReplaceAnalyzedRefused`.
  - `OneRefusalWritesNothing`: the third of five assignments refused, so
    none is written and the counter is unchanged.
  - `OverwrittenNumbersNotReused`.
  - `AfterImportContinuesAboveMax`: run the catalog import fixture
    (`tests/persistence/test_catalog_import.cpp` helpers), then allocate.
- [x] Implement: lock with `SELECT ... FOR UPDATE` (insert the seeded row first
      with `ON CONFLICT DO NOTHING`, then lock). Identifier text is the decimal
      number. Audit each identifier insert or replace in `change_entity`.
      Commit.

### Task 5: `libs/entry` skeleton, names, sample fields, CSV (spec 6)

**Files:** `libs/entry/CMakeLists.txt`, `include/pychron/entry/{names,sample_fields,csv}.hpp`,
`src/*.cpp`, root `CMakeLists.txt`, `tests/entry/CMakeLists.txt`,
`tests/entry/test_{names,sample_fields,csv}.cpp`.

- [x] CMake: `pychron_entry` static, returns early without
      `PYCHRON_PERSISTENCE_ENABLED`; links `pychron::persistence`, `pychron::core`,
      and privately `pychron::dvc`. Tests are globbed, like `tests/ingest`.
- [x] `names`, table-driven tests from the spec 6 rules:
  - PI: `Ross`, `Ross, J` and `Ross,J` pass; `Jake Ross` and `ross` fail;
    `NMGRL` passes when allowed; `"Ross, J"` parses into last name and
    initial.
  - Project: `IR-1010_a` passes; `1abc` and `a b` fail.
  - Irradiation increment: `NM-001`→`NM-002`, `NM-299`→`NM-300`,
    `NM-999`→`NM-1000`, `NM001`→`NM002`, `NM-ABC-001`→`NM-ABC-002`; none
    with the prefix gives `NM-001`. A name with whitespace is invalid.
  - Level letters: `A`→`B`, `Z`→`AA`, `AZ`→`BA`.
  - Packets: `P7`→`P8`, `9`→`10`, `P09`→`P10`.
- [x] `sample_fields`:
  - Lat/lon ranges and the both-or-neither rule.
  - UTM to WGS84 against at least four published points, one per
    hemisphere and quadrant, to 1e-6 degrees. Zone letters below `N` are
    southern.
- [x] `csv`:
  - Quoted fields with embedded delimiters, quotes and newlines.
  - BOM, CRLF and a trailing empty line.
  - Delimiter sniffing for `, ; \t |`, and a tie broken in that order.
  - Ragged rows reported with their line numbers.
- [x] Implement; commit.

### Task 6: Sample import and search (spec 6, 9.2)

**Files:** `include/pychron/entry/{sample_import,sample_search}.hpp`, `src/*.cpp`;
tests `tests/entry/test_sample_import.cpp`, `test_sample_search.cpp`.

- [x] `SampleImportPlan plan_sample_import(const CsvTable&, const ColumnMapping&,
      const CatalogSnapshot&, ImportOptions)`. `CatalogSnapshot` holds the
      PI, project, material and sample rows read through Task 1. It is
      plain data, so the planner is pure and tests need no store.
- [x] `default_mapping(headers)` with the alias table (`importer.py:36-69`).
- [x] `template_csv()`.
- [x] `to_batch(plan)`: inserts in foreign-key order with client-generated
      UUIDv7s, and updates (with `expected`) only when `update_existing`.
- [x] Failing tests:
  - One per row state.
  - Every error message for a row is listed.
  - Required fields: sample, project, PI and material.
  - Duplicates within the file.
  - A new PI, project and material created once for many rows.
  - Grainsize is part of the material key (the legacy bug
    `sample_entry.py:822-831`).
  - UTM rows, and UTM ignored when lat/lon are present.
  - `exists` versus `update`.
  - `TemplateParsesBack`.
- [x] `near_duplicates(name, samples)`: `FC-2` matches `fc 2` and `FC_2`;
      the result includes rows from other projects.
- [x] Store round trip: one integration test applies a plan to a SQLite
      store and re-plans the same file. Every row then reads `exists`.
      Commit.

### Task 7: Settings and the identifier planner (spec 7, 8)

**Files:** `include/pychron/entry/{settings,identifier_plan}.hpp`, `src/*.cpp`;
tests `test_settings.cpp`, `test_identifier_plan.cpp`.

- [x] `EntrySettings` with JSON read and write (nlohmann_json): defaults for
      missing keys, an unknown key kept, and an error for a wrong type.
      `load_settings(IStore&)` and `save_settings(IStore&, Actor, settings,
      expected_head)` go through the `document` reference.
- [x] `IdentifierPlan plan_identifiers(const std::vector<LevelSheet>&, std::int64_t last,
      bool overwrite)`: the section 8 pseudo-code. The plan carries the
      assignments, `expected_last` and the resulting last.
- [x] `human_error_checks(sheets, settings, irradiation_name)`: the two
      warnings of section 8.
- [x] Failing tests, with hand-worked expected numbers:
  - Two levels: numbers run on from A into B with no gap.
  - Monitors and unknowns interleave in position order.
  - Overwrite on and off; with overwrite, replaced positions get new
    numbers above `last`.
  - An analyzed identifier is never overwritten.
  - Positions without a sample are skipped.
  - Level order comes from names, not input order.
- [x] Property test (seeded, 500 cases): random sheets give exactly
      `last + 1 ... last + k` in (level name, position) order. Applying a plan and re-planning
      without overwrite yields no assignments.
- [x] Store test: plan, `allocate_identifiers`, read back. The level sheets
      show exactly the planned identifiers.
- [x] Commit.

### Task 8: Level sheet, irradiation edit, holders, export (spec 6)

**Files:** `include/pychron/entry/{level_sheet,package_edit,holder_import,export}.hpp`,
`src/*.cpp`; `libs/dvc/include/pychron/dvc/meta_files.hpp`; tests
`test_level_sheet.cpp`, `test_package_edit.cpp`, `test_holder_import.cpp`,
`test_export.cpp`.

- [x] Move the declarations of `parse_holder`, `parse_chronology` and their
      result structs from `libs/dvc/src/meta_layout.hpp` into the public
      `meta_files.hpp`. `meta_layout.hpp` includes it. Run the dvc tests.
- [x] `LevelSheetEdit` (operations as spec 6). `to_batch()` emits changed
      fields only, with loaded values as `expected`. `stage_refs(IUnitOfWork&)`
      stages z and production revisions with the loaded heads. Failing tests:
  - `ClearKeepsIdentifier`.
  - `AssignToEmptyHoleInserts`.
  - `UnchangedEmitsNothing`.
  - `MoveRefusedWhenAnalyzed`.
  - `FillPacketsSequence`.
  - `ValidateIdentifierNeedsSample`.
  - `OrphansAfterHolderShrink`: kept and listed, never deleted.
  - `HolesFromHolderOrdinal`: position n is ordinal n-1, and `hole_id` is
    shown.
- [x] `NewPackage` (`package_edit.hpp`): name, package kind, doses, reactor, levels. `validate()`
      covers dose order, `end > start` and `power > 0`, and the reactor is
      required for kind `irradiation`; kind `package` writes no chronology or production
      (test `PackageWritesNoRefs`). `set_kind` is a catalog edit that leaves
      reference data alone (test `KindChangeKeepsRefs`). `to_batch()` and `stage_refs()` cover the
      chronology, the copied production, the level productions and the
      level z values. Store test: one call creates the whole irradiation,
      and its `resolve_refs` for a position returns chronology and
      production. Also test the estimated-J helper.
- [x] `import_holder(text, name)`: legacy files with and without hole
      numbers, `#` and blank lines (which still advance the implicit id,
      `meta_object.py:260-299`), duplicate hole ids refused.
- [x] `export_level_csv`, `export_package_csv` round-trip through
      `positions import`. Commit.

### Task 9: `elctl entry` (spec 10)

**Files:** `apps/elctl/src/entry.cpp`, `entry.hpp`, `entry_stub.cpp` (built
without persistence, prints "built without persistence", like
`import_stub.cpp`), `cli.cpp`, `apps/elctl/CMakeLists.txt`; tests
`apps/elctl/tests/test_entry.cpp`.

- [x] Subcommands as listed in spec 10, each parsing into a struct, then one
      function calling `libs/entry` and the store. User and client come
      from `--user` (default `$USER`) and the hostname, with role
      `reduction`.
- [x] Output: one line per created, updated, stale or refused row. Exit codes
      0, 1 and 2.
- [x] Tests on a temp SQLite store:
  - `samples import --dry-run` writes nothing and prints the plan.
  - An import, then a re-import, which reports everything as existing.
  - `package add P-1 --kind package` writes no chronology.
  - `package add NM-001 --levels A-C --holder 24-hole`, after
    `holders import`.
  - `positions import`, then `identifiers generate --dry-run`, then the real
    run. The printed plan equals the stored identifiers.
  - A stale run exits with 2.
- [x] Commit.

### Task 10: UI bridge, Entry menu, samples window, import dialog (spec 9, 9.1, 9.2)

**Files:** `entry_bridge.{hpp,cpp}`, `samples_window.{hpp,cpp}`,
`sample_table_model.{hpp,cpp}`, `sample_import_dialog.{hpp,cpp}`,
`main_window.cpp`, `data_main_window.cpp`, `apps/pychron-ui/CMakeLists.txt`
(explicit source list, under `PYCHRON_UI_HAS_STORE`), `tests/ui/test_samples_window.cpp`,
`tests/ui/CMakeLists.txt`.

- [x] `EntryBridge`: one worker thread that opens its own store (the
      `StoreSource::Impl::call` pattern) and exposes async calls. Each call
      takes a callback, and the callback runs on the GUI thread through
      `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`. It is
      destroyed before the windows that use it are, so declare it first.
- [x] Entry menu in both main windows when a store URL is known. The actions
      open singleton windows.
- [x] `SampleTableModel`: rows from `samples()`, edits kept as
      `CatalogUpdate`s keyed by uuid with the loaded values as `expected`,
      and a new-row sentinel. PI, project and material delegates accept new
      text, which becomes an insert.
- [x] `SamplesWindow`: filters, table, detail form (with the UTM mode),
      Save, Revert, Delete (enabled only when there are no positions or
      identifiers), and paste into the import dialog. Stale rows are tinted,
      with a tooltip naming the other client's values.
- [x] `SampleImportDialog`: mapping table, preview with the state filter,
      error export, template, Import.
- [x] Headless tests on a SQLite store:
  - Edit two cells and save. The store has them and the model is clean.
  - Concurrent edit: change the row through a second store, then save. The
    row is tinted and nothing is written.
  - A new sample with a new project and PI.
  - A TSV paste opens the preview with the expected states.
- [x] Commit.

### Task 11: Packages window and its dialogs (spec 9.3)

**Files:** `packages_window.{hpp,cpp}`, `level_grid_model.{hpp,cpp}`,
`package_dialogs.{hpp,cpp}` (new package, new level, production
editor, clear fields, fill packets), `identifier_dialog.{hpp,cpp}`,
`tests/ui/test_packages_window.cpp`.

- [x] Tree model over `irradiations()` and `levels()`, the package kind shown
      as an icon.
- [x] `LevelGridModel` over a `LevelSheetEdit`: the columns of 9.3, analyzed
      rows tinted, orphans marked. Edits to weight, packet and note go
      through `LevelSheetEdit`.
- [x] Sample picker dock that reuses the samples filters. "Assign to
      selected" confirms once, with the analysis count, when any selected
      row is analyzed.
- [x] Level dock (holder, z, production, note) and chronology dock (hidden
      for kind `package`), with times in the lab's zone converted with
      `ingest::tz` helpers or `std::chrono::zoned_time`, whichever the tree
      already uses.
- [x] Dialogs as spec 9.3, New Package with the kind choice. It commits
      in one call. A kind switch on the Level dock asks for confirmation and
      shows or hides the chronology and production editors.
- [x] Generate Identifiers dialog: settings summary, overwrite, warnings,
      preview over every level, Commit, and re-preview on stale.
- [x] Unsaved-edits prompt on level change and on close.
- [x] Headless tests:
  - Create an irradiation through the dialog and check it in the store.
  - Assign, save and reload.
  - Generate, then compare the preview to the stored identifiers.
  - Stale generate: bump the counter through a second store, and the
    dialog re-previews.
  - Switching level with edits prompts.
- [x] Commit.

### Task 12: Holder view, holders dialog, PDF (spec 9.3, 9.4, 9.5)

**Files:** `holder_view.{hpp,cpp}` (QGraphicsView), `holders_dialog.{hpp,cpp}`,
`level_sheet_pdf.{hpp,cpp}`; tests in `tests/ui/test_packages_window.cpp`
and `tests/ui/test_level_sheet_pdf.cpp`.

- [x] Holder view: holes from `HolderValue`, filled by project colour,
      selection synced both ways with the grid (click and rubber-band).
- [x] Holders dialog: list, preview, import `.txt` through `import_holder`.
- [x] PDF via `QPdfWriter` and `QPainter`: a summary page and one page per
      level, with a row for every hole. Test: the file is written, the page
      count is 1 + the number of levels, and the text (through
      `QTextDocument` when the table is built as one) contains every
      identifier.
- [x] Commit.

### Task 13: Docs, full verification, merge

- [x] `docs/entry.md`: user guide for samples, packages, identifiers,
      settings, `elctl entry`, and the rule that a lab migrates its legacy
      database before using entry (spec E1).
- [x] AGENTS.md: one bullet under Build and test: `libs/entry` builds only
      with persistence; catalog edits go through `apply_catalog_edits`, never
      ad hoc UPDATEs; identifiers only through `allocate_identifiers`.
- [x] Priorities plan: add row 8a "Sample/irradiation entry", pointing at the
      spec and this plan.
- [ ] Full build and test on `dev` and `dev-ui`, persistence on PostgreSQL
      (`PYCHRON_TEST_PG_URL`), gcc 14 and clang 18 with ASan/UBSan.
- [ ] Rebase on `origin/main`, merge, push. (Session branch pushed; merging to `main` is the owner's.)
