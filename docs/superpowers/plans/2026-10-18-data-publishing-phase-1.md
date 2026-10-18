# Data Publishing Phase 1 (Release Model) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A lab can turn a selection of analyses into an immutable, verifiable release directory (Schaen tables, OGC API - Records catalog, GeoParquet tables, manifest) from the store with `elctl publish`, with embargoes honoured and nothing public by default.

**Architecture:** One additive migration adds the release, embargo and external-id tables and the PostgreSQL-only `pub.*` views; the store gains the reads and writes those tables need plus loading an analysis "as of" a bookmark. A new Qt-free library `libs/publish` turns a release's pinned analyses into row tables, a manifest, a records catalog and the files of the directory; Apache Arrow (vcpkg, optional) writes the Parquet. `elctl publish` and `elctl embargo` drive it; no UI in this phase.

**Tech Stack:** C++20, `libs/persistence` (TinyORM over SQLite and PostgreSQL), `libs/processing` (report, dataset, store source), nlohmann-json, Apache Arrow C++ with Parquet from vcpkg behind `PYCHRON_PUBLISH`, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-10-18-data-publishing-and-hosted-sharing-design.md`, section 13 phase 1 (sections 4, 5, 6.1, 6.2, 12 for the rules). Read `libs/persistence/AGENTS.md` before Tasks 1 to 3 and before any query added later.

## Global Constraints

- One migration for the whole phase: `0007_release.sql` (develop already has `0006_pi_identity`; the spec's "0006" is amended in Task 1). Additive only: new tables, nullable columns, indexes, views. Commit footer `BREAKING CHANGE: run elctl db migrate`.
- Schema source is `libs/persistence/migrations/pg/0007_release.sql`; `python3 tools/ddl_sqlite.py` regenerates the SQLite file, which is committed. Never edit an applied migration. `-- @sqlite skip` for PostgreSQL-only statements (the `pub` schema and its views).
- No SQL text outside `libs/persistence/src/sql/`. Every column a query filters or joins on gets an index that starts with it, with an `EXPLAIN QUERY PLAN` test in `SchemaIndexes` (`tests/persistence/test_schema.cpp`). No `OR` across tables; the embargo scope query is a `UNION`. Never a count in a subquery per row.
- Qt stays out of `libs/`. `libs/publish` depends on `persistence`, `processing`, `processing_store`, `reduction`, `core`, nlohmann-json; Arrow headers appear only in `libs/publish/src/parquet_writer.cpp`.
- Every file the build writes for the user goes through `pychron::mark_as_user_file` (`libs/core` `user_file.hpp`) after it is closed.
- Numbers in canonical row text and in JSON are shortest round-trip (`std::to_chars`), never locale-dependent; a non-finite double is a null cell. `TOML_FLOAT_CHARCONV=0` in `libs/core/CMakeLists.txt` is untouched.
- Manifest paths use `/` on every platform. Times are ISO-8601 UTC with microseconds and `Z`. Uuids are the 36-character lowercase form.
- A new stored format (the manifest, the canonical row text, the Parquet tables) gets a fixture written on one platform and compared on Windows CI (Task 7, `tests/publish/fixtures/`).
- Nothing public by default: a new release is `draft` and `private`; `publish` keeps `private` unless `--public`.
- Errors are `pychron::Result<T>`; `libs/` throws nothing.
- Software version in the manifest is `pychron-cpp/<PROJECT_VERSION>` (no git sha exists in the build; the spec's `+g<sha>` is amended in Task 1 to "when the build defines `PYCHRON_GIT_SHA`").

## Review Focus

1. A sample with no location: `geometry` and `bbox` null in `samples.parquet` and `analyses.parquet`, record `geometry: null`, catalog `extent.spatial` computed from the samples that have one (absent when none has). Test in Task 4 (`Tables.SampleWithoutLocationHasNullGeometry`) and Task 6 (`Records.CatalogExtentSkipsSamplesWithoutLocation`).
2. An embargo on one sample of a two-sample release: the public build drops that sample, its analyses and its interpreted ages, and `manifest.build.omitted_embargoed` counts them; the private build keeps them and sets `private_build: true`. Test in Task 6 (`Build.PublicBuildOmitsEmbargoedSample`).
3. A member with no age (no flux resolved): age columns null, `age_note` set, the sample's `n_ages` excludes it, the record's `pychron:ages` omits it; the build still succeeds. Test in Task 4 (`Tables.AnalysisWithoutAgeHasNullAgeColumns`).
4. A sample name that is not file-safe, or two samples with one name in two projects: directory and figure paths are the slug plus `-<first 8 of uuid>` on collision, and `manifest.files` paths are what exists on disk. Test in Task 6 (`Build.SampleNamesAreMadeFileSafeAndUnique`).
5. A retag after creation (`invalid` on a member) does not change the next build: the release pins the tag revision. Test in Task 5 (`Release.RetagAfterCreationDoesNotChangeMembersOrPins`).

---

## File structure

| Path | Responsibility |
|---|---|
| `libs/persistence/migrations/pg/0007_release.sql`, `migrations/sqlite/0007_release.sql` | release, release_sample, release_interpreted_age, embargo, external_id, pub schema and views |
| `libs/persistence/include/pychron/persistence/publish.hpp` | plain structs: ReleaseRow, ReleaseInsert, ReleaseUpdate, EmbargoRow, EmbargoInsert, ExternalIdRow, ExternalIdInsert, enums |
| `libs/persistence/include/pychron/persistence/store.hpp` | new `IStore` virtuals (release, embargo, external id, `load_analysis_at`, `resolve_refs_at`) |
| `libs/persistence/src/sql/publish.hpp` | the statements, including the one visibility rule text (spec 4.5) |
| `libs/persistence/src/publish.cpp` | implementation of those virtuals |
| `libs/processing/adapters/store/include/pychron/processing/store_source.hpp`, `src/store_source.cpp` | `StoreSource::load_at(uuid, bookmark)` |
| `libs/publish/CMakeLists.txt`, `cmake/PychronDependencies.cmake`, `vcpkg.json` | the library; `PYCHRON_PUBLISH` option and Arrow |
| `libs/publish/include/pychron/publish/tables.hpp`, `src/tables.cpp` | row model of the five tables, canonical row text, rows_sha256 |
| `libs/publish/include/pychron/publish/release.hpp`, `src/release.cpp` | create, publish, withdraw, set visibility, delete, selection to members |
| `libs/publish/include/pychron/publish/manifest.hpp`, `src/manifest.cpp` | Manifest struct, JSON read and write, verify |
| `libs/publish/include/pychron/publish/records.hpp`, `src/records.cpp` | catalog.json and records/*.json |
| `libs/publish/include/pychron/publish/build.hpp`, `src/build.cpp` | build_release: directory, Schaen tables, constants, CITATION, README, LICENSE, manifest; calls the Parquet writer |
| `libs/publish/include/pychron/publish/parquet.hpp`, `src/parquet_writer.cpp`, `src/parquet_stub.cpp` | IParquetWriter; Arrow implementation; stub without Arrow |
| `libs/publish/include/pychron/publish/embargo.hpp`, `src/embargo.cpp` | set, lift, list, scope resolution by name |
| `libs/publish/licences/` | SPDX licence texts shipped (`CC-BY-4.0.txt`, `CC0-1.0.txt`, `CC-BY-SA-4.0.txt`) |
| `apps/elctl/src/publish.{hpp,cpp}`, `publish_stub.cpp`, `embargo.{hpp,cpp}`, `embargo_stub.cpp`, `cli.cpp`, `CMakeLists.txt` | the commands |
| `tests/persistence/test_publish.cpp`, `test_schema.cpp` | store tests |
| `tests/publish/` | `CMakeLists.txt`, `release_fixture.hpp`, `test_tables.cpp`, `test_release.cpp`, `test_build.cpp`, `test_manifest.cpp`, `test_records.cpp`, `test_parquet.cpp`, `test_elctl_publish.cpp`, `test_elctl_embargo.cpp`, `fixtures/` |
| `docs/release_format.md`, `docs/user/08-elctl-reference.md`, `docs/export.md`, the spec | documentation |

Test binary of `tests/publish` is `pychron_publish_tests` (built only with persistence, like `tests/persistence`); it links `pychron::publish`, `pychron_elctl_lib` (the first test binary to drive elctl command code; `apps/elctl/CMakeLists.txt` already says that is what the lib is for), `TinyOrm::TinyOrm`, `Qt6::Core`, `Qt6::Sql` for `tests/persistence/store_fixture.hpp`, and `GTest::gtest_main`. Build: `cmake --build --preset dev --target pychron_publish_tests`; run `build/dev/tests/publish/pychron_publish_tests --gtest_filter='<Suite>.*'`.

---

### Task 1: Migration `0007_release.sql` and the spec amendments

**Files:**
- Create: `libs/persistence/migrations/pg/0007_release.sql`
- Create (generated): `libs/persistence/migrations/sqlite/0007_release.sql`
- Modify: `tests/persistence/test_schema.cpp` (`MigrateIsIdempotentAndRecordsChecksums` list; `EveryAppendOnlyTableHasItsTriggers`; `SchemaIndexes`)
- Modify: `docs/superpowers/specs/2026-10-18-data-publishing-and-hosted-sharing-design.md` (section 4.2 heading and R5: `0007_release.sql`; section 5.7 `software`: `pychron-cpp/<version.txt>` plus `+g<short sha>` only when the build defines `PYCHRON_GIT_SHA`; section 12 "Records": validated structurally, JSON-schema validation when a validator is a dependency)

**Interfaces:**
- Produces: tables `release`, `release_sample`, `release_interpreted_age`, `embargo`, `external_id` exactly as spec 4.2 (less the embargo `CHECK` placeholder, already dropped there); PostgreSQL schema `pub` with tables `pub.analysis_value`, `pub.interpreted_age_value` and views `pub.sample`, `pub.analysis`, `pub.interpreted_age`, `pub.release`, `pub.record` of spec 9.2; function `release_guard()` and trigger allowing updates of `release` only in `state`, `visibility`, `doi`, `doi_state`, `manifest_sha256`, `published_utc`, `withdrawn_utc`, `withdrawn_reason` (`-- @sqlite guard release allow state visibility doi doi_state manifest_sha256 published_utc withdrawn_utc withdrawn_reason`); `forbid_mutation` triggers on `release_sample`, `release_interpreted_age`; indexes `release_state_ix (state, visibility)`, `release_sample_sample_ix (sample_uuid)`, `embargo_scope_ix (scope_type, scope_uuid) WHERE lifted_utc IS NULL`, `external_id_entity_ix (entity_type, entity_uuid)`, `release_name_version_ux UNIQUE (name, version)` (replaces the spec's `name UNIQUE`: one name, several versions; amend spec 4.2).
- `pub.record.search` is a `tsvector` column of a materialized view `pub.record` with GIN index `pub_record_search_gin`; `pub.analysis` joins `pub.analysis_value (release_uuid, analysis_uuid, <numeric columns of analyses.parquet>)`, empty until phase 6.

- [ ] **Step 1: Write the failing schema tests**

In `tests/persistence/test_schema.cpp`:

```cpp
// MigrateIsIdempotentAndRecordsChecksums: ASSERT_EQ(status->size(), 7u);
EXPECT_EQ((*status)[6].version, 7);
EXPECT_EQ((*status)[6].description, "release");
// EveryAppendOnlyTableHasItsTriggers: add "release_sample", "release_interpreted_age" to the list.
TEST_P(SchemaTest, ReleaseRowUpdatesOnlyItsStateColumns)   // UPDATE release SET title = 'x' fails; SET state = 'published' succeeds
TEST_P(SchemaIndexes, EmbargoScopeLookupUsesPartialIndex)   // plan of the UNION of spec 4.2 names embargo_scope_ix, no SCAN
TEST_P(SchemaIndexes, ReleaseSamplesOfASampleUseIndex)      // SELECT release_uuid FROM release_sample WHERE sample_uuid = ? names release_sample_sample_ix
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build --preset dev --target pychron_persistence_tests && build/dev/tests/persistence/pychron_persistence_tests --gtest_filter='Engines/SchemaTest.*:Engines/SchemaIndexes.*'`
Expected: FAIL (`status->size()` is 6; no such table `release`).

- [ ] **Step 3: Write `0007_release.sql`**

The DDL of spec 4.2 with the amendments in Interfaces, then `-- @sqlite skip` blocks: `CREATE SCHEMA pub`, the two `pub.*_value` tables, the views. Each view's `WHERE` is the visibility rule of spec 4.5 written once as a SQL fragment and repeated verbatim (a comment names it `-- visibility rule (spec 4.5)`). `pub.record` is `CREATE MATERIALIZED VIEW` over `UNION ALL` of samples and releases, `search` as `to_tsvector('english', coalesce(title,'') || ' ' || coalesce(description,'') || ' ' || array_to_string(keywords,' '))`, then `CREATE INDEX pub_record_search_gin ON pub.record USING GIN (search)` and `CREATE UNIQUE INDEX pub_record_id_ux ON pub.record (id)` (needed by `REFRESH ... CONCURRENTLY`). Run `python3 tools/ddl_sqlite.py` and commit its output.

- [ ] **Step 4: Run the tests to verify they pass, on both engines**

Run: the Step 2 command, then with `PYCHRON_TEST_PG_URL` set (rebuild first: ctest lists `pg` cases at build time). `python3 tools/ddl_sqlite.py --check` exits 0.
Expected: PASS on `sqlite` and `pg`.

- [ ] **Step 5: Amend the spec** as listed under Files, compressed style.

- [ ] **Step 6: Commit**

```bash
git add libs/persistence/migrations tests/persistence/test_schema.cpp docs/superpowers/specs/2026-10-18-data-publishing-and-hosted-sharing-design.md
git commit -m "feat(persistence): release, embargo and external_id tables with the pub views

