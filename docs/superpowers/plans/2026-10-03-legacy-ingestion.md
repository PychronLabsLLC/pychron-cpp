# Legacy Ingestion (core + pychron DVC adapter) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Import legacy pychron DVC data (catalog dump, MetaData repo, project repos with full git history) into the SQL store through a source-agnostic ingest core, driven by `elctl import`.

**Architecture:** `libs/ingest` owns id derivation, batch writing, provenance, conflicts, resume and verify; it sees sources only through `ISourceAdapter`, which yields neutral `ImportBatch` values keyed by source keys, never uuids. `libs/dvc` holds three adapters (catalog, meta repo, project repo) that read git through one-shot `git` subprocesses and parse legacy JSON with nlohmann_json. `libs/persistence` gains the import write and read surface.

**Tech Stack:** C++20, CMake, GoogleTest, TinyORM/QtSql (inside persistence only), nlohmann_json 3.11.3 (new), `git` CLI at import time, Python 3 standard library for the dump converter.

**Spec:** `docs/superpowers/specs/2026-10-03-legacy-ingestion-design.md`. Background: `docs/superpowers/specs/2026-10-01-dvc-schema-design.md` sections 10.2, 10.3.

## Global Constraints

- Qt appears only inside `libs/persistence/src`. Public headers of `persistence`, `ingest` and `dvc` are std-only (nlohmann_json is PRIVATE to `dvc`).
- `libs/ingest`, `libs/dvc`, their tests and the `elctl import` command build only when `PYCHRON_PERSISTENCE_ENABLED`. With it off, `elctl import` prints `elctl was built without persistence` and returns `kUsage` (2).
- Fallible functions return `pychron::Result<>`. No exception crosses a library boundary.
- Never edit `migrations/pg/0001_init.sql`. New DDL goes in `0002_import_detail.sql`; run `python3 tools/ddl_sqlite.py` and commit the generated SQLite file.
- Store tests are `TEST_P` over `StoreTest` (`tests/persistence/store_fixture.hpp`) so they run on SQLite and, with `PYCHRON_TEST_PG_URL`, PostgreSQL.
- Tests that need `git` call `GTEST_SKIP() << "git not found"` when it is absent. Fixture commits set `GIT_AUTHOR_NAME`, `GIT_AUTHOR_EMAIL`, `GIT_AUTHOR_DATE`, `GIT_COMMITTER_*` through `ProcessSpec::env` so shas are stable.
- In tests, declare fakes before the object that holds references to them (AGENTS.md lifetime rules).
- Build and test: `cmake --preset dev && cmake --build --preset dev && ctest --preset dev -R <regex>`. All tests pass under ASan/UBSan. A failure on one compiler only is a real bug.
- Commit after each task on branch `claude/legacy-data-ingestion-7c6a1b`. No pull requests. Merge to `main` only after the last task (rebase on `origin/main`, run all tests, push).
- Import namespace uuid (fixed forever): `6f0e4c1a-9d7b-5c2e-8a41-70796368726e`.
- Default batch size 500 commits; blob cache limit 256 MB; verify default relative tolerance `1e-9`.

## Review Focus

1. **Python-written JSON is not strict JSON.** Legacy files contain bare `NaN`, `Infinity`, `-Infinity`. A person expects those analyses imported with the value as NaN/null, not thousands of `unparseable` conflicts. Pinned in Task 9.
2. **Paths git quotes.** File names with spaces, non-ASCII or quotes are C-quoted by `diff-tree` unless `-z` is used. Expected: imported like any other file. Pinned in Task 10.
3. **Large or slow git output.** A `.data` batch can exceed 64 KiB and 60 s. Expected: no truncation, no timeout kill on a healthy import. Pinned in Tasks 6 and 10.
4. **Unusable source.** Empty repo, missing branch, shallow clone, path that is not a repo. Expected: `import add`/`run` exits 2 with one clear line; nothing written. Pinned in Tasks 10 and 15.
5. **Analysis identity trouble.** Analysis file with no uuid, or the same runid with two different uuids in one repo. Expected: the first gets a derived uuid with a provenance note; the second is an `identity_clash` conflict, and the import continues. Pinned in Task 11.

---

## File Structure

```
libs/core/include/pychron/core/sha1.hpp, src/sha1.cpp        SHA-1 (for UUIDv5)
libs/core/.../process.hpp, src/process_*.cpp                 + ProcessSpec::stdout_file
libs/persistence/include/pychron/persistence/ids.hpp         + Uuid::v5
libs/persistence/include/pychron/persistence/import.hpp      import structs, IImportUnitOfWork
libs/persistence/include/pychron/persistence/store.hpp       + IStore import methods, wider SampleSpec, loads
libs/persistence/src/import.cpp                              implementation
libs/persistence/migrations/pg/0002_import_detail.sql        + sqlite twin (generated)
libs/ingest/include/pychron/ingest/{ids,batch,adapter,writer,tz,verify}.hpp
libs/ingest/src/{ids,writer,tz,verify}.cpp
libs/dvc/include/pychron/dvc/{git_reader,legacy_layout,project_adapter,meta_adapter,catalog_adapter}.hpp
libs/dvc/src/{git_reader,legacy_layout,legacy_json,project_adapter,meta_adapter,catalog_adapter}.cpp
apps/elctl/src/import.{hpp,cpp}, import_stub.cpp
tools/legacy_dump_to_jsonl.py, tools/import_fixture_check.sh
tests/ingest/, tests/dvc/ (+ tests/dvc/fixtures/), tools/tests/
```

`PYCHRON_LIBS` in the root `CMakeLists.txt` becomes `core persistence ingest dvc transport ...` (ingest and dvc right after persistence).

---

### Task 1: Legacy layout survey and fixtures

No product code. Its outputs are the authority for Tasks 9, 11, 12, 13.

**Files:**
- Create: `tests/dvc/fixtures/project/` (one unknown analysis and one blank analysis: every file legacy pychron wrote for each, copied verbatim from a public NMGRLData project repo), `tests/dvc/fixtures/meta/` (one irradiation directory with one level, productions, chronology; one gains file; sensitivity file; one irradiation holder; from `github.com/NMGRLData/MetaData`), `tests/dvc/fixtures/ia/` (one interpreted-age file), `tests/dvc/fixtures/README.md` (source repo url, commit sha and original path of every file)
- Modify: `docs/superpowers/specs/2026-10-01-dvc-schema-design.md` (sections 10.3, 12.1, 13.3 per ingestion spec section 9), `docs/superpowers/plans/2026-10-02-dvc-persistence.md` (stage D4 points at this plan)

**Interfaces:**
- Produces: fixture files; `README.md` containing (a) the path→kind table with real paths, (b) the chosen real-data fixture repo and sha, (c) where per-analysis ages live in interpreted-age files, (d) the legacy MySQL table and column names the catalog needs.

- [ ] **Step 1:** Clone `https://github.com/NMGRL/pychron` and `https://github.com/NMGRLData/MetaData` into the scratch directory (not the repo). Read `pychron/dvc/__init__.py` (`analysis_path`, the `<TAG>` constants), `pychron/dvc/dvc_persister.py` (collection commits), `pychron/dvc/dvc.py` (interpreted ages, transfers), `pychron/dvc/meta_repo.py`, `pychron/dvc/dvc_orm.py` (MySQL tables).
- [ ] **Step 2:** Pick the smallest public NMGRLData project repo that has at least one refit commit (`<ISOEVO>` after collection), one blank change and one interpreted age. Record url and head sha.
- [ ] **Step 3:** Copy the fixture files and write `README.md`. Expected layout to confirm or correct (from memory of `analysis_path`, the fixture is the authority): `<runid[:3]>/<runid[3:]>.json`; `<runid[:3]>/<modifier>/<runid[3:]>.<modifier[:4]>.json` for modifiers `intercepts`, `baselines`, `blanks`, `icfactors`, `tags`, `extraction`, `monitor`, `peakcenter`, `cosmogenic`; raw data under `.data/` with suffix `.data.json`.
- [ ] **Step 4:** Edit the two existing docs. In 13.3 mark Q3, Q4, Q5 resolved and fill Q1 with the repo and sha.
- [ ] **Step 5:** If the survey contradicts the ingestion spec (for example, no per-analysis ages in interpreted-age files), stop and report; do not continue to Task 2.
- [ ] **Step 6:** Commit: `Ingest: legacy DVC layout survey and fixtures`.

### Task 2: SHA-1 and `Uuid::v5`

