# DVC Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the DVC persistence spec stage by stage (D1-D7): one
shared PostgreSQL store with revisioned values and CAS heads, an outbox on
acquisition PCs, a history importer and a git publisher.

**Architecture:** `libs/persistence` (depends on core only) holds the schema,
the `IStore` access layer and, later, the outbox and uploader. `libs/dvc`
(later) maps `AnalysisRecord` to ingest payloads and owns the importer and
publisher. TinyORM over QtSql is the database layer for both engines, private
to `libs/persistence`.

**Tech Stack:** C++20, TinyORM v0.38.1 (query builder; no `tom`), range-v3
0.12.0, Qt6 Core + Sql (QSQLITE, QPSQL), PostgreSQL >= 14 (CI: 16), SQLite >=
3.37, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-10-01-dvc-schema-design.md` (section
12.4.1 records the TinyORM decision).

## Global Constraints

- Public headers in `libs/persistence/include/` are std-only: no Qt, no
  TinyORM. Qt, TinyORM and driver exceptions stay under `src/`.
- All SQL text lives in `src/sql/` or the migration files (P6).
- `migrations/pg/*.sql` is the only schema source. After editing it, run
  `python3 tools/ddl_sqlite.py` and commit the regenerated
  `migrations/sqlite/*.sql`. CI runs `--check`.
- Applied migrations are never edited: their SHA-256 is in `schema_version`
  and a mismatch is a fatal `Config` error (11.4). Changes go in a new
  `NNNN_<name>.sql`.
- Every writing transaction ends with `take_change()` (9.1).
- Tests run on SQLite always and on PostgreSQL when `PYCHRON_TEST_PG_URL` is
  set. Each test gets a fresh database or schema.

## Done (2026-10-02)

### D1 schema

- [x] `migrations/pg/0001_init.sql`: Appendix A DDL, plus explicit
      append-only triggers on every insert-only table and an `analysis_guard`
      trigger (only identity columns, `provisional`, `runid_text` and
      `signals_state` may change; no delete).
- [x] `tools/ddl_sqlite.py`: generates the STRICT SQLite DDL (type mapping,
      UUID/timestamp/JSON CHECKs, folded deferred FKs, triggers), with
      `--check` mode.
- [x] Migrations embedded in the library, applied one transaction each with
      a checksum. PostgreSQL migrators serialize on an advisory lock.
- [x] Tests: parity of tables, columns, PK, UNIQUE and FK between PostgreSQL
      and SQLite; every append-only table has its triggers; UPDATE/DELETE
      rejected; checksum mismatch fatal; outdated schema refused without
      `migrate`.
- [ ] `elctl db migrate|status|verify` (needs elctl to link persistence
      conditionally on `PYCHRON_PERSISTENCE_ENABLED`).
- [ ] Roles (11.2) as a deployment script, not a migration.

### D2 store core

- [x] `ids.hpp`: UUIDv7 (monotonic within a generator, survives clock
      steps), `UtcTime` (microsecond ISO-8601 `Z`).
- [x] `blob.hpp`: `f32le-tv/1`, `f32le-tvs/1`, legacy `>ff` base64,
      `SHA-256(codec || 0x00 || bytes)`.
- [x] SHA-256 moved from `experiment` to `core` (`pychron/core/sha256.hpp`,
      streaming); `experiment::record::sha256_hex` delegates to it.
- [x] `IStore` / `IUnitOfWork`: staged revisions and head moves; one
      transaction; CAS in (subject, kind) order; conflicts returned as
      values with the winner's changeset; nothing persists on conflict.
- [x] Change cursor: `change_counter` row lock, `change_log`,
      `change_entity`, `pg_notify`; `changes_since` paging.
- [x] Idempotent ingest: collection changeset with root revisions and heads
      for the six collection kinds; `ingest_receipt` (same sha: ack,
      different sha: `IdempotencyMismatch`); natural-key resolution (ensure
      user and load); blob ingest with `signals_state` completion.
- [x] Catalog writes (client, user, mass spectrometer, identifier) audited
      with a field diff in `change_entity.detail`.
- [x] Payloads: intercepts, baselines, blanks (+references), IC factors
      (+references), signal refs, tags, annotation, refpins, cosmogenic.
- [x] Invariants covered by tests: I1 (part), I2, I3, I4 (rollback to
      collection), I5 (sequential and 6 concurrent writers), I6 (gap-free
      and paging), I7, I9, I13 (signal refs).

## Remaining

### D2 completion

- [ ] Identity revisions (5.6): `identity_value` plus the same-transaction
      update of `analysis` identity columns and `runid_text`.
- [ ] Bookmarks (5.5): create from repository or group scope; restore as a
      `bookmark_restore` changeset.
- [ ] Reference data (6): `ref_object` create, `value` revisions with the
      per-type payloads; `resolve_refs(analysis, policy)` honouring
      `refpins`.
- [ ] Interpreted ages (5.7): `ia_value` and `ia_member` payloads.
- [ ] `derived_value` cache with fingerprint (4.3, I14).
- [ ] Remaining ingest satellites: `analysis_meta`, `peak_center`,
      `monitor_check`, `analysis_artifact`, `measured_position`,
      `spectrometer_snapshot`, `script_text`, `experiment_queue`; extend I13
      to their blobs.
- [ ] Property test: random commit/rollback/restore sequences keep I1-I6
      and I12 (12.6 `property/`).

### D3 outbox (needs experiment E4 `IAnalysisPersister`)

- [ ] `Outbox` (SQLite spool, `outbox_*` tables, WAL, `synchronous=FULL`).
- [ ] `Uploader` Scheduler task: dependency order, backoff, poison,
      classification by `ErrorKind` (`NotConnected`/`Timeout` retryable,
      `Protocol` permanent).
- [ ] Aliquot leases, provisional allocation and renumbering (8.5); lease
      check at ingest (I10).
- [ ] `OutboxPersister` in `libs/dvc`: `AnalysisRecord` to `AnalysisIngest`.
- [ ] Fault-injection suite (12.6 `outbox/`).

### D4-D7

- [ ] D4 importer (legacy MySQL catalog, project repos with full history,
      meta repo). Open questions 13.3 Q3-Q5 must be answered first.
- [ ] D5 publisher (byte-compatible files, bookmarks as tags).
- [ ] D6 `ChangeFeed`: poll plus `QSqlDriver::subscribeToNotification`.
- [ ] D7 writable offline export and `changeset` outbox items.

## Notes for implementers

- A store and its units of work belong to the opening thread (TinyORM
  connections are per thread). Open one store per thread.
- A unit of work holds a reference to its store's connection: the store must
  outlive it.
- PostgreSQL `jsonb` normalizes key order and whitespace on read. Compare
  JSON semantically, never as text. Byte-compatible publishing must rebuild
  JSON from columns plus `extra`.
- `:memory:` SQLite is one database per connection. Tests that need a second
  connection (raw SQL, threads) use a temp-file database.