BREAKING CHANGE: run elctl db migrate"
```

---

### Task 2: Store reads and writes for releases, embargoes and external ids

**Files:**
- Create: `libs/persistence/include/pychron/persistence/publish.hpp`
- Create: `libs/persistence/src/sql/publish.hpp`, `libs/persistence/src/publish.cpp`
- Modify: `libs/persistence/include/pychron/persistence/store.hpp` (new virtuals), `libs/persistence/src/store.cpp` (the `Store` class declares them; bodies in `publish.cpp` through the same pattern `catalog.cpp` uses)
- Test: `tests/persistence/test_publish.cpp`

**Interfaces:**
- Produces, in `publish.hpp` (namespace `pychron::persistence`):

```cpp
enum class ReleaseState { Draft, Published, Withdrawn };
enum class Visibility { Private, Public };
enum class EmbargoScopeType { Project, Sample, Analysis };
std::string_view to_string(ReleaseState); std::optional<ReleaseState> parse_release_state(std::string_view);
// likewise Visibility ("private"/"public"), EmbargoScopeType ("project"/"sample"/"analysis")

struct ReleaseInsert {
  Uuid uuid;  // caller-generated v7
  std::string name, title; std::optional<std::string> description;
  int version = 1; std::optional<Uuid> supersedes;
  Uuid group, bookmark;
  std::string licence; std::string authors_json; std::optional<std::string> publisher;
  std::optional<UtcTime> embargo_until;
  std::string options_json, reduction_version, software_version;
  std::vector<Uuid> samples;                                   // release_sample rows
  std::vector<std::pair<Uuid, Uuid>> interpreted_ages;        // (ia_uuid, revision_uuid)
};
struct ReleaseRow { /* every column of `release` as optional/plain fields, state and visibility as the enums, authors_json and options_json as text */ };
struct ReleaseUpdate {
  Uuid release;
  std::optional<ReleaseState> state; std::optional<Visibility> visibility;
  std::optional<std::string> doi, doi_state, withdrawn_reason;
  std::optional<Sha256Digest> manifest_sha256;
};
struct ReleaseQuery { std::optional<ReleaseState> state; std::optional<std::string> name; };
struct ReleaseInterpretedAge { Uuid ia, revision; };
struct EmbargoScope { EmbargoScopeType type; Uuid uuid; };
struct EmbargoInsert { EmbargoScope scope; std::optional<UtcTime> until; std::optional<std::string> reason; };
struct EmbargoRow { Uuid uuid; EmbargoScope scope; std::optional<UtcTime> until; std::optional<std::string> reason;
                    Uuid set_by; UtcTime created; std::optional<UtcTime> lifted; std::optional<Uuid> lifted_by; };