**Files:**
- Create: `libs/core/include/pychron/core/sha1.hpp`, `libs/core/src/sha1.cpp`, `tests/core/test_sha1.cpp`, `tests/persistence/test_uuid_v5.cpp`
- Modify: `libs/core/CMakeLists.txt`, `libs/persistence/include/pychron/persistence/ids.hpp`, `libs/persistence/src/ids.cpp`

**Interfaces:**
- Produces: `std::array<std::uint8_t, 20> pychron::sha1(std::string_view bytes);` and `static Uuid Uuid::v5(const Uuid& ns, std::string_view name);`

- [ ] **Step 1: Failing tests.** `Sha1.KnownVectors`: `sha1("")` hex = `da39a3ee5e6b4b0d3255bfef95601890afd80709`; `sha1("abc")` = `a9993e364706816aba3e25717850c26c9cd0d89d`. `UuidV5.DnsVector`: `Uuid::v5(*Uuid::parse("6ba7b810-9dad-11d1-80b4-00c04fd430c8"), "www.example.com").str() == "2ed6657d-e927-568b-95e1-2665a8aea6a2"`. `UuidV5.VersionAndDeterminism`: `version() == 5`, two calls equal, different names differ.
- [ ] **Step 2:** Run `ctest --preset dev -R "Sha1|UuidV5"`. Expected: build failure, symbols undefined.
- [ ] **Step 3:** Implement. v5 = SHA-1 over namespace bytes then name, first 16 bytes, version nibble 5, variant bits `10`.
- [ ] **Step 4:** Run the same command. Expected: PASS.
- [ ] **Step 5:** Commit: `core, persistence: SHA-1 and UUIDv5`.

### Task 3: Migration and import source, provenance, conflict store API

**Files:**
- Create: `libs/persistence/migrations/pg/0002_import_detail.sql`, `libs/persistence/migrations/sqlite/0002_import_detail.sql` (generated), `libs/persistence/include/pychron/persistence/import.hpp`, `libs/persistence/src/import.cpp`, `tests/persistence/test_import_store.cpp`
- Modify: `libs/persistence/include/pychron/persistence/store.hpp`, `libs/persistence/src/store_impl.hpp`, `libs/persistence/src/store.cpp`, `libs/persistence/CMakeLists.txt`

**Interfaces:**
- Produces (namespace `pychron::persistence`, in `import.hpp`):

```cpp
enum class ImportSourceKind { ProjectRepo, MetaRepo, LegacyDb };   // 'project_repo' | 'meta_repo' | 'legacy_db'
enum class ConflictKind { HandEdit, UnknownAnalysis, ValueMismatch, Unparseable, IdentityClash, ProvisionalRenumber };

struct ImportSourceSpec {
  Uuid uuid;                       // caller-derived (Task 7)
  ImportSourceKind kind;
  std::string url_or_path;         // already normalized
  std::optional<std::string> branch;
  std::string importer_version;
  std::string lab_time_zone;       // IANA
};
struct ImportSourceInfo {
  ImportSourceSpec spec;
  std::optional<std::string> head_sha, progress_token;
  int total = 0, done = 0;
  UtcTime started;
  std::optional<UtcTime> finished;
  std::string status;              // registered | running | paused | finished | failed
};
struct ProvenanceRow {
  std::string entity_type;         // analysis | revision | changeset | ref_object | bookmark
  Uuid entity;
  std::string path, commit_sha, git_blob_sha, git_author;
  UtcTime git_utc;
  std::optional<std::string> detail_json;
};
struct ImportConflictRow {
  Uuid uuid;                       // caller-derived, so a re-run does not duplicate it
  std::string path;
  std::optional<Uuid> entity;
  ConflictKind kind;
  std::optional<Uuid> db_head_revision;
  std::optional<Sha256Digest> file_sha256;
  std::string detail_json;         // "{}" when empty
  std::string resolution = "pending";
};
struct ImportProgress { std::string token; int done = 0, total = 0; std::optional<std::string> head_sha; std::string status; };
struct ConflictFilter { std::optional<Uuid> source; std::optional<ConflictKind> kind; std::optional<std::string> resolution; };
```

- Produces on `IStore`:

```cpp
virtual Result<ImportSourceInfo> begin_import(const ImportSourceSpec& spec) = 0;      // insert, or return the stored row
virtual Result<std::vector<ImportSourceInfo>> import_sources() = 0;
virtual Result<std::vector<ImportConflictRow>> import_conflicts(const ConflictFilter& filter) = 0;
virtual Result<std::vector<ProvenanceRow>> provenance_for(Uuid entity) = 0;
virtual Result<bool> has_provenance(Uuid source, std::string_view commit_sha, std::string_view path) = 0;
virtual Result<bool> has_provenance_blob(Uuid source, std::string_view path, std::string_view git_blob_sha) = 0;
virtual Result<bool> has_conflict(Uuid source, std::string_view path, const Sha256Digest& file_sha256) = 0;
// git blob sha recorded for the current head revision of (subject, kind) from this source; nullopt if none.
virtual Result<std::optional<std::string>> imported_head_blob_sha(Uuid source, Uuid subject, Kind kind) = 0;
```

- [ ] **Step 1:** Write `0002_import_detail.sql`: `ALTER TABLE import_provenance ADD COLUMN detail jsonb;`. Run `python3 tools/ddl_sqlite.py` then `python3 tools/ddl_sqlite.py --check` (exit 0).
- [ ] **Step 2: Failing tests** (`ImportStoreTest : StoreTest`):
  - `BeginImportInsertsThenReturnsStored`: first call returns `status == "registered"`, `done == 0`; second call with the same uuid returns the same row and `import_sources().size() == 1`.
  - `SchemaStatusListsSecondMigration`: `schema_status()` has 2 entries.
  - `EmptyReads`: `import_conflicts({})`, `provenance_for(Uuid::v7())` are empty; `has_provenance(...)` false; `imported_head_blob_sha(...)` nullopt.
- [ ] **Step 3:** Run `ctest --preset dev -R ImportStoreTest`. Expected: build failure.
- [ ] **Step 4:** Implement in `src/import.cpp` following the query style of `src/refs.cpp`. Write paths for provenance and conflicts arrive in Task 4; the read functions are complete here.
- [ ] **Step 5:** Run again. Expected: PASS on SQLite (and PostgreSQL when `PYCHRON_TEST_PG_URL` is set).
- [ ] **Step 6:** Commit: `persistence: import source, provenance and conflict reads; migration 0002`.

### Task 4: `IImportUnitOfWork`

**Files:**
- Modify: `libs/persistence/include/pychron/persistence/import.hpp`, `store.hpp` (`AnalysisIngest`), `libs/persistence/src/import.cpp`, `src/ingest.cpp`, `src/unit_of_work.cpp` (share `insert_changeset`, `insert_revision`, payload inserts and the head move with the import path; move them to `store_impl.hpp` if they are file-local), `tests/persistence/test_import_store.cpp`

**Interfaces:**
- Consumes: Task 3 structs.
- Produces:

```cpp
struct ImportedRevision { Uuid uuid; Uuid subject; Kind kind; RevisionPayload payload; };
struct ImportedChangeset {
  Uuid uuid; ChangesetKind kind;           // Import or Reference
  Uuid author_user; UtcTime created; std::string message;
  std::vector<ImportedRevision> revisions; // parent = head of (subject, kind) at write time
};
class IImportUnitOfWork {
 public:
  virtual ~IImportUnitOfWork() = default;
  virtual Result<void> add_changeset(ImportedChangeset changeset) = 0;
  virtual Result<void> add_provenance(ProvenanceRow row) = 0;
  virtual Result<void> add_conflict(ImportConflictRow row) = 0;
  virtual Result<void> set_progress(ImportProgress progress) = 0;
  // One transaction, one change_log entry. Rows whose uuid already exists are skipped.
  virtual Result<ChangeSeq> commit() = 0;
};
// IStore:
virtual Result<std::unique_ptr<IImportUnitOfWork>> begin_import_batch(Uuid source, Uuid client) = 0;
// AnalysisIngest gains:
std::optional<Uuid> import_source;         // stamped on the collection changeset
std::optional<Uuid> author_user;           // overrides `analyst` as changeset author when set
```