struct ExternalIdInsert { std::string entity_type, scheme, value, registrar, state; Uuid entity;
                          Sha256Digest request_sha256; std::optional<std::string> response_json; };
struct ExternalIdRow { /* the columns */ };
```

- Produces, new `IStore` virtuals in `store.hpp` (section "publishing (data publishing spec 4)"):

```cpp
virtual Result<Uuid> add_release(const Actor& actor, const ReleaseInsert& release) = 0;              // one transaction incl. release_sample, release_interpreted_age, change_log kind 'changeset'
virtual Result<std::optional<ReleaseRow>> release(Uuid release) = 0;
virtual Result<std::optional<ReleaseRow>> find_release(const std::string& name, std::optional<int> version) = 0;  // newest version when absent
virtual Result<std::vector<ReleaseRow>> releases(const ReleaseQuery& query) = 0;                     // by name then version
virtual Result<void> update_release(const Actor& actor, const ReleaseUpdate& update) = 0;            // Config on an illegal transition (spec 4.4)
virtual Result<void> delete_release(const Actor& actor, Uuid release) = 0;                           // draft only, else Config
virtual Result<std::vector<Uuid>> release_samples(Uuid release) = 0;
virtual Result<std::vector<ReleaseInterpretedAge>> release_interpreted_ages(Uuid release) = 0;
virtual Result<Uuid> add_embargo(const Actor& actor, const EmbargoInsert& embargo) = 0;
virtual Result<void> lift_embargo(const Actor& actor, Uuid embargo) = 0;                             // Config when already lifted
virtual Result<std::vector<EmbargoRow>> embargoes(bool live_only, UtcTime at) = 0;
virtual Result<std::vector<Uuid>> embargoed_samples(std::span<const Uuid> samples, UtcTime at) = 0;   // those under a live project or sample embargo
virtual Result<std::vector<Uuid>> embargoed_analyses(std::span<const Uuid> analyses, UtcTime at) = 0; // under a live project, sample or analysis embargo
virtual Result<Uuid> add_external_id(const Actor& actor, const ExternalIdInsert& id) = 0;
virtual Result<std::vector<ExternalIdRow>> external_ids(std::string_view entity_type, Uuid entity) = 0;
```

Legal transitions: `Draft -> Published` (sets `published_utc`), `Published -> Withdrawn` (sets `withdrawn_utc`, needs `withdrawn_reason`), `Published` visibility `Private -> Public`; `Public -> Private` only with state `Withdrawn` in the same update; `Draft` visibility change refused; any change to a `Withdrawn` release refused. `embargoed_*` are one statement each over `WHERE x IN (...)` with the three-scope `UNION`, never a query per uuid. Every write ends with a `change_log` row (kind `catalog` for embargo and external_id, `changeset` for releases) and `change_entity` rows (`entity_type` `release`, `embargo`, `external_id`).

- [ ] **Step 1: Write the failing tests** (`tests/persistence/test_publish.cpp`, `TEST_P(PublishStoreTest, ...)` parameterised by engine as `StoreFixture` tests are; fixture ingests two analyses of two samples in one project with the pattern of `tests/processing/test_store_source.cpp` `ingest()`, creates a group and a bookmark):

```cpp
AddReleaseWritesRowSamplesAndInterpretedAges   // release() returns the row with state Draft, visibility Private; release_samples() has both; change_log grew by one
FindReleaseGivesNewestVersion                   // two inserts name "r" v1 and v2; find_release("r", nullopt)->version == 2
DraftToPublishedSetsPublishedUtc
PublishedCannotGoPrivateOrChangeTitle           // update visibility Public->Private without Withdrawn: Config
WithdrawNeedsAReason
DeleteOnlyADraft
EmbargoOnProjectCoversItsSamplesAndAnalyses     // embargoed_samples / embargoed_analyses return them; after lift_embargo, empty
EmbargoExpiresAtUntil                           // until = T; at T - 1 s embargoed, at T not
EmbargoedAnalysesIsOneStatementUsingTheIndex    // plan test moved here from test_schema if simpler: EXPLAIN QUERY PLAN of the statement text in sql/publish.hpp names embargo_scope_ix
ExternalIdUniquePerSchemeAndValue               // second insert of the same (scheme, value): error
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset dev --target pychron_persistence_tests && build/dev/tests/persistence/pychron_persistence_tests --gtest_filter='Engines/PublishStoreTest.*'`
Expected: compile failure (no `publish.hpp`), then FAIL.

- [ ] **Step 3: Implement** the structs, the statements in `src/sql/publish.hpp` (standard SQL; `sql::ts` for times; `Dialect` switch only where engines differ) and the virtuals in `src/publish.cpp`.

- [ ] **Step 4: Run to verify they pass** on `sqlite` and, with `PYCHRON_TEST_PG_URL`, on `pg`.

- [ ] **Step 5: Commit**

```bash
git add libs/persistence tests/persistence/test_publish.cpp
git commit -m "feat(persistence): release, embargo and external id reads and writes"
```

---

### Task 3: Load an analysis as of a bookmark

**Files:**
- Modify: `libs/persistence/include/pychron/persistence/store.hpp`, `libs/persistence/src/store.cpp` (`load_analysis`, `resolve_refs` share their body with the new pair), `libs/persistence/src/refs.cpp`
- Modify: `libs/processing/adapters/store/include/pychron/processing/store_source.hpp`, `libs/processing/adapters/store/src/store_source.cpp`
- Test: `tests/persistence/test_publish.cpp` (new cases), `tests/processing/test_store_source.cpp`

**Interfaces:**
- Produces (`IStore`):

```cpp
// The analysis with its heads replaced by the bookmark's entries for it; nullopt when the
// bookmark has no entry for the analysis. Kinds the bookmark lacks are absent from payloads.
virtual Result<std::optional<AnalysisView>> load_analysis_at(Uuid analysis, Uuid bookmark) = 0;
// resolve_refs, but every ref_object the bookmark pins (kind value) is taken at the pinned revision.
virtual Result<RefResolution> resolve_refs_at(Uuid analysis, Uuid bookmark) = 0;
```

- Produces (`StoreSource`):

```cpp
// load(uuid) as of `bookmark`: heads, payloads and references from its entries. The result is
// not cached and `input_fingerprint` is of the pinned revisions.
Result<AnalysisPtr> load_at(const std::string& uuid, const std::string& bookmark);
```

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/persistence/test_publish.cpp
LoadAnalysisAtBookmarkIgnoresLaterHeadMoves   // bookmark; commit a new intercepts revision; load_analysis_at gives the old revision in payloads, load_analysis the new
ResolveRefsAtBookmarkUsesPinnedFluxRevision   // bookmark pins flux ref; add flux value revision; resolve_refs_at gives the pinned one, resolve_refs the head
// tests/processing/test_store_source.cpp
TEST_F(StoreSourceTest, LoadAtBookmarkReproducesTheOldIntercept)  // intercept value in Analysis::isotopes equals the pre-edit value; heads["intercepts"] is the old revision
```