- [ ] **Step 1: Failing tests:**
  - `ImportedChangesetKeepsCallerIdsAndTime`: after `seed_lab` and one `analysis_item` ingest, add a changeset with fixed uuid `U`, `created = *UtcTime::parse("2016-03-04T05:06:07Z")`, one intercepts revision with fixed uuid `R`; commit. `head(analysis, Kind::Intercepts) == R`; `history(...)` last entry has `uuid == R`, parent = previous head, changeset created = that time.
  - `RerunIsNoOp`: commit the same batch twice through two units of work; `history` size unchanged, second `commit()` succeeds.
  - `ProvenanceAndConflictRoundTrip`: rows written are returned by `provenance_for`, `import_conflicts`, including `detail_json`; `has_provenance`, `has_provenance_blob`, `has_conflict` true; `imported_head_blob_sha(source, analysis, Kind::Intercepts)` equals the revision's provenance `git_blob_sha`.
  - `ProgressIsStoredWithTheBatch`: `set_progress({"abc", 3, 10, "head", "running"})` then commit; `begin_import(spec)` returns `progress_token == "abc"`, `done == 3`.
  - `FailedCommitLeavesNothing`: a changeset whose revision names an unknown subject makes `commit()` fail; token unchanged, no provenance rows.
  - `OneChangeLogEntryPerBatch`: `latest_change_seq()` rises by exactly 1 for a batch of 3 changesets.
  - `IngestStampsImportSource`: an `AnalysisIngest` with `import_source` set produces a collection changeset whose `import_source_uuid` is that source (query through the white-box `Db`).
  - `AppendOnlyTriggersStillHold`: a raw `UPDATE revision ...` fails after an import.
- [ ] **Step 2:** Run `ctest --preset dev -R ImportStoreTest`. Expected: FAIL.
- [ ] **Step 3:** Implement. Inserts use `ON CONFLICT DO NOTHING` on primary keys. A revision whose uuid already exists does not move the head again. Head moves use reason `commit`.
- [ ] **Step 4:** Run again, plus `ctest --preset dev -R "Ingest|UnitOfWork"` for regressions. Expected: PASS.
- [ ] **Step 5:** Commit: `persistence: import unit of work with caller-set ids and times`.

### Task 5: Catalog writes fit for import

**Files:**
- Modify: `libs/persistence/include/pychron/persistence/store.hpp`, `libs/persistence/src/collections.cpp` (or wherever `add_sample` lives), `tests/persistence/test_catalog_import.cpp` (create)

**Interfaces:**
- Produces: `SampleSpec` extended with one `std::optional` field per remaining nullable column of table `sample` in `0001_init.sql:29-39` (same names as the columns); `std::optional<Uuid> uuid` added to every catalog `*Spec` (used when the row is created; ignored when it exists);

```cpp
struct LoadSpec { std::string name; std::optional<std::string> holder; std::optional<UtcTime> created; std::optional<std::string> note; std::optional<Uuid> uuid; };
struct LoadPositionSpec { Uuid load; int position = 0; std::optional<Uuid> identifier; std::optional<double> weight; std::optional<std::string> note; };
virtual Result<Uuid> add_load(Uuid client, const LoadSpec& spec) = 0;
virtual Result<void> add_load_position(Uuid client, const LoadPositionSpec& spec) = 0;
```

  Match `LoadSpec`/`LoadPositionSpec` fields to tables `load`, `load_position` (`0001_init.sql:71-76`); drop any field the table lacks.

- [ ] **Step 1: Failing tests** (`CatalogImportTest : StoreTest`), one per `add_*` used by the importer (`ensure_user`, `add_mass_spectrometer`, `add_extract_device`, `add_principal_investigator`, `add_project`, `add_material`, `add_sample`, `add_irradiation`, `add_level`, `add_irradiation_position`, `add_identifier`, `add_ref_object`, `add_repository`, `add_load`): calling twice with the same natural key returns the same uuid and `latest_change_seq()` does not rise on the second call. Plus `CreatedRowUsesSuppliedUuid`, `SampleKeepsEveryColumn`.
- [ ] **Step 2:** Run `ctest --preset dev -R CatalogImportTest`. Expected: FAIL for each function that errors or duplicates on the second call.
- [ ] **Step 3:** Fix each failing function to select by natural key first and return the existing uuid. Natural keys are the table's UNIQUE constraints in `0001_init.sql`.
- [ ] **Step 4:** Run again and `ctest --preset dev -R persistence`. Expected: PASS.
- [ ] **Step 5:** Commit: `persistence: ensure-by-natural-key catalog writes, loads, full sample`.

### Task 6: `ProcessSpec::stdout_file`

**Files:**
- Modify: `libs/core/include/pychron/core/process.hpp`, the POSIX and Windows process sources under `libs/core/src/`, `tests/core/test_process.cpp`

**Interfaces:**
- Produces: `std::optional<std::filesystem::path> stdout_file;` on `ProcessSpec`. When set, the child's stdout is written to that file with no size limit and `ProcessResult::output` holds stderr only (still capped at 64 KiB).

- [ ] **Step 1: Failing tests:** `Process.StdoutFileIsUncapped`: run a program that writes 1 MiB to stdout (POSIX: `head -c 1048576 /dev/zero`; Windows: `cmd /c` loop or the test's own helper executable if one exists in `tests/core`); file size is 1048576 and `output` is empty. `Process.StdoutFileKeepsStderrSeparate`: a child writing to both gives stderr text in `output` and only stdout in the file. `Process.StdinStillFed`: `cat` (or `findstr "^"`) with `input = "abc\n"` and `stdout_file` set writes `abc`.
- [ ] **Step 2:** Run `ctest --preset dev -R Process`. Expected: build failure.
- [ ] **Step 3:** Implement: `posix_spawn_file_actions_addopen` for fd 1 on POSIX; `CreateFileW` with an inheritable handle as `hStdOutput` on Windows.
- [ ] **Step 4:** Run again. Expected: PASS.
- [ ] **Step 5:** Commit: `core: run_process can send stdout to a file`.

### Task 7: `libs/ingest`: batch types, ids, adapter interface, `BatchWriter`

**Files:**
- Create: `libs/ingest/CMakeLists.txt`, `libs/ingest/include/pychron/ingest/{ids,batch,adapter,writer}.hpp`, `libs/ingest/src/{ids,writer}.cpp`, `tests/ingest/CMakeLists.txt`, `tests/ingest/fake_adapter.hpp`, `tests/ingest/test_ids.cpp`, `tests/ingest/test_batch_writer.cpp`
- Modify: root `CMakeLists.txt` (`PYCHRON_LIBS`)

**Interfaces:**
- Consumes: Tasks 2–5.
- Produces (namespace `pychron::ingest`; `P::` = `pychron::persistence::`):

```cpp
// ids.hpp
inline constexpr std::string_view kImportNamespace = "6f0e4c1a-9d7b-5c2e-8a41-70796368726e";
std::string normalize_source_url(std::string_view url);   // strip trailing "/" and ".git", lower-case scheme+host; local paths made absolute, otherwise untouched
P::Uuid source_id(P::ImportSourceKind kind, std::string_view url, std::string_view branch);
P::Uuid changeset_id(std::string_view url, std::string_view commit);
P::Uuid revision_id(std::string_view url, std::string_view commit, std::string_view path);
P::Uuid catalog_id(std::string_view table, std::string_view natural_key);
P::Uuid derived_analysis_id(std::string_view url, std::string_view runid);
P::Uuid conflict_id(std::string_view url, std::string_view commit, std::string_view path);
```

  Name strings are the arguments joined with `'\n'`, prefixed by a tag (`"source"`, `"changeset"`, `"revision"`, `"catalog"`, `"analysis"`, `"conflict"`).

```cpp
// batch.hpp
struct SourceKey { std::string commit, path, blob_sha; };
struct GitWho { std::string name, email; P::UtcTime utc; };

// Catalog items name parents by natural key; the writer resolves uuids.
struct PiItem { std::string last_name, first_initial; std::optional<std::string> affiliation, email; };
struct ProjectItem { std::string name; std::optional<std::string> pi_last_name, pi_first_initial; };
struct MaterialItem { std::string name, grainsize; };
struct SampleItem { P::SampleSpec fields; std::string project, material, grainsize; };   // fields.project/material ignored
struct IrradiationItem { std::string name; };
struct LevelItem { std::string irradiation, name; std::optional<std::string> holder; std::optional<double> z; std::optional<std::string> note; };
struct PositionItem { std::string irradiation, level; int position = 0; std::string identifier;
                      std::optional<std::string> sample, project, material, grainsize; };
struct SpecialIdentifierItem { std::string identifier, analysis_type; std::optional<std::string> mass_spectrometer; };
struct UserItem { std::string name; };
struct MassSpecItem { P::MassSpectrometerSpec spec; };
struct ExtractDeviceItem { std::string name; };
struct LoadItem { P::LoadSpec spec; };
struct RepositoryItem { std::string name; };
struct RefObjectItem { P::RefObjectSpec spec; };
using CatalogItem = std::variant<PiItem, ProjectItem, MaterialItem, SampleItem, IrradiationItem, LevelItem, PositionItem,
                                 SpecialIdentifierItem, UserItem, MassSpecItem, ExtractDeviceItem, LoadItem,
                                 RepositoryItem, RefObjectItem>;

struct RefObjectKey { std::string ref_type, name; };
using SubjectRef = std::variant<P::Uuid, RefObjectKey>;       // analysis uuid, or a reference object

struct RootKeys { SourceKey record, signals, intercepts, baselines, blanks, icfactors, tags; };
struct AnalysisItem {
  P::AnalysisIngest ingest;      // analysis uuid set; changeset, created, roots.* uuids, import_source, author_user left for the writer
  RootKeys keys;                 // record = the <runid>.json commit that dates and authors the collection
  GitWho who;
  bool synthetic_collection = false;
  std::vector<std::string> repositories;
  std::string detail_json = "{}";
};
struct BlobItem { SourceKey key; P::BlobIngest blob; };
struct RevisionItem { SourceKey key; SubjectRef subject; P::Kind kind; P::RevisionPayload payload; };
struct ChangesetItem { std::string commit; P::ChangesetKind kind; GitWho who; std::string message; std::vector<RevisionItem> revisions; };
struct ConflictItem { SourceKey key; std::optional<P::Uuid> entity; P::ConflictKind kind; std::optional<Sha256Digest> file_sha256; std::string detail_json = "{}"; };
struct BookmarkItem { std::string name, commit; std::vector<P::Uuid> analyses; GitWho who; };

struct ImportBatch {
  std::vector<CatalogItem> catalog;      // in dependency order
  std::vector<BlobItem> blobs;
  std::vector<AnalysisItem> analyses;
  std::vector<ChangesetItem> changesets; // in source order
  std::vector<ConflictItem> conflicts;
  std::vector<BookmarkItem> bookmarks;
  std::string resume_token;
  int done = 0, total = 0;
  std::string head;
};

// adapter.hpp
struct IImportState {            // read-only view of what is already imported, given to adapters
  virtual ~IImportState() = default;
  virtual Result<std::optional<std::string>> head_blob_sha(const SubjectRef& subject, P::Kind kind) = 0;
  virtual Result<bool> analysis_exists(P::Uuid analysis) = 0;
};
struct SourceDescription { P::ImportSourceKind kind; std::string url, branch, head; };
class ISourceAdapter {
 public:
  virtual ~ISourceAdapter() = default;
  virtual Result<SourceDescription> describe() = 0;
  virtual Result<int> plan(std::optional<std::string> resume_token, IImportState& state) = 0;  // units remaining
  virtual Result<std::optional<ImportBatch>> next_batch() = 0;                                  // nullopt: done
};

// writer.hpp
struct WriterConfig { std::string importer_version, lab_time_zone; std::map<std::string, std::string> author_map; /* email -> user name */ bool dry_run = false; };
struct RunStats { int batches = 0, analyses = 0, changesets = 0, revisions = 0, conflicts = 0, would_write = 0; bool finished = false; };
class BatchWriter {
 public:
  BatchWriter(P::IStore& store, P::Uuid client, WriterConfig config);
  Result<P::ImportSourceInfo> open(ISourceAdapter& adapter);                    // begin_import; returns the stored row with its token
  Result<RunStats> run(ISourceAdapter& adapter, std::optional<int> max_batches, const std::function<bool()>& keep_going,
                       const std::function<void(const RunStats&, const ImportBatch&)>& on_batch);
  IImportState& state();
};
```

  Writer rules: author user = `author_map[email]` if present, else `ensure_user("git:" + email)`. An `AnalysisItem` whose identifier or mass spectrometer is missing in the store becomes an `UnknownAnalysis` conflict, not an error. `IngestItem.item = revision_id(url, keys.record.commit, keys.record.path)`; `payload_sha256` = SHA-256 of `keys.record.blob_sha`. Root revision uuids = `revision_id(url, key.commit, key.path)` for each of the six kinds. Provenance: one row per analysis, per revision, per changeset; `synthetic_collection` and the extra root commit shas go in `detail_json`. In `dry_run` nothing is written and `would_write` counts every row whose uuid is not yet in the store. Status transitions: `running` while batches flow, `paused` when `keep_going()` returns false or `max_batches` is reached, `finished` at end of stream, `failed` on error.

- [ ] **Step 1: Failing tests.**
  - `test_ids.cpp`: `NormalizeUrl` — `"https://GitHub.com/NMGRLData/Foo.git/"` → `"https://github.com/NMGRLData/Foo"`; `"git@GitHub.com:NMGRLData/Foo.git"` → `"git@github.com:NMGRLData/Foo"`; `IdsAreStableAndDistinct` — each function is deterministic, has `version() == 5`, and `revision_id(u, c, "a") != revision_id(u, c, "b")`; `changeset_id(u, c) != revision_id(u, c, "")`.
  - `test_batch_writer.cpp` (parameterised over `engines()`; `FakeAdapter` serves a scripted `std::vector<ImportBatch>` and can fail at batch N): `WritesCatalogAnalysesAndRevisions` (one batch: sample chain + identifier, one analysis, one later intercepts changeset → `load_analysis` finds it, intercepts history size 2, changeset time = `who.utc`, provenance present); `UnknownAuthorBecomesGitUser`; `AuthorMapPicksExistingUser`; `MissingIdentifierBecomesConflict`; `ResumeAfterFailureMatchesCleanRun` (fail at batch 2 of 4, run again; row counts of `changeset`, `revision`, `import_provenance`, `import_conflict` equal a clean run on a fresh database); `SecondRunWritesNothing` (`latest_change_seq()` unchanged, `stats.finished`); `DryRunCountsWithoutWriting` (`would_write > 0` on an empty store, `0` after a real run); `PauseStopsAfterCurrentBatch` (`keep_going` false after batch 1 → status `paused`, token of batch 1); `BookmarkAndMembership`.
- [ ] **Step 2:** Run `ctest --preset dev -R "IngestIds|BatchWriter"`. Expected: build failure.
- [ ] **Step 3:** Implement `libs/ingest` (CMake: `pychron::ingest`, PUBLIC `pychron::persistence pychron::core`, return early unless `PYCHRON_PERSISTENCE_ENABLED`). `tests/ingest/CMakeLists.txt` mirrors `tests/persistence/CMakeLists.txt` and adds `${PROJECT_SOURCE_DIR}/tests/persistence` to the include path for `store_fixture.hpp`.
- [ ] **Step 4:** Run again. Expected: PASS.
- [ ] **Step 5:** Commit: `ingest: batch model, id derivation and batch writer`.

### Task 8: Time zone conversion

**Files:**
- Create: `libs/ingest/include/pychron/ingest/tz.hpp`, `libs/ingest/src/tz.cpp`, `tests/ingest/test_tz.cpp`
- Modify: `libs/ingest/CMakeLists.txt`, `cmake/PychronDependencies.cmake`

**Interfaces:**
- Produces:

```cpp
bool known_zone(std::string_view iana);
enum class LocalKind { Unique, Ambiguous, Nonexistent };
struct LocalToUtc { persistence::UtcTime utc; LocalKind kind = LocalKind::Unique; };
// "YYYY-MM-DD HH:MM:SS[.ffffff]" or with 'T'. Ambiguous: the earlier instant. Nonexistent: the instant of the transition.
Result<LocalToUtc> local_to_utc(std::string_view naive_local, std::string_view iana);
```

- [ ] **Step 1: Failing tests:** `Tz.Plain`: `"2019-07-01 12:00:00"`, `America/Denver` → `2019-07-01T18:00:00Z`, `Unique`. `Tz.Ambiguous`: `"2019-11-03 01:30:00"` → `2019-11-03T07:30:00Z`, `Ambiguous`. `Tz.Nonexistent`: `"2019-03-10 02:30:00"` → `2019-03-10T09:00:00Z`, `Nonexistent`. `Tz.Micros`: `"2019-07-01T12:00:00.250000"` keeps 250000 µs. `Tz.UnknownZone`: `known_zone("Mars/Olympus") == false`, `local_to_utc` returns an error. `Tz.Garbage`: `"yesterday"` returns an error.
- [ ] **Step 2:** Run `ctest --preset dev -R "^Tz\\."`. Expected: build failure.
- [ ] **Step 3:** In CMake, `check_cxx_source_compiles` a program that calls `std::chrono::locate_zone("UTC")`; set `PYCHRON_HAVE_STD_TZDB`. When it is off, FetchContent HowardHinnant/date `v3.0.3` with `USE_SYSTEM_TZ_DB ON`, `BUILD_TZ_LIB ON`, linked PRIVATE to `pychron_ingest`. `tz.cpp` uses `std::chrono` or `date::` behind one `#if`.
- [ ] **Step 4:** Run again. Expected: PASS.
- [ ] **Step 5:** Commit: `ingest: local time to UTC by IANA zone`.

### Task 9: `libs/dvc` scaffold and `LegacyJsonLayout`

**Files:**
- Create: `libs/dvc/CMakeLists.txt`, `libs/dvc/include/pychron/dvc/legacy_layout.hpp`, `libs/dvc/src/legacy_layout.cpp`, `libs/dvc/src/legacy_json.{hpp,cpp}`, `tests/dvc/CMakeLists.txt`, `tests/dvc/test_legacy_layout.cpp`
- Modify: `cmake/PychronDependencies.cmake` (nlohmann_json 3.11.3 by `FetchContent_Declare(... URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz URL_HASH SHA256=<computed with shasum -a 256 on download> FIND_PACKAGE_ARGS CONFIG)`), `vcpkg.json` (`"nlohmann-json"`), root `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 fixtures and README table; Task 7 batch types; Task 8.
- Produces (namespace `pychron::dvc`):

```cpp
enum class FileKind { Record, Data, Intercepts, Baselines, Blanks, IcFactors, Tags, PeakCenter, Extraction, Monitor,
                      Cosmogenic, InterpretedAge, Ignored, Unknown };
struct PathInfo { FileKind kind = FileKind::Unknown; std::string runid; };   // runid empty for InterpretedAge/Ignored/Unknown
PathInfo classify_path(std::string_view repo_path);

struct ParseContext { std::string lab_time_zone; };
struct ParsedRecord { persistence::AnalysisIngest ingest; bool had_uuid = true; std::vector<std::string> notes; };
Result<ParsedRecord> parse_record(std::string_view json, const ParseContext& ctx);          // <runid>.json (+ what it embeds)
Result<std::vector<persistence::BlobIngest>> parse_data(std::string_view json, persistence::SignalRefs& refs_out);
Result<persistence::RevisionPayload> parse_revision(FileKind kind, std::string_view json);  // Intercepts..Cosmogenic
struct ParsedInterpretedAge { std::string name; persistence::InterpretedAgeValue value;
                              struct Member { persistence::Uuid analysis; double age = 0, age_err = 0; }; std::vector<Member> members; };
Result<ParsedInterpretedAge> parse_interpreted_age(std::string_view json);
// Satellite kinds (PeakCenter, Extraction, Monitor) fold into an AnalysisIngest:
Result<void> merge_satellite(FileKind kind, std::string_view json, persistence::AnalysisIngest& into);
```

  `legacy_json.hpp` (private): `Result<nlohmann::json> parse_legacy(std::string_view)` — accepts bare `NaN`, `Infinity`, `-Infinity` (pre-pass that rewrites those tokens outside strings to `null`), a UTF-8 BOM and CRLF; anything else invalid is an error.

- [ ] **Step 1: Failing tests.**
  - `LegacyJson.NanAndInfinityBecomeNull`: `{"a": NaN, "b": [Infinity, -Infinity], "c": "NaN"}` parses; `a` is null, `c` is the string `"NaN"`. `LegacyJson.BomAndCrlf`. `LegacyJson.TruncatedIsError`.
  - `Layout.ClassifiesEveryFixturePath`: one assertion per row of the Task 1 README table, plus `classify_path("README.md").kind == Ignored`, `classify_path("664/weird/57-01A.xyz.json").kind == Unknown`.
  - `Layout.RecordFixture`: `parse_record` on the fixture record gives the identifier, aliquot, increment, analysis type, mass spectrometer, uuid and UTC timestamp read by hand from the file (write the literal values into the test).
  - `Layout.DataFixtureRoundTrips`: each blob's `codec == "f32le-tv/1"`; `decode_tv` of the first isotope equals `decode_legacy_ff_base64` of the fixture's base64 string; `n_points` matches.
  - One test per revision kind on its fixture file asserting two literal values from the file (for intercepts: one isotope's value and its fit string).
  - `Layout.UnknownKeysGoToExtra`: add a key `"zz_new": 1` to a copy of the intercepts fixture; the row's `extra_json` contains `zz_new`.
  - `Layout.RecordWithoutUuid`: `had_uuid == false`, `ingest.analysis.is_nil()`.
  - `Layout.NaiveTimestampUsesLabZone`: a record timestamp `2019-11-03 01:30:00` with zone `America/Denver` gives `2019-11-03T07:30:00Z` and a note containing `ambiguous`.
  - `Layout.GarbageIsError`: `parse_revision(FileKind::Intercepts, "not json")` fails.
  - `Layout.InterpretedAgeFixture`: name, age, error and the first member's analysis uuid and age as literals from the fixture.
- [ ] **Step 2:** Run `ctest --preset dev -R "LegacyJson|Layout"`. Expected: build failure.
- [ ] **Step 3:** Implement. `pychron::dvc` links PUBLIC `pychron::ingest`, PRIVATE `nlohmann_json::nlohmann_json` (SYSTEM include). `tests/dvc` gets `PYCHRON_DVC_FIXTURES_DIR` as a compile definition. Field mapping follows the reader in `libs/processing/adapters/store/src/store_source.cpp` (`analysis_from_store`, `fit_spec`): whatever that reader consumes must be populated.
- [ ] **Step 4:** Run again. Expected: PASS.
- [ ] **Step 5:** Commit: `dvc: legacy JSON layout and parsers`.

### Task 10: `GitReader`

**Files:**
- Create: `libs/dvc/include/pychron/dvc/git_reader.hpp`, `libs/dvc/src/git_reader.cpp`, `tests/dvc/git_fixture.hpp`, `tests/dvc/test_git_reader.cpp`

**Interfaces:**
- Consumes: Task 6.
- Produces:

```cpp
struct GitCommit { std::string sha; std::vector<std::string> parents; ingest::GitWho author; std::string message; };
struct GitChange { std::string commit, path, blob_sha; char status = 'M'; };   // A, M, D; renames arrive as D + A
struct GitTag { std::string name, commit; };
struct GitConfig { std::filesystem::path repo; std::string branch; std::filesystem::path scratch;
                   std::chrono::milliseconds timeout{std::chrono::minutes(30)}; std::size_t cache_bytes = 256u << 20; };
class GitReader {
 public:
  static Result<GitReader> open(GitConfig config);            // verifies git exists, repo is a repo, branch resolves, not shallow
  static Result<std::filesystem::path> mirror(std::string_view url, const std::filesystem::path& cache_dir);  // clone --mirror, or fetch if present
  const std::string& head() const;
  Result<std::string> default_branch() const;
  Result<std::vector<std::string>> rev_list(std::optional<std::string> after) const;  // topo-order, reverse; `after` exclusive
  Result<bool> is_ancestor(std::string_view ancestor, std::string_view descendant) const;
  Result<std::vector<GitCommit>> commits(std::span<const std::string> shas) const;
  Result<std::vector<GitChange>> changes(std::span<const std::string> shas) const;    // vs first parent; root commits vs empty tree
  Result<void> fetch_blobs(std::span<const std::string> blob_shas);                   // into the LRU
  Result<std::string_view> blob(const std::string& blob_sha);                         // must have been fetched; valid until next fetch
  Result<std::vector<GitTag>> tags() const;
};
```

  `GitFixture` (test helper): `init()`, `write(path, text)`, `remove(path)`, `commit(message, date_iso, author = "Ann <ann@example.org>") -> sha`, `branch(name)`, `checkout(name)`, `merge(name, message, date)`, `tag(name)`, `path()`; temp dir removed in the destructor; `static bool available()`.

  Commands: `git -C <repo> rev-list --topo-order --reverse <branch>`; `git log --no-walk=unsorted --stdin -z --format=%H%x1f%P%x1f%an%x1f%ae%x1f%aI%x1f%B`; `git diff-tree --stdin -r -z --root -m --first-parent --no-renames --raw --no-abbrev`; `git cat-file --batch` with shas on stdin and `stdout_file` in `scratch`; `git for-each-ref refs/tags --format=...` peeling annotated tags (`%(*objectname)`); `git rev-parse --is-shallow-repository`; `git merge-base --is-ancestor`. All output that can grow uses `stdout_file`.

- [ ] **Step 1: Failing tests** (each skips without git):
  - `GitReader.LinearHistoryInOrder`: three commits; `rev_list(nullopt)` returns them oldest first; `rev_list(first)` returns the last two.
  - `GitReader.CommitFields`: author name, email, message with a blank line and trailing text preserved; `2016-03-04T05:06:07-07:00` gives `who.utc == 2016-03-04T12:06:07Z`.
  - `GitReader.ChangesAddModifyDelete`: statuses `A`, `M`, `D`; the root commit lists its files as `A`.
  - `GitReader.RenameIsDeletePlusAdd`.
  - `GitReader.MergeDiffsAgainstFirstParent`: only files that differ from the first parent appear for the merge commit; the side branch's commits appear before the merge in `rev_list`.
  - `GitReader.OddFileNames`: `"664/in tercepts/57 01A.inte.json"`, `"dir/naïve.json"`, `"dir/q\"uote.json"` come back byte-identical.
  - `GitReader.BlobLargerThan64KiB`: a 300000-byte file is returned whole and equal.
  - `GitReader.BlobCacheEvicts`: `cache_bytes = 1000`, fetch two 800-byte blobs in separate calls; both readable right after their own fetch.
  - `GitReader.TagsLightweightAndAnnotated`: both resolve to the commit sha.
  - `GitReader.IsAncestor`.
  - `GitReader.OpenRejectsNonRepo`, `OpenRejectsMissingBranch`, `OpenRejectsEmptyRepo`, `OpenRejectsShallowClone` (clone the fixture with `--depth 1 file://...`): each error message names the path and the reason.
- [ ] **Step 2:** Run `ctest --preset dev -R GitReader`. Expected: build failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run again. Expected: PASS.
- [ ] **Step 5:** Commit: `dvc: git reader over one-shot git subprocesses`.

### Task 11: Project repo adapter

**Files:**
- Create: `libs/dvc/include/pychron/dvc/project_adapter.hpp`, `libs/dvc/src/project_adapter.cpp`, `tests/dvc/legacy_repo_builder.hpp`, `tests/dvc/test_project_import.cpp`

**Interfaces:**
- Consumes: Tasks 7, 9, 10.
- Produces:

```cpp
struct ProjectAdapterConfig { GitConfig git; std::string url; std::string repository_name; std::string lab_time_zone;
                              int batch_commits = 500; bool catalog_from_repos = false; };
class ProjectRepoAdapter final : public ingest::ISourceAdapter {
 public:
  static Result<std::unique_ptr<ProjectRepoAdapter>> open(ProjectAdapterConfig config);
  // describe / plan / next_batch
};
```

  `LegacyRepoBuilder` (test helper on `GitFixture`): `collect(runid, uuid, date) ` writes the fixture file set for one analysis (record with `<COLLECTION>` message, then `<ISOEVO> default collection fits`, `<BLANKS> preceding`, `<ICFactor> default` commits, as Task 1 found them); `refit(runid, isotope, new_value, date)`; `import_without_collection(runid, uuid, date)`; `set_tag(runid, tag, date)`.

  Behaviour (spec 4.4): resume token = sha of the last commit with no pending analysis at or before it. Pending analyses (record seen, not all of intercepts/baselines/blanks/icfactors yet) are held across batches and flushed when complete; at end of walk any still pending are flushed as `synthetic_collection` with the roots they have and default empty rows for the rest. On `plan(token)`, pending state is rebuilt by re-reading from the token. A file is skipped when its blob sha equals `state.head_blob_sha(subject, kind)` or the adapter's in-walk map. Deleted files emit nothing. `FileKind::Unknown` or a parse error emits an `Unparseable` conflict with `file_sha256`. A record without uuid gets `ingest::derived_analysis_id(url, runid)` and `detail_json` `{"derived_uuid": true}`. A runid whose record appears with a second, different uuid emits `IdentityClash`. An analysis for which `state.analysis_exists(uuid)` is true and that this source did not create emits no `AnalysisItem`, only repository membership; if its record blob differs from the stored one, `IdentityClash`. Interpreted-age files become `Kind::InterpretedAge` revisions on an interpreted-age subject created through `add_interpreted_age` (add `InterpretedAgeItem { P::InterpretedAgeSpec spec; }` to `CatalogItem` and `InterpretedAgeKey { std::string name; }` to `SubjectRef` in Task 7's headers as part of this task). Tags: one `BookmarkItem` per git tag, emitted in the batch containing the tagged commit, listing every analysis imported so far. With `catalog_from_repos`, each record also emits the catalog items its fields imply and one `IdentityClash` conflict with `{"synthesized": true}` per synthesized identifier.