- [ ] **Step 2: Run to verify they fail** (persistence and processing test binaries, filtered).

- [ ] **Step 3: Implement.** `load_analysis_at` reads `bookmark_entry WHERE bookmark_uuid = ? AND subject_uuid = ?` (one statement) and the payloads of those revisions; `resolve_refs_at` runs `resolve_refs` with `honour_pins = false` then overrides each ref whose object has a bookmark entry. `StoreSource::load_at` is `load` with those two calls in place of `load_analysis` and `resolve_refs`, bypassing the cache.

- [ ] **Step 4: Run to verify they pass.**

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(persistence): load an analysis and its references as of a bookmark"
```

---

### Task 4: `libs/publish` skeleton and the row tables

**Files:**
- Create: `libs/publish/CMakeLists.txt`, `libs/publish/include/pychron/publish/tables.hpp`, `libs/publish/src/tables.cpp`
- Modify: top-level `CMakeLists.txt` (`add_subdirectory(libs/publish)` after processing), `tests/CMakeLists` equivalent where `tests/persistence` is added (`add_subdirectory(tests/publish)`)
- Create: `tests/publish/CMakeLists.txt`, `tests/publish/test_tables.cpp`

**Interfaces:**
- Produces (namespace `pychron::publish`):

```cpp
enum class ColumnType { Double, Int32, Int64, Bool, String, TimestampUtc, Json, Wkb };
struct Column { std::string name; ColumnType type; bool nullable = true; };
using Cell = std::variant<std::monostate, double, std::int32_t, std::int64_t, bool, std::string, UtcTime, JsonText, WkbBytes>;
// JsonText{std::string} and WkbBytes{std::string} are strong wrappers (struct with one `std::string value`).
struct Table { std::string name; std::vector<Column> columns; std::vector<std::vector<Cell>> rows; bool geo = false; };
struct ReleaseTables { Table samples, positions, analyses, isotopes, interpreted_ages; };

// Inputs the tables are made from. `dataset` is reduced and grouped as the report was.
struct SampleRecord { persistence::SampleRow row; std::string project, principal_investigator, material, grainsize; std::optional<Uuid> project_uuid; };
struct PositionRecord { /* identifier, uuids, irradiation, level, position, holder, packet, weight, reactor, chronology start and duration, monitor and J fields, production ratios, the three revision uuids */ };
struct InterpretedAgeRecord { Uuid ia, revision; std::string name; Uuid sample; std::string identifier; persistence::InterpretedAgeValue value; };
struct AnalysisPins { Uuid analysis; std::map<persistence::Kind, Uuid> heads; std::map<Uuid, Uuid> refs; std::optional<Sha256Digest> record_sha256; };
struct TableInputs {
  persistence::ReleaseRow release;
  processing::Dataset dataset; processing::Report report;
  std::vector<SampleRecord> samples; std::vector<PositionRecord> positions;
  std::vector<InterpretedAgeRecord> interpreted_ages; std::vector<AnalysisPins> pins;
};
ReleaseTables build_tables(const TableInputs& in);          // columns and order exactly as spec 5.3
std::string canonical_row_text(const std::vector<Cell>& row); // cells joined by \x1f: doubles shortest round-trip, times ISO-8601 UTC us, nulls empty, Json as is (already canonical), Wkb lowercase hex
Sha256Digest rows_sha256(const Table& table);               // over rows joined by \x1e
std::string wkb_point(double lon, double lat);               // little-endian WKB Point, 21 bytes
std::string file_safe(std::string_view name);                // [^A-Za-z0-9._-] -> '_'
```

- [ ] **Step 1: Write the failing tests** (`tests/publish/test_tables.cpp`, inputs built from `tests/processing/fixtures.hpp` `make_unknown`, `make_step`, `irradiate`, reduced with `processing::reduce`, grouped into a `Dataset`, report by `make_report`):

```cpp
TEST(Tables, AnalysesColumnsMatchTheSpecOrder)        // names and types equal the spec 5.3 list, first "analysis_uuid", last "bbox"
TEST(Tables, RowsAreOrderedByTimestampThenUuid)
TEST(Tables, SampleWithoutLocationHasNullGeometry)    // samples row geometry monostate, bbox monostate; analyses row too
TEST(Tables, AnalysisWithoutAgeHasNullAgeColumns)     // age_ma null, age_note non-empty, samples.n_ages excludes it
TEST(Tables, IsotopeRowsPerAnalysisAndIsotope)        // 5 rows per argon analysis, units "fA"
TEST(Tables, CanonicalTextOfKnownRowIsExact)          // row {1.5, 2, true, "a,b", t, null} == "1.5\x1f" "2\x1f" "true\x1f" "a,b\x1f" "2026-10-18T00:00:00.000000Z\x1f" ""
TEST(Tables, NonFiniteDoubleIsNull)                   // NaN cell canonical text is "" and build_tables writes monostate
TEST(Tables, RowsSha256IsStableAcrossTwoBuilds)
TEST(Tables, WkbPointIsTwentyOneBytesLittleEndian)    // wkb_point(-106.9, 34.0)[0] == 0x01, bytes 1..4 == 01 00 00 00
TEST(Tables, FileSafeReplacesEverythingButWordChars)  // "FC-2 / split (b)" -> "FC-2___split__b_"
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --preset dev && cmake --build --preset dev --target pychron_publish_tests` (configure once for the new directories)
Expected: compile failure.

- [ ] **Step 3: Implement** `libs/publish/CMakeLists.txt` (`pychron_publish` STATIC, alias `pychron::publish`, links `pychron::persistence pychron::processing pychron::processing_store pychron::reduction pychron::core nlohmann_json::nlohmann_json`, `pychron_set_warnings`; built only when `PYCHRON_PERSISTENCE_ENABLED`), `tests/publish/CMakeLists.txt` as described under File structure, and `tables.cpp`. Column values come from `Report` cells where the report already computed them (analyses table of `report.hpp`: find the column by its name) and from `ReducedAnalysis` and the catalog records otherwise; no second reduction.

- [ ] **Step 4: Run to verify they pass.**

- [ ] **Step 5: Commit**

```bash
git add libs/publish tests/publish CMakeLists.txt
git commit -m "feat(publish): row tables of a release with canonical text and row hashes"
```

---

### Task 5: Creating, publishing and withdrawing a release

**Files:**
- Create: `libs/publish/include/pychron/publish/release.hpp`, `libs/publish/src/release.cpp`
- Create: `tests/publish/release_fixture.hpp`, `tests/publish/test_release.cpp`

**Interfaces:**
- Consumes: Task 2 store virtuals; Task 3 `load_analysis_at`; `elctl`'s `Selection` is not visible here, so the library takes uuids.
- Produces:

```cpp
inline constexpr std::string_view kDefaultLicence = "CC-BY-4.0";  // spec 6.3; the owner may change it
struct Author { std::string name; std::optional<std::string> orcid, affiliation; };
struct ReleaseSpec {
  std::string name, title; std::optional<std::string> description;
  std::vector<Author> authors; std::string licence{kDefaultLicence}; std::optional<std::string> publisher;
  std::optional<UtcTime> embargo_until; std::optional<std::string> supersedes;   // a name or a uuid text
  std::vector<std::string> analyses;          // candidate uuids (the command's selection, already filtered by type and --include-invalid)
  std::vector<std::string> tags;              // head tag names kept; empty: every tag but "invalid"
  std::optional<std::vector<std::string>> interpreted_ages;  // names; nullopt: every IA whose members are all in the release; empty vector: none
  std::string options_json, reduction_version, software_version;
};
struct ReleaseCreated { Uuid release; int version; std::size_t members, samples, interpreted_ages, embargoed_samples, embargoed_analyses; };
Result<ReleaseCreated> create_release(persistence::IStore& store, const persistence::Actor& actor, const ReleaseSpec& spec);
Result<void> publish_release(persistence::IStore&, const persistence::Actor&, Uuid release, const Sha256Digest& manifest_sha256, bool make_public);
Result<void> withdraw_release(persistence::IStore&, const persistence::Actor&, Uuid release, std::string reason);
Result<void> set_release_public(persistence::IStore&, const persistence::Actor&, Uuid release);
Result<void> delete_release(persistence::IStore&, const persistence::Actor&, Uuid release);
// "name", "name@v2" or a uuid text -> the row; Config when none.
Result<persistence::ReleaseRow> find_release(persistence::IStore&, std::string_view name_or_uuid);
```

`create_release` does spec 4.3 steps 1 to 5: filter candidates by head tag (one `heads()` read per analysis is acceptable here, members number at most the selection's `--limit`), `resolve_refs` per member, `create_group(name = "release:<name>@v<n>")`, `create_bookmark({name, message "release <name> v<n>", group})` whose entries are every member head of every kind plus every resolved ref at its revision plus every chosen IA at its head (bookmark creation takes the heads as they are: a `BookmarkSpec` has no explicit entry list today, so extend `BookmarkSpec` with `std::optional<std::vector<HeadInfo>> entries` in Task 5 and have `create_bookmark` write those when given; test `BookmarkWithExplicitEntriesWritesThem` in `tests/persistence/test_collections.cpp`), then `add_release`. Version = 1 + newest version of the name (0 when new); `supersedes` resolves to that row's uuid.

- [ ] **Step 1: Write the failing tests** (`release_fixture.hpp`: a `TestDatabase`, a reduction actor, `tests/processing/flux_seed.hpp` `seed_flux_level` for a level with flux, two unknowns of two samples in one project and one of a sample in another, ingested with `signals_state` complete; exposes `store()`, `actor()`, `analyses()` uuids, `samples()`):

```cpp
TEST_P(ReleaseTest, CreatePinsEveryHeadAndRef)                       // bookmark_heads has 6 kinds per member + flux/production/chronology refs
TEST_P(ReleaseTest, TagFilterLeavesOutInvalidByDefault)               // tag one "invalid": members == 2
TEST_P(ReleaseTest, RetagAfterCreationDoesNotChangeMembersOrPins)     // Review Focus 5
TEST_P(ReleaseTest, VersionBumpsPerName)                              // second create of "r": version 2, supersedes == first uuid
TEST_P(ReleaseTest, EmbargoedCountsAreReported)                       // embargo one sample: embargoed_samples == 1, embargoed_analyses == 1, members unchanged
TEST_P(ReleaseTest, PublishThenWithdraw)                              // states and timestamps; withdraw without reason: Config
TEST_P(ReleaseTest, FindReleaseByNameVersionAndUuid)
```

- [ ] **Step 2: Run to verify they fail.** Run: `build/dev/tests/publish/pychron_publish_tests --gtest_filter='Engines/ReleaseTest.*'`

- [ ] **Step 3: Implement** `release.cpp` and the `BookmarkSpec::entries` extension in persistence (with its test).

- [ ] **Step 4: Run to verify they pass** (publish and persistence binaries).

- [ ] **Step 5: Commit**

```bash
git add libs/publish libs/persistence tests/publish tests/persistence/test_collections.cpp
git commit -m "feat(publish): create, publish and withdraw a release pinned by a bookmark"
```

---

### Task 6: Manifest, records and the build of a release directory (no Parquet yet)

**Files:**
- Create: `libs/publish/include/pychron/publish/manifest.hpp`, `src/manifest.cpp`, `include/pychron/publish/records.hpp`, `src/records.cpp`, `include/pychron/publish/build.hpp`, `src/build.cpp`, `include/pychron/publish/parquet.hpp` (interface only; Task 7 implements), `libs/publish/licences/CC-BY-4.0.txt`, `CC0-1.0.txt`, `CC-BY-SA-4.0.txt`
- Modify: `libs/publish/CMakeLists.txt` (licence texts installed beside the program as `libs/earthbank/templates` are: `cmake/PychronInstall.cmake`; `PYCHRON_PUBLISH_SOURCE_LICENCE_DIR` for an uninstalled build)
- Test: `tests/publish/test_manifest.cpp`, `test_records.cpp`, `test_build.cpp`

**Interfaces:**
- Produces:

```cpp
// manifest.hpp
struct ManifestFile { std::string path; std::uint64_t bytes; Sha256Digest sha256; std::string media_type; };
struct ManifestTable { std::string path; std::uint64_t rows; Sha256Digest rows_sha256; std::vector<std::string> columns; };
struct Manifest { /* release, build, pins, tables, files of spec 5.7 as nested structs; format == "pychron-release/1" */ };
std::string manifest_json(const Manifest&);                       // keys in spec order, 2-space indent, "\n" line ends
Result<Manifest> parse_manifest(std::string_view json);
Result<Manifest> read_manifest(const std::filesystem::path& dir);
struct VerifyReport { std::vector<std::string> missing, changed, extra; std::optional<std::string> reduction_version_differs; bool ok() const; };
Result<VerifyReport> verify_files(const std::filesystem::path& dir);                     // files[] against disk
Result<VerifyReport> verify_rebuild(const std::filesystem::path& dir, const Manifest& rebuilt);  // rows_sha256 per table and sha per non-Parquet file