- [ ] **Step 1: Failing tests** (git fixture → adapter → `BatchWriter` → store, SQLite; skip without git):
  - `ProjectImport.CollectionFoldsIntoOneChangeset`: one `collect`; store has one analysis with the legacy uuid, each of the six kinds has history size 1 with parent null, changeset kind `collection`, created = the `<COLLECTION>` commit date, author `git:ann@example.org`; provenance `detail` lists four commit shas.
  - `ProjectImport.RefitAddsRevision`: `collect` then `refit`; intercepts history size 2; second revision's changeset kind `import`, message verbatim, time = refit commit date; other kinds still size 1.
  - `ProjectImport.CollectionSplitAcrossBatches`: `batch_commits = 2` with one `collect` (4 commits) → same store rows as with `batch_commits = 500`.
  - `ProjectImport.ResumeMidCollection`: `batch_commits = 2`, `max_batches = 1`, then run again → same rows as an uninterrupted run.
  - `ProjectImport.SyntheticCollection`: `import_without_collection`; provenance `detail` has `"synthetic_collection": true`.
  - `ProjectImport.UnparseableFileIsConflictAndImportContinues`: a garbage intercepts file between two good analyses; one `Unparseable` conflict; both analyses imported.
  - `ProjectImport.NanInFileImports`: an intercepts file with a bare `NaN` imports with no conflict.
  - `ProjectImport.RecordWithoutUuidGetsDerivedId`: analysis uuid = `derived_analysis_id(url, runid)`.
  - `ProjectImport.SameRunidTwoUuidsIsIdentityClash`: second record overwrites the first with a different uuid → one `IdentityClash`, first analysis intact, later analyses imported.
  - `ProjectImport.SameUuidInSecondRepoAddsMembershipOnly`: import repo A then repo B holding the same analysis → one analysis, member of both repositories, no conflict.
  - `ProjectImport.MergeCommitLinearizes`: refit on a side branch merged in → intercepts history size 2, no duplicate revision.
  - `ProjectImport.DeletedFileAddsNothing`.
  - `ProjectImport.GitTagBecomesBookmark`: `bookmark_heads` returns the heads as of the tagged commit, not later ones.
  - `ProjectImport.SecondRunIsNoOp`: `latest_change_seq()` unchanged.
  - `ProjectImport.NewCommitsAfterFinishAreImported`: finish, add a `refit`, run again → history grows by one.
  - `ProjectImport.RewrittenHistoryStops`: finish, `git commit --amend` the head, run → error whose message contains `history was rewritten`; store unchanged.
  - `ProjectImport.CatalogFromReposSynthesizes`: empty store, `catalog_from_repos = true` → analysis imported; one `IdentityClash` with `synthesized`. Without the flag → `UnknownAnalysis` conflict and no analysis.