// records.hpp
std::string catalog_json(const Manifest&, const ReleaseTables&);                 // spec 5.4
std::string sample_record_json(const Manifest&, const ReleaseTables&, const Uuid& sample);
std::string release_record_json(const Manifest&, const ReleaseTables&);

// parquet.hpp
class IParquetWriter { public: virtual ~IParquetWriter() = default;
  virtual Result<void> write(const Table& table, const std::filesystem::path& path) = 0; };
std::unique_ptr<IParquetWriter> make_parquet_writer();   // nullptr when built without Arrow (Task 7)

// build.hpp
struct BuildOptions { std::filesystem::path out; bool private_build = false; bool with_figures = false; bool with_viewer = false; };
class IProgress { public: virtual ~IProgress() = default; virtual void stage(std::string_view name) = 0; };
struct BuildResult { Manifest manifest; std::filesystem::path dir; bool parquet_written; std::vector<std::string> warnings; };
Result<BuildResult> build_release(persistence::IStore& store, processing::StoreSource& source, Uuid release,
                                  const BuildOptions& options, IProgress* progress);
```

`build_release`: loads members with `source.load_at`, reduces and groups with the options in `release.options_json` (same path `elctl export` takes: `ReportOptions`, grouping), drops embargoed samples and analyses unless `private_build` (counts into `build.omitted_embargoed`), builds tables (Task 4), writes `tables/release.{csv,json}`, `tables/<sample>/report.{csv,json}`, `constants.json` (the report's constants table as `{name: {value, error, units}}`), `LICENSE.txt` (the licence text by SPDX id; unknown id: a one-line file naming the id and a warning), `CITATION.cff` (`cff-version: 1.2.0`, `title`, `authors` with `name` or `family-names`/`given-names` split on the last space, `orcid` as `https://orcid.org/<id>`, `version`, `date-released`, `doi` when any, `license`), `README.md` (one paragraph: title, release name and version, counts, how to cite, how to read: "serve the directory; see docs/release_format.md"), `data/*.parquet` through `make_parquet_writer()` when not null (else `warnings` says "built without Parquet support"), `records/*.json`, `catalog.json`, then `manifest.json` last. The output directory is `<out>/<name>-v<version>`; an existing non-empty one is an error (`Config`). Every file is passed to `mark_as_user_file` after close. `with_figures` and `with_viewer` are accepted and ignored with a warning in this phase.

- [ ] **Step 1: Write the failing tests**

```cpp
// test_manifest.cpp
TEST(Manifest, RoundTripsThroughJson)                      // parse_manifest(manifest_json(m)) == m
TEST(Manifest, JsonKeysAreInSpecOrderAndDeterministic)     // two calls equal; first key "format"
TEST(Manifest, VerifyFilesNamesMissingAndChanged)          // write dir from a manifest; delete one file, change one byte of another
TEST(Manifest, VerifyRebuildReportsReductionVersionOnly)   // rebuilt.reduction_version differs: report has only that, ok() false
// test_records.cpp
TEST(Records, CatalogHasItemLinkPerSampleAndRelease)       // links with rel "item" == samples + 1; type "Collection"; itemType "record"
TEST(Records, CatalogExtentSkipsSamplesWithoutLocation)    // Review Focus 1
TEST(Records, SampleRecordCarriesIgsnAsExternalId)         // externalIds [{scheme "igsn", value}]
TEST(Records, ReleaseRecordLinksEveryEnclosure)
// test_build.cpp (Engines/BuildTest over release_fixture.hpp)
TEST_P(BuildTest, WritesEveryFileInTheManifestWithMatchingSha)   // verify_files(dir).ok()
TEST_P(BuildTest, TwoBuildsGiveEqualRowHashesAndEqualNonParquetBytes)
TEST_P(BuildTest, PublicBuildOmitsEmbargoedSample)              // Review Focus 2: no records/<uuid>.json, counts in manifest, private build has it and private_build true
TEST_P(BuildTest, SampleNamesAreMadeFileSafeAndUnique)          // Review Focus 4: two samples "A/B" in two projects -> tables/A_B-<8>/ and tables/A_B-<8>/ differ
TEST_P(BuildTest, RefusesANonEmptyTargetDirectory)
TEST_P(BuildTest, EveryItemLinkInCatalogResolvesToAFile)
TEST_P(BuildTest, OutputIsNotQuarantined)                       // on macOS: xattr absent; elsewhere passes
```

- [ ] **Step 2: Run to verify they fail.**

- [ ] **Step 3: Implement** the four sources and the licence files (the texts verbatim from creativecommons.org legalcode, plain text).