- [ ] **Step 2:** Run `ctest --preset dev -R ProjectImport`. Expected: build failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run again and `ctest --preset dev -R "BatchWriter|ingest"`. Expected: PASS.
- [ ] **Step 5:** Commit: `dvc: project repo adapter with full history`.

### Task 12: Meta repo adapter

**Files:**
- Create: `libs/dvc/include/pychron/dvc/meta_adapter.hpp`, `libs/dvc/src/meta_adapter.cpp`, `libs/dvc/src/meta_layout.{hpp,cpp}`, `tests/dvc/test_meta_import.cpp`

**Interfaces:**
- Consumes: Tasks 7, 9 (`parse_legacy`), 10; Task 1 meta fixtures.
- Produces:

```cpp
struct MetaAdapterConfig { GitConfig git; std::string url; std::string lab_time_zone; int batch_commits = 500; };
class MetaRepoAdapter final : public ingest::ISourceAdapter { public: static Result<std::unique_ptr<MetaRepoAdapter>> open(MetaAdapterConfig); };
```

  Each commit touching reference files → one `ChangesetItem` of kind `Reference`. Subjects are `RefObjectKey{ref_type, name}` with names: flux position `<irrad>/<level>/<position>`; production `<irrad>/<production name>`; level production `<irrad>/<level>`; chronology `<irrad>`; gains `<spectrometer>`; sensitivity `<spectrometer>`; holders by file stem. The adapter emits `IrradiationItem`, `LevelItem` and `RefObjectItem` before first use. A level file is diffed per position against the previous blob of that file: a `flux_position` revision only for positions whose JSON object changed; its `SourceKey.path` is `<file path>#<position>` so revision ids differ. Sensitivity: one revision per list entry in list order, path `<file>#<index>`, only for entries beyond the previously imported count or changed. Non-reference files are ignored.

- [ ] **Step 1: Failing tests** (fixture files committed through `GitFixture`):
  - `MetaImport.LevelFileMakesOneRevisionPerPosition`: fixture level with N positions → N `flux_position` ref objects, each history size 1, J value of position 1 equals the literal in the fixture.
  - `MetaImport.ChangedPositionOnly`: second commit changes J of one position → that object's history is 2, the others 1.
  - `MetaImport.ProductionAndChronology`: production ratios and dose list equal the fixture literals; changeset kind `reference`.
  - `MetaImport.SensitivityEntriesInOrder`: three entries → three revisions whose order matches the list; appending a fourth in a later commit adds exactly one.
  - `MetaImport.GainsAndHolders`.
  - `MetaImport.UnparseableLevelIsConflict`.
  - `MetaImport.SecondRunIsNoOp`.
- [ ] **Step 2:** Run `ctest --preset dev -R MetaImport`. Expected: build failure.
- [ ] **Step 3:** Implement. Payload shapes are `FluxValue`, `ProductionValue`, `LevelProductionValue`, `ChronologyValue`, `GainsValue`, `SensitivityValue`, `HolderValue` in `persistence/model.hpp:204-330`.
- [ ] **Step 4:** Run again. Expected: PASS.
- [ ] **Step 5:** Commit: `dvc: meta repo adapter`.

### Task 13: Dump converter and catalog adapter

**Files:**
- Create: `tools/legacy_dump_to_jsonl.py`, `tools/tests/test_legacy_dump_to_jsonl.py`, `tools/tests/fixtures/legacy_dump.sql`, `libs/dvc/include/pychron/dvc/catalog_adapter.hpp`, `libs/dvc/src/catalog_adapter.cpp`, `tests/dvc/fixtures/catalog/*.jsonl`, `tests/dvc/test_catalog_db.cpp`

**Interfaces:**
- Consumes: Task 7 catalog items; Task 1 table and column names.
- Produces: `python3 tools/legacy_dump_to_jsonl.py <dump.sql> <outdir>` → `<outdir>/<TableName>.jsonl` (one object per row, keys = column names, SQL `NULL` → `null`, numbers as numbers, everything else as strings) and `<outdir>/MANIFEST.json` `{"tables": {"<name>": <row count>}, "sha256": "<of dump>"}`. Exit 2 with a message on an unreadable or empty dump.

```cpp
struct CatalogAdapterConfig { std::filesystem::path dir; std::string lab_time_zone; int batch_rows = 2000; };
class CatalogAdapter final : public ingest::ISourceAdapter { public: static Result<std::unique_ptr<CatalogAdapter>> open(CatalogAdapterConfig); };
```

  `describe()`: kind `LegacyDb`, url = absolute dir, head = manifest `sha256`. Items are emitted in the order PI, project, material, sample, irradiation, level, position/identifier, users, mass spectrometers, extract devices, loads. Resume token = `"<table index>:<row index>"`. A row whose parent row is missing (dangling foreign key) → `IdentityClash` conflict with the table and legacy id in `detail`. Tables of schema design section 3.9 are not handled in this plan (see Deferred).

- [ ] **Step 1: Failing Python test** (`unittest`, same style as `tools/tests/test_pychron_conditionals_import.py`): fixture dump with two tables, a multi-row `INSERT`, a string containing `\'`, `,` and `)`, a `NULL`, a negative float, backtick-quoted names, `LOCK TABLES` and `/*! ... */` lines. Assert row counts, the escaped string round-trips, `NULL` → `None`, manifest counts. Second test: empty file → exit code 2.
- [ ] **Step 2:** Run `python3 -m unittest tools/tests/test_legacy_dump_to_jsonl.py`. Expected: FAIL (module missing).
- [ ] **Step 3:** Implement the converter with a character-level scanner for `INSERT INTO ... VALUES (...),(...);` (not regex over whole statements; dumps have multi-megabyte lines). Column names come from the preceding `CREATE TABLE`.
- [ ] **Step 4:** Run again. Expected: OK.
- [ ] **Step 5: Failing C++ tests:** `CatalogDb.ImportsInForeignKeyOrder` (fixture with one of each → store has the sample linked to its project and material, identifier linked to its position, level to irradiation); `CatalogDb.SampleKeepsLatLonAndNote`; `CatalogDb.DanglingForeignKeyIsConflict`; `CatalogDb.ResumeFromToken` (`batch_rows = 2`, `max_batches = 1`, then finish → same rows as one run); `CatalogDb.SecondRunIsNoOp`; `CatalogDb.MissingManifestIsError`.
- [ ] **Step 6:** Run `ctest --preset dev -R CatalogDb`. Expected: build failure. Implement. Run again. Expected: PASS.
- [ ] **Step 7:** Commit: `dvc: mysqldump to JSON lines and catalog adapter`.

### Task 14: Verifier

**Files:**
- Create: `libs/ingest/include/pychron/ingest/verify.hpp`, `libs/ingest/src/verify.cpp`, `tests/ingest/test_verifier.cpp`
- Modify: `libs/ingest/include/pychron/ingest/adapter.hpp`, the three adapters

**Interfaces:**
- Produces:

```cpp
// adapter.hpp: every unit the source contains, for accounting.
struct SourceUnit { std::string commit, path, blob_sha; bool deleted = false; };
// ISourceAdapter gains:
virtual Result<void> for_each_unit(const std::function<Result<void>(const SourceUnit&)>& visit) = 0;

// verify.hpp
struct LegacyAge { persistence::Uuid analysis; double age = 0, age_err = 0; };
struct ComputedAge { double age = 0, age_err = 0; };
using AgeFn = std::function<Result<std::optional<ComputedAge>>(persistence::Uuid analysis)>;   // nullopt: not comparable
struct VerifyOptions { double tolerance = 1e-9; };
struct VerifyReport {
  std::vector<SourceUnit> unaccounted;
  int would_write = 0;
  int parity_pass = 0, parity_fail = 0, parity_not_comparable = 0;
  std::vector<persistence::Uuid> parity_failures;
  bool ok() const;   // unaccounted empty && would_write == 0 && parity_fail == 0
};
Result<VerifyReport> verify(persistence::IStore& store, persistence::Uuid client, ISourceAdapter& adapter,
                            const WriterConfig& config, const AgeFn& age_fn, VerifyOptions options);
```

  Accounting rule (spec 6.1): a unit is accounted for when `deleted`, or `has_provenance(source, commit, path)`, or `has_conflict(source, path, sha256(bytes))` — adapters supply conflicts keyed the same way — or `has_provenance_blob(source, path, blob_sha)`; a unit whose path the adapter classifies as ignored is not visited at all. Idempotence = `BatchWriter` with `dry_run`. Parity: legacy ages = members of every imported interpreted-age head revision; relative difference `|a-b| / max(|a|,|b|)` on age and on error against `tolerance`; a failure also writes a `ValueMismatch` conflict (`conflict_id(url, "parity", analysis uuid)`) with both values. `verify` writes nothing else.