- [ ] **Step 4: Run to verify they pass.** Run: `build/dev/tests/publish/pychron_publish_tests --gtest_filter='Manifest.*:Records.*:Engines/BuildTest.*'`

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(publish): build a release directory with manifest, records and Schaen tables"
```

---

### Task 7: GeoParquet tables with Apache Arrow behind `PYCHRON_PUBLISH`

**Files:**
- Modify: `vcpkg.json` (feature `publish`: `{"name": "arrow", "default-features": false, "features": ["parquet"]}` plus `zstd`), `cmake/PychronDependencies.cmake` (`PYCHRON_PUBLISH` `AUTO|ON|OFF`, default `AUTO`, `find_package(Arrow CONFIG)` and `find_package(Parquet CONFIG)`, `PYCHRON_PUBLISH_ARROW_ENABLED`, messages as `PYCHRON_VISION_OPENCV`), `libs/publish/CMakeLists.txt` (compile `parquet_writer.cpp` with `SYSTEM` includes and `PYCHRON_PUBLISH_ARROW_ENABLED` when found, else `parquet_stub.cpp`), `CMakePresets.json` (`dev` and CI presets pass the `publish` vcpkg feature where they pass `ui`), `.github/workflows/ci.yml` (every job builds it; `docs/dev_setup.md` gets a line), `AGENTS.md` build section (one line: `-DPYCHRON_PUBLISH=AUTO|ON|OFF`, Arrow only in `parquet_writer.cpp`)
- Create: `libs/publish/src/parquet_writer.cpp`, `libs/publish/src/parquet_stub.cpp`, `tests/publish/test_parquet.cpp`, `tests/publish/fixtures/release-fixture.manifest.json`, `tests/publish/fixtures/README.md`

**Interfaces:**
- Consumes: `Table`, `IParquetWriter` (Tasks 4, 6).
- Produces: `make_parquet_writer()` returns the Arrow writer when built with it. The writer maps `ColumnType` to Arrow types (`Double` float64, `Int32`, `Int64`, `Bool`, `String` utf8, `TimestampUtc` timestamp(us, "UTC"), `Json` utf8, `Wkb` binary), adds a `bbox` struct column `{xmin, ymin, xmax, ymax}` float64 when `table.geo`, ZSTD, one row group, file metadata `geo` exactly as spec 5.2 (`version "1.1.0"`, `primary_column "geometry"`, `columns.geometry {encoding "WKB", geometry_types ["Point"], crs <PROJJSON of OGC:CRS84>, bbox [...], covering {bbox {xmin ["bbox","xmin"], ...}}}`), `created_by` left to Arrow.

- [ ] **Step 1: Write the failing tests**

```cpp
// test_parquet.cpp: skipped (GTEST_SKIP) when make_parquet_writer() is null, except the last
TEST(Parquet, ReadsBackColumnsAndTypes)                // Arrow reader (test-only dependency): schema names/types equal the Table's; row count equal
TEST(Parquet, GeoMetadataIsGeoParquet11)                // key "geo" parses; version "1.1.0"; encoding "WKB"; covering names the bbox struct
TEST(Parquet, NullsStayNull)
TEST(Parquet, SameTableTwiceGivesSameBytes)
TEST(Parquet, WithoutArrowBuildStillSucceedsAndWarns)   // only when null: build_release of the fixture: parquet_written false, warning names it, no data/ directory
// test_build.cpp
TEST_P(BuildTest, RowHashesEqualTheCommittedFixture)    // fixtures/release-fixture.manifest.json tables[].rows_sha256 equal the build's (the fixture is written on macOS; this is what Windows and Linux CI compare)
```

- [ ] **Step 2: Run to verify they fail** (after `cmake --preset dev`: first configure fetches Arrow; expect minutes).

- [ ] **Step 3: Implement** CMake and vcpkg wiring, `parquet_writer.cpp`, `parquet_stub.cpp`, generate the fixture with the test's own `--write-fixture` path (a `PYCHRON_WRITE_FIXTURE=1` environment check in the test that rewrites the file, documented in `fixtures/README.md`), commit the fixture.

- [ ] **Step 4: Run to verify they pass** with Arrow found and with `-DPYCHRON_PUBLISH=OFF` (a second build directory, `build/dev-nopublish`, configured by hand; both run `pychron_publish_tests`).

- [ ] **Step 5: Commit**

```bash
git add vcpkg.json cmake CMakePresets.json .github libs/publish tests/publish AGENTS.md docs/dev_setup.md
git commit -m "feat(publish): GeoParquet tables through Apache Arrow behind PYCHRON_PUBLISH"
```

---

### Task 8: Embargo helpers in `libs/publish`

**Files:**
- Create: `libs/publish/include/pychron/publish/embargo.hpp`, `src/embargo.cpp`
- Test: `tests/publish/test_embargo.cpp`

**Interfaces:**
- Produces:

```cpp
struct EmbargoRequest { persistence::EmbargoScopeType type; std::string name_or_uuid;   // project name, sample name ("name" or "name@project"), analysis uuid or runid
                        std::optional<UtcTime> until; std::optional<std::string> reason; };