- [ ] **Step 1: Failing tests** (`FakeAdapter` extended with scripted units; fake `AgeFn`): `Verifier.CleanImportIsOk`; `Verifier.MissingProvenanceIsUnaccounted` (one extra unit → listed, `ok() == false`); `Verifier.DeletedAndConflictedUnitsAreAccounted`; `Verifier.KnownBlobAtOtherCommitIsAccounted`; `Verifier.NotYetImportedCountsWouldWrite`; `Verifier.ParityPassFailNotComparable` (three members: equal age → pass; age off by 1e-6 relative → fail and a `ValueMismatch` conflict with both values in `detail`; `AgeFn` nullopt → not comparable); `Verifier.ToleranceIsRelative` (age `1e9` vs `1e9 + 0.5` passes at `1e-9`).
- [ ] **Step 2:** Run `ctest --preset dev -R Verifier`. Expected: build failure.
- [ ] **Step 3:** Implement `verify` and `for_each_unit` in the three adapters; add `ProjectImport.VerifyAfterImportIsOk` and `MetaImport.VerifyAfterImportIsOk` (with an `AgeFn` returning nullopt).
- [ ] **Step 4:** Run `ctest --preset dev -R "Verifier|ProjectImport|MetaImport|CatalogDb"`. Expected: PASS.
- [ ] **Step 5:** Commit: `ingest: verify accounting, idempotence and age parity`.

### Task 15: `elctl import`

**Files:**
- Create: `apps/elctl/src/import.hpp`, `apps/elctl/src/import.cpp`, `apps/elctl/src/import_stub.cpp`, `apps/elctl/tests/test_import_cmd.cpp`
- Modify: `apps/elctl/CMakeLists.txt`, `apps/elctl/tests/CMakeLists.txt`, `apps/elctl/src/cli.cpp` (dispatch `import`, help text)

**Interfaces:**
- Consumes: everything above; `processing::StoreSource` and the reduction call the Data browser uses (`libs/processing/adapters/store`) to build the `AgeFn`.
- Produces: `int elctl::import_command(const std::vector<std::string>& args, Io io);`

  CMake: when `TARGET pychron_persistence`, compile `import.cpp`, link `pychron::dvc pychron::ingest pychron::processing` and define `PYCHRON_ELCTL_IMPORT`; otherwise compile `import_stub.cpp` (removing the other from the glob). Commands and flags exactly as spec section 5. Details the spec leaves open:
  - `add`: `--tz` required and checked with `ingest::known_zone`; `--source` that is a URL is mirrored into `--cache <dir>` (default `<user cache dir>/pychron/import`); for `project_repo` the repository name is the last path segment of the normalized url; default branch from `GitReader::default_branch()`; `GitReader::open` must succeed before anything is stored. Prints the source uuid. The author map and `--catalog-from-repos` are stored per source in a TOML file `<cache>/<source uuid>.toml` and re-read by `run`.
  - `run --all`: order `legacy_db`, `meta_repo`, then `project_repo` sorted by url. One stderr line per batch: `<name> <done>/<total> commits, <n> analyses, <n> conflicts`. SIGINT sets a flag read by `keep_going`; exit 0 and print `paused: <name> at <done>/<total>`.
  - `status`: one line per source: `uuid kind name status done/total head`.
  - `conflicts`: one line per row `kind path entity detail`; `--json` prints a JSON array.
  - `verify`: prints the three results; exit `kFailed` (1) when `!report.ok()` or pending conflicts exist.
  - Exit codes: 0, 1, 2 as spec; any `Result` error prints `elctl import: <message>` and returns 2.
  - The importer registers one client: `register_client({hostname, "importer", nullopt, <elctl version>})`.

- [ ] **Step 1: Failing tests** (through `elctl::run`, file-backed SQLite in a temp dir, `LegacyRepoBuilder` repo; compile only when `PYCHRON_ELCTL_IMPORT`; one test for the stub otherwise):
  - `ImportCmd.AddRunStatusVerify`: `add` returns 0 and prints a uuid; `run --all` returns 0; `status` output contains `finished` and `4/4`; `verify` returns 0.
  - `ImportCmd.AddIsIdempotent`: twice → one line in `status`.
  - `ImportCmd.AddRequiresTz` → 2, stderr contains `--tz`. `ImportCmd.AddRejectsUnknownZone` → 2.
  - `ImportCmd.AddRejectsNonRepo`, `AddRejectsMissingBranch`, `AddRejectsEmptyRepo` → 2, one stderr line naming the path; `status` shows no source.
  - `ImportCmd.RunLimitPausesAndResumes`: `run --batch 2 --limit 1` → 0, stdout contains `paused`; second `run` finishes.
  - `ImportCmd.ConflictsListsAndVerifyFails`: repo with a garbage file → `conflicts` prints one `unparseable` line; `conflicts --json` parses as an array of length 1; `verify` returns 1.
  - `ImportCmd.AuthorMapUsed`.
  - `ImportCmd.UnknownSubcommand` → 2.
  - `ImportCmd.BadDbUrl` → 2.
  - `ImportCmd.StubWithoutPersistence` (stub build): returns 2, stderr `elctl was built without persistence`.
- [ ] **Step 2:** Run `ctest --preset dev -R ImportCmd`. Expected: build failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run again; then configure a second build with `-DPYCHRON_PERSISTENCE=OFF` and run `ctest -R "ImportCmd|elctl"` there. Expected: PASS in both.
- [ ] **Step 5:** Commit: `elctl: import add, run, status, conflicts, verify`.

### Task 16: Real-data check, docs, merge

**Files:**
- Create: `tools/import_fixture_check.sh`
- Modify: `docs/dev_setup.md` (section "Importing legacy data": the five commands, the dump converter, `git` requirement), `AGENTS.md` (one bullet: `libs/ingest` and `libs/dvc` build with persistence; dvc tests need `git`), `.github/workflows/ci.yml` only if a runner lacks `git` or tzdata

- [ ] **Step 1:** Write the script: args `<build dir>`; clones the Task 1 repo at its pinned sha and MetaData into a temp dir, creates a SQLite store, runs `import add` (meta, project with `--catalog-from-repos`, `--tz America/Denver`), `import run --all`, `import status`, `import conflicts`, `import verify`; prints wall time and store file size; exits with verify's code.
- [ ] **Step 2:** Run it. Expected: accounting and idempotence pass. Record commits/second and the conflict and parity counts in the commit message. Parity failures or conflict kinds other than the synthesized-catalog ones are findings: report them to the user with the `conflicts --json` output before changing code; do not loosen tolerance.
- [ ] **Step 3:** Full local gate: `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`, then a gcc 13 build with `-DPYCHRON_WARNINGS_AS_ERRORS=OFF` if available, and `python3 -m unittest discover tools/tests`. Expected: all pass.
- [ ] **Step 4:** Commit: `Ingest: real-data check script and docs`.
- [ ] **Step 5:** `git fetch origin && git rebase origin/main`, re-run Step 3, merge the branch into `main`, push. Watch CI on all four toolchains; a failure on one compiler is a bug to fix, not to skip.

---

## Deferred (in spec, not in this plan)

- **Verbatim copy of `SamplePrep*` and similar tables into a `legacy` schema** (spec 4.2, schema design 3.9). The JSON-lines directory already preserves them; no reader needs them yet. Own task when a consumer exists.
- **Filling `published_file` / `publish_state` at end of repo** (spec 4.4 "Completion"). `publish_target` rows are created by the publisher (stage D5), which does not exist; do this in D5.
- **`hand_edit` conflicts on re-import of a published repo**: same dependency on D5.

## Self-review notes

- Spec 3.1 `provenance_for`, `imported_head_blob_sha`: Task 3/4. Spec 3.2 ids: Tasks 2, 7. 3.3 time: Tasks 8, 9. 3.4: Tasks 4, 7, 11. 4.1 order: Task 15. 4.2: Task 13. 4.3: Task 12. 4.4: Tasks 10, 11. 4.5: Task 10 (cache), Task 16 (measurement). 5: Task 15. 6: Task 14. 7: Tasks 7, 10, 11, 15. 8: each task. 9: Task 1.
- Review Focus pins: 1 → Task 9 `LegacyJson.NanAndInfinityBecomeNull`, Task 11 `NanInFileImports`; 2 → Task 10 `OddFileNames`; 3 → Task 6 `StdoutFileIsUncapped`, Task 10 `BlobLargerThan64KiB` and the 30 min default timeout; 4 → Task 10 `OpenRejects*`, Task 15 `AddRejects*`; 5 → Task 11 `RecordWithoutUuidGetsDerivedId`, `SameRunidTwoUuidsIsIdentityClash`.