Result<Uuid> set_embargo(persistence::IStore&, const persistence::Actor&, const EmbargoRequest&);    // Config naming the ambiguity when a sample name matches several projects
Result<void> lift_embargo(persistence::IStore&, const persistence::Actor&, std::string_view embargo_uuid);
struct EmbargoListing { persistence::EmbargoRow row; std::string scope_name; };                     // "project Alpha", "sample FC-2 (Alpha)", "runid 77000-01"
Result<std::vector<EmbargoListing>> list_embargoes(persistence::IStore&, bool live_only, UtcTime at);
```

- [ ] **Step 1: Write the failing tests**: `SetBySampleNameRefusesAmbiguity`, `SetByRunidResolvesTheAnalysis`, `ListNamesTheScope`, `LiftTwiceIsConfig`.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement** (lookups by `find_catalog_row`, `samples({.text})` is not used: name lookups go through `find_catalog_row(CatalogTable::Sample, {name, project, material})` over the projects that have the name; runid through `find_analysis`).
- [ ] **Step 4: Run to verify they pass.**
- [ ] **Step 5: Commit** `feat(publish): set, lift and list embargoes by name`.

---

### Task 9: `elctl publish`

**Files:**
- Create: `apps/elctl/src/publish.hpp`, `publish.cpp`, `publish_stub.cpp`
- Modify: `apps/elctl/src/cli.cpp` (dispatch `publish`; summary lines under "Publication data"), `apps/elctl/CMakeLists.txt` (stub split as `export`; link `pychron::publish`; `PYCHRON_ELCTL_VERSION` already defined feeds `software_version`)
- Test: `tests/publish/test_elctl_publish.cpp`

**Interfaces:**
- Consumes: `Selection`, `AgeFlags`, `load_selection`, `open_source` (`selection.hpp`); Tasks 5, 6, 7.
- Produces: `int publish_command(const std::vector<std::string>& args, Io io)` with the subcommands and flags of spec 6.2 for this phase: `create`, `build`, `verify`, `publish`, `withdraw`, `list`, `show`, `delete`, `help`. Not in this phase (say "not yet; phase 7" and exit 2): `doi`, `igsn`, `upload`. `--with-figures` and `--with-viewer` accepted, warn. `--author "Name|orcid|affiliation"` splits on `|`. `--licence` default `kDefaultLicence`. `options_json` is the `ReportOptions` and `AgeFlags` and `--group-by` as JSON (one function `options_json(const AgeFlags&, int sigma, std::string_view group_by, std::string_view lab, notes)` in `publish.cpp`, and `options_from_json` for `build`). Exit codes: 0; 1 nothing matched or verify found a difference; 2 usage or fatal. `publish` reads `<dir>/manifest.json`, checks `pins.bookmark_uuid` equals the store's and every pinned revision is still the bookmark's, records `manifest_sha256`.

- [ ] **Step 1: Write the failing tests** (`ElctlPublishTest` over `release_fixture.hpp` with a temp-file SQLite url; drive `elctl::run({"publish", ...}, io)` with string streams):

```cpp
CreatePrintsUuidAndCounts                   // out contains "release " + uuid, "members 3", "samples 3"
CreateWithUnknownLicenceStillCreatesAndWarns
BuildWritesDirectoryAndVerifyPasses         // exit 0 both; dir/<name>-v1/manifest.json exists
VerifyFailsOnAChangedCell                   // edit tables/release.csv: exit 1, err names "tables/release.csv"
PublishRequiresABuildOfTheSamePins          // manifest of another release: exit 2
PublishKeepsPrivateUnlessPublicFlag
ListAndShow                                 // list shows name, v1, draft, private; show prints the counts and the bookmark uuid
WithoutDbIsUsage                            // exit 2, err starts "elctl publish: --db"
```

- [ ] **Step 2: Run to verify they fail.** Run: `build/dev/tests/publish/pychron_publish_tests --gtest_filter='ElctlPublishTest.*'`
- [ ] **Step 3: Implement** `publish.cpp` (usage text in the style of `export.cpp`), the stub, dispatch and CMake.
- [ ] **Step 4: Run to verify they pass**; also `elctl publish help` by hand prints the usage and exits 0.
- [ ] **Step 5: Commit** `feat(elctl): publish create, build, verify, publish, withdraw, list, show`.

---

### Task 10: `elctl embargo`

**Files:**
- Create: `apps/elctl/src/embargo.hpp`, `embargo.cpp`, `embargo_stub.cpp`
- Modify: `apps/elctl/src/cli.cpp`, `apps/elctl/CMakeLists.txt`
- Test: `tests/publish/test_elctl_embargo.cpp`

**Interfaces:**
- Consumes: Task 8.
- Produces: `int embargo_command(const std::vector<std::string>& args, Io io)`: `set --db <url> (--project P | --sample S | --analysis U)... [--until YYYY-MM-DD] [--reason t]`, `lift --db <url> --id <uuid>` or the same scope flags (lifts every live embargo on them), `list --db <url> [--live]`, `help`. `--until` is midnight UTC of the day after (inclusive date). Output of `list`: one line per embargo: uuid, scope name, until or `indefinite`, reason, `lifted <date>` when lifted.

- [ ] **Step 1: Write the failing tests**: `SetProjectThenListShowsIt`, `LiftByScopeLiftsEveryLiveOne`, `UntilIsInclusiveDate` (list shows `2026-12-31`, store has `2027-01-01T00:00:00Z`), `AmbiguousSampleNameIsAnError` (exit 2, err names both projects).
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run to verify they pass.**
- [ ] **Step 5: Commit** `feat(elctl): embargo set, lift and list`.

---

### Task 11: Documentation

**Files:**
- Create: `docs/release_format.md` (every column of every table with unit and source, the directory layout, manifest fields, records fields, how to verify, how to serve; written from `tables.cpp` and `records.cpp`, not from memory)
- Modify: `docs/user/08-elctl-reference.md` (contents entries and sections `publish` and `embargo` under "Publication data"; `help` output pasted), `docs/export.md` (one paragraph pointing at releases), `docs/superpowers/specs/2026-10-18-data-publishing-and-hosted-sharing-design.md` (section 13 phase 1: done items ticked with the date; any departure made in Tasks 1 to 10 recorded in the section it touches)

- [ ] **Step 1: Write the pages.**
- [ ] **Step 2: Check every column name in `docs/release_format.md` against `build_tables`**: a small test `TEST(Tables, EveryColumnIsDocumented)` in `test_tables.cpp` reads `docs/release_format.md` (path from `PYCHRON_SOURCE_DIR` compile definition of the test target) and requires each column name of each table to appear in a backticked span. Run it; expected PASS.
- [ ] **Step 3: Commit** `docs(publish): release format and elctl publish reference`.

---

### Task 12: Timing, landing

**Files:** none new; the commit message of the measurement.

- [ ] **Step 1: Measure the build** on a lab-sized store copy (`libs/persistence/AGENTS.md` "How to know"): `elctl publish create` and `elctl publish build` at 24, 400 and 2000 analyses, wall time each, and `PYCHRON_BENCH_DB=... build/dev/tests/processing/pychron_processing_store_tests --gtest_filter='StoreLoadTiming.*'` before and after this branch to show loading did not slow. Numbers go in the landing commit message (or in a `chore(publish): timing` empty commit). A build that grows faster than the count is a per-row query: fix it before landing.
- [ ] **Step 2: Rebase on `origin/develop`.**
- [ ] **Step 3: Static analysis**: `python3 tools/quality_check.py` exits 0; `python3 tools/compiler_sweep.py` reports nothing.
- [ ] **Step 4: Full build and tests**: `cmake --build --preset dev` and `ctest --preset dev` (not `dev-quick`); with `PYCHRON_TEST_PG_URL` set, rebuild and run `ctest --preset dev -R 'Engines/'` on PostgreSQL as well (a migration and new queries landed). Then once with `-DPYCHRON_PUBLISH=OFF` in a second build directory: `pychron_publish_tests` passes with the Arrow cases skipped.
- [ ] **Step 5: Sanitizers**: a `-DPYCHRON_SANITIZE=address,undefined` build of `pychron_publish_tests` and `pychron_persistence_tests` runs clean.
- [ ] **Step 6: Merge into `develop`, push** (`AGENTS.md` Workflow); if `origin/develop` moved, rebase and rerun Steps 3 and 4.

---

## Self-review notes

- Spec coverage of phase 1 (section 13 item 1): migration (T1), `release*`, `embargo`, `external_id` (T1, T2), `pub.*` views (T1), `load_analysis_at` (T3), `libs/publish` core `release.hpp` (T5), `embargo.hpp` (T8), `tables.hpp` (T4), `manifest.hpp`, `records.hpp` (T6), `elctl publish create|build|verify|publish|withdraw|list|show` (T9), `elctl embargo` (T10), Schaen tables, records, constants, CITATION, README, manifest (T6), Parquet behind `PYCHRON_PUBLISH` with Arrow from vcpkg (T7), `docs/release_format.md` (T11). `registrar.hpp`, `earthbank.hpp`, figures and viewer are later phases and not here.
- Departures from the spec recorded in Task 1 and Task 11: migration number 0007, `UNIQUE (name, version)`, software version without a git sha, structural record checks, `BookmarkSpec::entries`.
- Names used across tasks: `Table`, `ReleaseTables`, `TableInputs`, `build_tables`, `rows_sha256` (T4) used by T6 and T7; `IParquetWriter`, `make_parquet_writer` (T6 interface, T7 body); `ReleaseSpec`, `create_release`, `find_release` (T5) used by T9; `EmbargoRequest`, `set_embargo`, `list_embargoes` (T8) used by T10; `ReleaseInsert`, `ReleaseUpdate`, `embargoed_samples`, `embargoed_analyses`, `load_analysis_at` (T2, T3) used by T5 and T6.

## Open questions

None.
