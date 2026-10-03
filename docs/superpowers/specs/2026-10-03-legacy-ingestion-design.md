# Legacy data ingestion: core and pychron DVC adapter

Date: 2026-10-03
Status: design, awaiting review
Related: `2026-10-01-dvc-schema-design.md` (sections 10.2, 10.3, 12.1, 13.3),
`2026-09-29-persistence-adr.md`, plan `2026-10-02-dvc-persistence.md` stage D4.

## 1. Purpose

Roughly a decade of lab data exists only in legacy pychron DVC repositories
(git repos of per-analysis JSON files, a MetaData repo and a MySQL index
database). The new store must be able to take all of it in, losslessly and
repeatably. The same machinery must later take in data from MassSpec and
ArArCalc.

This spec covers two things:

1. A source-agnostic **ingest core** (`libs/ingest`).
2. The **pychron DVC adapter** (`libs/dvc`), the first source.

MassSpec and ArArCalc adapters get their own specs. They are sketched here
only far enough to check that the core interface does not need to change for
them.

### 1.1 Success criteria

An import of a source is successful when `elctl import verify` reports:

- **Accounting.** Every file at every commit of the source is either imported
  (has an `import_provenance` row) or listed in `import_conflict`.
- **Idempotence.** Running the import again adds no rows.
- **Age parity.** Ages recomputed by `libs/reduction` from the imported heads
  match the legacy stored ages within a tolerance.

### 1.2 Decisions taken

| Topic | Decision |
|---|---|
| Scope of this spec | Ingest core and DVC adapter together |
| History | Full git history, as in schema design section 10.3 |
| Catalog source | A mysqldump file of the legacy database, not a live MySQL connection |
| Real-data fixture | Public `github.com/NMGRLData` repos; CI uses synthetic fixtures |
| Surface | CLI only (`elctl import ...`) |
| Structure | C++ core and C++ adapters |
| Dump handling | A `tools/` script converts the mysqldump to JSON lines; the adapter reads those |
| Git access (13.3 Q5) | Shell out to `git`; no libgit2 |
| Branches (13.3 Q4) | The default branch only; others only when named |
| Git authors (13.3 Q3) | Unknown authors become `git:<email>` users; an optional map file assigns emails to existing users |

### 1.3 Out of scope

MassSpec adapter, ArArCalc adapter, an import action in pychron-ui, commands
that resolve conflicts (accept or reject), the publisher, a PostgreSQL `COPY`
bulk path, a live MySQL connection.

Three items of schema design section 10.3 wait for the publisher (stage D5)
or a consumer, and are not built here: filling `published_file` and
`publish_state` at the end of a repo, `hand_edit` conflicts on re-import of a
published repo, and the verbatim copy of `SamplePrep*`-style tables into a
`legacy` schema (the JSON-lines directory already preserves them).

## 2. Architecture

```
apps/elctl      import add | run | status | conflicts | verify
     |
libs/ingest     ISourceAdapter -> ImportBatch stream -> BatchWriter -> IStore
     |          id derivation, provenance, conflict log, resume, Verifier
     |
libs/dvc        GitReader, LegacyJsonLayout, MetaRepoReader, CatalogDb
                (later: libs/massspec, libs/ararcalc)
```

Dependencies:

- `libs/ingest` depends on `persistence`, `core` and, for verify only,
  `reduction`.
- `libs/dvc` depends on `ingest`, `core` and nlohmann_json (new dependency).
- Qt stays confined to `libs/persistence`. Public headers of both new
  libraries are std-only.
- Both libraries, and the `elctl import` command, are built only when
  persistence is built.

### 2.1 Units

**`ISourceAdapter`** knows one source format and never touches the store.

- `describe()` returns kind, normalized url or path, branch and current head.
- `plan(resume_token)` returns the number of units of work remaining.
- `next_batch()` returns the next `ImportBatch`, or end of stream.

**`ImportBatch`** is the neutral, ordered unit handed from adapter to core:

- catalog specs (ensure-by-natural-key),
- `AnalysisIngest` items and their blobs,
- `ImportedChangeset { source_key, kind, author, utc, message, revisions[] }`,
- provenance rows and conflict rows,
- the resume token that becomes valid once this batch commits.

A batch carries no uuids except legacy analysis uuids. The adapter supplies
**source keys** (source url, commit sha, path) and the core derives ids from
them, so one rule applies to every source.

**`BatchWriter`** writes one batch in two steps. First the idempotent writes:
catalog rows (ensure-by-natural-key), analyses and blobs (the existing
`IStore::ingest`, keyed by a deterministic item id). Then one transaction
holding changesets, revisions, provenance, conflicts and the progress token.
After a crash the import restarts from the last committed token; the
idempotent writes repeat as no-ops and deterministic ids prevent duplicates.

**`Verifier`** implements the three checks of section 1.1.

### 2.2 Fit of later sources

- **MassSpec.** An adapter over the same dump-to-JSON-lines path as the DVC
  catalog. One synthesized changeset per analysis; raw peak-time data becomes
  signal blobs.
- **ArArCalc.** An adapter that emits analyses with intercepts only and no
  raw signals.

Neither requires a change to `ISourceAdapter` or `ImportBatch`. Both use
`import_source.kind = 'legacy_db'` or a new kind added by migration in their
own spec.

## 3. Store additions

The import tables (`import_source`, `import_provenance`, `import_conflict`)
already exist in `migrations/pg/0001_init.sql`. One migration,
`0002_import_detail.sql`, adds `detail jsonb` to `import_provenance`; it holds
the `synthetic_collection` flag, the extra commit shas of a folded collection
and time-zone notes. `import_source.progress_commit_sha` is used as an opaque
resume token.

### 3.1 New `IStore` surface

- `begin_import(ImportSourceSpec) -> ImportSourceId`. Upsert on
  (kind, url, branch). Returns the stored progress token.
- `begin_import_batch(source, actor) -> IImportUnitOfWork`, one transaction:
  - `add_changeset(ImportedChangeset)`: the caller sets uuid, `created_utc`,
    author, message, kind and `import_source_uuid`. This closes the gap that
    `IUnitOfWork::commit` always uses `Uuid::v7()` and `UtcTime::now()`.
  - `add_revision(...)` with caller-supplied uuid and parent. The head moves
    with reason `commit`.
  - `add_provenance(row)`, `add_conflict(row)`,
    `set_progress(token, done, total)`.
  - `commit()`: one `change_log` entry per batch, not per source commit.
- `AnalysisIngest` gains an optional `import_source`, stamped on its
  collection changeset.
- Reads: `import_sources()`, `import_conflicts(source, filter)`,
  `provenance_for(entity)`, `imported_head_blob_sha(subject, kind)`.
- Catalog: `SampleSpec` is widened to the full `sample` table. `add_load` and
  `add_load_position` are added. Every `add_*` the importer uses must be
  ensure-by-natural-key, so a re-run is a no-op. The plan's first task audits
  the current behaviour of each and fixes those that fail on a duplicate.
- `Uuid::v5(namespace, name)` is added to `ids.hpp`.

### 3.2 Id rules

| Entity | Id |
|---|---|
| analysis | legacy uuid from the file, verbatim (principle P3) |
| changeset | v5(import namespace, source url, commit sha) |
| revision | v5(import namespace, source url, commit sha, path) |
| collection root revisions | v5 from the `<COLLECTION>` commit sha and path |
| catalog row | matched by natural key; if created, v5(namespace, table, natural key) |
| import_source | v5(namespace, kind, url, branch) |
| signal blob | sha256 of content (existing) |

The source url is normalized before hashing: strip a trailing `.git` and
trailing slash, lower-case the host. Otherwise one repo cloned two ways would
produce two sets of ids.

### 3.3 Time rules

- Git commit time is the author date. It carries its own offset, so the UTC
  value is exact.
- Naive local timestamps in legacy JSON and in the MySQL dump are converted
  with `import_source.lab_time_zone`, an IANA zone that `import add` requires
  (principle P4).
- A local time that is ambiguous or nonexistent because of a DST change takes
  the earlier instant, and the fact is recorded in the provenance row. It is
  never resolved silently.
- Time zone conversion sits behind `ingest/tz.hpp`. C++20 `std::chrono` tzdb
  is used where the standard library provides it. The plan starts with a
  probe on all four CI toolchains; where it is missing, Howard Hinnant's
  `date/tz` is vendored behind the same header.

### 3.4 Idempotence and conflicts

- Import writes use deterministic ids and `ON CONFLICT DO NOTHING`. A re-run
  at the same source head changes nothing.
- A bad or unexpected file never aborts an import. It becomes an
  `import_conflict` row and the batch continues.
- An import aborts only on a store error or a git failure. The batch is
  rolled back and the token does not advance.

## 4. DVC adapter

### 4.1 Source order

`import run --all` processes sources in this order, because an analysis
cannot be ingested before its identifier and mass spectrometer exist:

1. `legacy_db`: catalog from the dump.
2. `meta_repo`: reference data.
3. each `project_repo`: analyses and their history.

### 4.2 Catalog (`CatalogDb`)

- `tools/legacy_dump_to_jsonl.py <dump.sql> <outdir>` writes one
  `<table>.jsonl` file per table, one JSON object per row, values verbatim.
  It uses the Python standard library only. JSON lines rather than SQLite
  keep a sqlite3 link dependency out of `libs/dvc` (Qt's bundled SQLite is
  private to `libs/persistence`); the catalog tables are small enough to join
  in memory.
- The adapter reads the directory with nlohmann_json and emits
  catalog specs in foreign-key order: principal investigator, project,
  material, sample, irradiation, level, position, identifier, users, mass
  spectrometers, extract devices, loads.
- `--catalog-from-repos` is an opt-in fallback for sources with no dump (the
  public fixture). It synthesizes a thin catalog from analysis JSON and the
  meta repo. Each synthesized row is recorded as an `identity_clash` conflict
  with `detail.synthesized = true`, so it is visible and reviewable.

### 4.3 Meta repo (`MetaRepoReader`)

The commit walk of section 4.4 is used. Each commit that touches reference
files becomes one `reference` changeset.

- A level file holds all positions. It is diffed per position, and only
  changed positions get a new `flux_position` revision.
- Productions, chronology, gains, irradiation holders and load holders map to
  the matching `ref_object` payloads.
- Each sensitivity list entry becomes its own revision, in list order.

### 4.4 Project repo: commit walk

```
git rev-list --topo-order --reverse <branch>     commit list; skip through token
per batch of about 500 commits:
  git diff-tree -r --root -m --first-parent      (commit, path, blob sha, status)
  skip files whose blob sha equals the imported head blob for (subject, kind)
  git cat-file --batch, distinct blob shas on stdin
  LegacyJsonLayout: path -> (runid, kind); JSON -> payload rows
  -> ImportBatch
```

`core::run_process` runs a process to completion, feeds stdin from a string,
and keeps only the first 64 KiB of output. `ProcessSpec` gains an optional
`stdout_file`: stdout goes to that file, uncapped, and stderr alone is
captured. `cat-file --batch` is run once per commit batch with the list of
blob shas on stdin, so no long-lived child process is needed. There is never
a checkout.

**Path to kind.** One table in `LegacyJsonLayout` maps each legacy file type
(analysis JSON, raw data, intercepts, baselines, blanks, icfactors, tags,
peak center, extraction, monitor, cosmogenic) to a revision kind or analysis
satellite. The file suffix decides the kind; the tag in the commit message
(`<ISOEVO>`, `<BLANKS>`, ...) is recorded only. The exact suffix strings are
taken from the legacy `dvc` source and confirmed against the fixture repo
when the table is written, with one test per entry.

**Raw signals.** The legacy `>ff` base64 series is decoded with the existing
`decode_legacy_ff_base64` and stored as an `f32le-tv/1` blob. The original git
blob sha is kept in provenance.

**Interpreted ages.** Legacy interpreted-age files become `interpreted_age`
revisions. They also carry the per-analysis ages that verify compares
against (section 6).

**Missing uuid.** An analysis file with no legacy uuid gets
v5(namespace, source url, runid), noted in provenance `detail`.

**Unknown content.** Unknown JSON keys go into the row's `extra` column
(principle P8). A file of unknown type, or one that does not parse, becomes
an `unparseable` conflict that records its sha.

**Collection folding.** For each analysis, the first add of `<runid>.json`
and the first adds of its intercepts, baselines, blanks and icfactors files
fold into one `collection` changeset, written as an `IngestItem` whose root
revisions have no parent. Author and time come from the `<COLLECTION>`
commit; every contributing commit sha is kept in provenance.

The files of one collection can fall in different batches. The adapter holds
such analyses as pending and flushes each when complete, or at the end of the
walk. The resume token advances only past commits that have no pending
analysis behind them, so a restart re-reads what it needs.

An analysis that entered a repo without a collection commit (`IMPORT`,
`MASS SPEC REDUCED`, transfers) gets a collection changeset synthesized from
its first commit, flagged `synthetic_collection` in provenance.

**Later changes.** Each later commit becomes an `import` changeset with one
revision per changed file. The parent is the current imported head.

**Deletes, renames, moves.** A deleted file produces no revision and no
row; verify counts a deletion as accounted for. An analysis whose legacy uuid is already imported from
another repo gains a `repository_member` row and is not duplicated; if its
content differs, an `identity_clash` conflict is recorded.

**Merges.** A merge commit is diffed against its first parent. History is
linearized into one chain per (subject, kind). The true ancestry stays in
`import_provenance`.

**Tags.** A git tag on commit C becomes a bookmark of the imported heads as
of C.

**Membership.** Every analysis seen in a repo becomes a member of the
`repository` named after the source.

### 4.5 Memory and performance

The blob cache is an LRU keyed by blob sha with a byte limit (default
256 MB). `diff-tree` and `cat-file` output is handled per batch. Throughput
is measured on the real-data fixture. A `COPY` path is added only if that
measurement shows a need.

## 5. CLI

`apps/elctl/src/import.cpp`, modelled on `exp.cpp`. When elctl is built
without persistence the command prints a message and exits 2.

```
elctl import add  --db <url> --kind legacy_db|meta_repo|project_repo
                  --source <path|url> [--branch <b>] --tz <IANA>
                  [--author-map <file.toml>] [--catalog-from-repos]
elctl import run  --db <url> [--source <id|name> | --all] [--batch 500] [--limit N]
elctl import status    --db <url>
elctl import conflicts --db <url> [--source <s>] [--kind <k>] [--json]
elctl import verify    --db <url> [--source <s>] [--tolerance 1e-9] [--json]
```

- `add` only registers a source and is idempotent.
- `run` is resumable. On interrupt it finishes the current batch and exits 0
  with a "paused" message.
- A remote url is fetched with `git clone --mirror` into a cache directory. A
  local path is read in place and never modified.
- One progress line per batch goes to stderr.
- Exit codes: 0 success; 1 verify failed or conflicts are pending; 2 usage
  error or fatal error.

## 6. Verify

1. **Accounting.** Walk the source again, read-only. Every (commit, path)
   must be a deletion, have a provenance row or a conflict row, or carry a
   blob sha already recorded in provenance for that path (content the
   linearized walk had already imported). Misses are listed.
2. **Idempotence.** Run the batch stream against the store without writing
   and count rows that would be new. The count must be zero.
3. **Age parity.** For each analysis with a legacy stored age (the
   per-analysis ages inside imported interpreted-age files), reduce from
   the imported heads with `libs/reduction` and compare age and error by
   relative tolerance. Each analysis is reported as pass, fail or not
   comparable (for example, missing J or blank). A failure is also recorded
   as a `value_mismatch` conflict holding both values.

## 7. Error handling

| Failure | Behaviour |
|---|---|
| Bad file content | Conflict row; continue |
| git missing or git command fails | Batch rolled back, token unchanged, exit 2 with git's stderr |
| Store error or lost CAS | Batch rolled back, exit 2; a re-run resumes |
| Identifier missing for an analysis | `unknown_analysis` conflict; the analysis is skipped and retried on the next run |
| Unknown time zone | Refused at `import add` |
| Source head moved since last run | Continue from the token; new commits are imported |
| History rewritten (token not an ancestor of head) | Stop, exit 2, report; no automatic recovery |

Fallible functions return `Result<>`. No exception crosses a library
boundary.

## 8. Testing

GoogleTest, one `test_<component>.cpp` per component. Store tests run on
SQLite and, when `PYCHRON_TEST_PG_URL` is set, PostgreSQL.

| Test | Covers |
|---|---|
| `tests/persistence/test_uuid_v5.cpp` | RFC 4122 vectors |
| `tests/persistence/test_import_store.cpp` | Caller-set uuid and time honoured; provenance and conflict round trip; progress resume; re-run is a no-op; append-only triggers still hold |
| `tests/ingest/test_batch_writer.cpp` | Fake adapter to store; failure after N batches then resume gives the same row counts as a clean run |
| `tests/ingest/test_verifier.cpp` | Accounting gap detected; idempotence; parity pass, fail, not comparable |
| `tests/ingest/test_tz.cpp` | Zone conversion, ambiguous and nonexistent local times |
| `tests/dvc/test_legacy_layout.cpp` | Path-to-kind table; each JSON kind to payload rows; unknown keys to `extra`; garbage to `unparseable` |
| `tests/dvc/test_git_reader.cpp` | Repo built in `SetUp` with real `git` and fixed author and date: linear, merge, tag, delete, rename. Skipped with a message when `git` is absent |
| `tests/dvc/test_project_import.cpp` | End to end on a synthetic repo: collection folding, including across batches; synthetic collection; refit chain; same uuid in two repos |
| `tests/dvc/test_meta_import.cpp` | Per-position flux diff; sensitivity list order |
| `tests/dvc/test_catalog_db.cpp` | Checked-in small JSON-lines fixture; foreign-key order; widened `SampleSpec` |
| `tools/tests/test_legacy_dump_to_jsonl.py` | Small mysqldump sample to JSON lines |
| `apps/elctl/tests/test_import_cmd.cpp` | add, run, status, verify through `elctl::run`; exit codes |

**Real data.** `tools/import_fixture_check.sh` clones a pinned NMGRLData
project repo and the MetaData repo, then runs import and verify with
`--catalog-from-repos`. It is run by hand, not in CI, because it needs the
network. The first task of the plan chooses the project repo and commit sha
and records them in schema design section 13.3 Q1.

All tests must pass under ASan and UBSan on every CI compiler. The subprocess
`stdout_file` path needs a test on MSVC.

## 9. Updates to existing documents

- Schema design section 13.3: mark Q3, Q4 and Q5 resolved as in section 1.2
  here; fill Q1 when the fixture is chosen.
- Schema design section 10.3: catalog rows come from a dump of the legacy
  MySQL database, not a live connection.
- Schema design section 12.1: `libs/dvc` holds the pychron adapter;
  `libs/ingest` is added; the CLI family is `elctl import`, not
  `elctl dvc import`.
- Plan `2026-10-02-dvc-persistence.md` stage D4 points at the plan written
  from this spec.

## 10. Amendments after the layout survey

The survey of the legacy source and public repos
(`tests/dvc/fixtures/README.md`, the authority on layout) changed these
points. Where this section and an earlier one disagree, this section wins.

1. **Path key.** A path yields a key that is a runid or, in newer repos, a
   uuid; the directory prefix length varies (2 to 5). The key is the prefix
   directory name joined to the file stem. Kind comes from the modifier
   directory and suffix. The raw-data suffix is `.dat.json`.
2. **More file kinds.** Root-level spectrometer settings files
   (`<40 hex>.json`) are imported as spectrometer snapshots of the analyses
   that name them. Root-level frozen production files
   (`<irradiation>.<level>.production.json`) are imported as `production`
   reference objects named `frozen/<repository>/<irradiation>/<level>`; using
   them in reduction is not part of this spec. Run logs
   (`logs/*.logs.log`) are skipped and count as accounted for; importing them
   as artifacts is deferred. Only a path matching no known pattern is an
   `unparseable` conflict.
3. **Tags without tag files.** Later legacy versions kept tags only in MySQL
   (`AnalysisChangeTbl.tag`). When an analysis has no tags file and a catalog
   dump is registered, the root tag comes from the dump, noted in provenance
   `detail`. With neither, the tag is `ok`.
4. **Two interpreted-age formats** (2018 flat, later nested). Both are
   parsed.
5. **References to analyses in other repos.** Blanks live in per-spectrometer
   repos. A blank or IC-factor reference to an analysis not yet in the store
   is kept in the row's `extra` and noted in provenance `detail`; it is not a
   conflict.
6. **Age parity is as of the interpreted age.** A stored age was computed
   from the reduction state when the interpreted age was saved; later edits
   (the fixture repo has a bulk IC-factor rescale three years later) change
   the heads. Verify therefore reduces each member analysis from the
   revisions that were head at the interpreted-age commit, with reference
   data as of that commit. When that state cannot be reproduced, the analysis
   is reported as not comparable with the reason, never as a pass.
7. **Retrying skipped analyses.** An analysis refused for a missing catalog
   row (`unknown_analysis`) does not hold the resume token back. After the
   catalog is fixed, `import run --replay` walks the source again from the
   start; deterministic ids make everything already imported a no-op and the
   skipped analyses and their later revisions are written in order. When a
   replay writes an entity that has a pending `unknown_analysis` conflict,
   the conflict's resolution becomes `superseded`. Only `pending` conflicts
   make verify fail.
8. **Conflicts are matched by id.** A conflict's uuid is
   v5(namespace, source url, commit sha, path). Accounting looks a conflict
   up by that id, not by file hash. A refused analysis gets one
   `unknown_analysis` conflict per file of its collection, so each of its
   files is accounted for.
9. **Bookmarks and groups take a caller-supplied id** (v5 of source url and
   tag name) with ensure semantics, so a crash between creating a bookmark
   and committing the batch cannot duplicate it.
10. **Collection changeset id.** One commit can add several analyses, so a
    collection changeset's id is v5(namespace, source url, commit sha,
    analysis uuid), distinct from the commit's `import` changeset id.
11. **Files rewritten after collection.** A later commit that rewrites an
    analysis record or a satellite file (legacy `<SYNC>`, `<EDIT>`,
    `<DEFINE EQUIL>` and manual commits) is not a conflict. If identifier,
    aliquot or increment changed, it becomes an `identity` revision. A
    rewritten raw-data file becomes a `signals` revision. Every other
    difference is kept in the provenance `detail` of that commit's `import`
    changeset as `rewrites`: path, blob sha and the changed keys with old and
    new values. `hand_edit` stays reserved for re-import of a published repo.
12. **Merges.** Diffing a merge against its first parent alone leaves the
    wrong head when the merge keeps the first parent's version of a file the
    other side changed. At a merge commit the adapter also diffs the merge
    against each other parent; for every path that differs, if the merge's
    blob is not the blob last imported for that path, the merge emits a
    revision carrying the merge's content. After a merge, every imported
    head equals the merge tree.
13. **Resume position.** The resume token records the commit sha and its
    index in the `rev-list --topo-order --reverse` order. On resume the list
    is recomputed; if the commit at that index is the token's sha the walk
    continues after it, with in-walk state rebuilt from the commits before
    it. If it is not (history rewritten is an error; order merely changed is
    not), the walk replays from the start, which deterministic ids make
    safe.
14. **Bounded wait for a collection.** An analysis whose collection is still
    incomplete 20 commits after its record commit is flushed as a
    `synthetic_collection` with the files it has. Files that arrive later are
    ordinary revisions. One incomplete analysis therefore cannot hold the
    resume token for the rest of the walk.
15. **Identity guards survive a resume.** A run id already used by another
    analysis, or (with `--catalog-from-repos`) a position already holding
    another identifier, is an `identity_clash` conflict whether the earlier
    analysis was seen in this walk, an earlier run, or another source. It
    never aborts the import.
16. **One history, one result.** However the walk is cut into batches,
    interrupted, resumed, run incrementally or replayed, the store ends in
    the same state as one uninterrupted import of the same history. In
    particular a renumbered analysis frees its run id for a later analysis
    whether the two fall in one batch or several; a replay never refuses an
    analysis this source already imported; a byte-identical second copy of an
    analysis in the same source is membership only, a differing one is an
    `identity_clash`; and all `rewrites` of one commit are kept even when the
    commit's effects reach the writer in more than one batch.
17. **Resume token carries a prefix hash.** Besides sha and index the token
    holds a hash of the commit shas before it; a different prefix means
    replay from the start.
18. **Large rewritten values.** A value in `rewrites` larger than 64 KiB is
    stored as a reference (git blob sha and byte count), not verbatim.
19. **Known limits.** Topological order can place commits of a branch not
    yet merged before an unrelated merge, so a bookmark made from a tag on
    that merge can include a value from the unmerged branch; heads are
    correct once the branch is merged. Analyses still incomplete when an
    incremental run ends are folded as synthetic collections, where one
    longer uninterrupted import would have folded them complete.
    Within a single commit: two analyses that swap run ids both become
    `identity_clash`; a collection in the same commit as the renumber that
    frees its run id is refused (a replay then imports it). An analysis
    renumbered while its collection is still pending is folded under its
    later identity with no `identity` revision; the rewrite is kept under
    `rewrites`. A record without a uuid that is renumbered reads as a
    different analysis (`identity_clash`).
20. **Token rule, revised.** Every batch moves the resume token to its last
    commit; the token no longer waits for pending analyses (this replaces
    the token sentence of section 4.4 and the last sentence of item 14). On
    resume, analyses still pending at the token are rebuilt by re-reading
    the paths, not the contents, of the commits before it.
21. **Removed reference data.** When a position, a level's production, a
    sensitivity list, or a whole reference file disappears from the meta
    repo, the legacy system has no value there. The import does not leave
    the old value as head: it writes a revision whose value is explicitly
    absent, with `removed` in provenance detail, so a reduction sees "no
    value", not a stale one. Where a payload type cannot express absence the
    removal is recorded in the changeset's provenance detail and the old
    head stays; that case is a documented limit. Deleted analysis files in a
    project repo are unaffected: the analysis stays.
22. **Chronology is strict.** A chronology line that looks like a dose but
    cannot be read makes the file an `unparseable` conflict; a chronology
    stored with fewer doses would silently change decay corrections.
23. **Catalog rows with a bad optional link.** A catalog row is refused only
    when the store cannot hold it (a required parent is missing or was
    refused). When the broken link is optional in the store (a position's
    sample, a project's principal investigator, a load's user) the row is
    imported with the link absent and an `identity_clash` conflict records
    the broken link. One unusable parent therefore does not remove its whole
    subtree.
24. **String keys in the legacy database** are matched as MySQL's default
    collations match them: ignoring case and trailing spaces. The parent's
    own spelling is what is stored.
25. **Catalog resume token** is `<table index>:<row index>@<manifest sha256>`,
    so a directory converted again from a newer dump does not resume at a
    stale offset.
26. **What fails verify.** Verify fails on pending conflicts that mean data
    was not imported or does not agree: `unparseable`, `unknown_analysis`,
    `value_mismatch`, and `identity_clash` rows that refused something.
    Conflicts that only annotate an imported row (`imported: true` for a
    broken optional catalog link, `synthesized: true` for a catalog row made
    from repo contents) are listed as warnings and do not fail verify.
27. **Accounting has no blob shortcut** (replaces the last alternative of
    section 6 item 1). A unit is accounted for by a row at its own
    (commit, path), by its conflict, or, when the walk itself determines that
    it repeats content already imported, by the specific earlier unit it
    repeats. "The same blob exists somewhere in provenance for this path" is
    not evidence: it would hide a revision dropped from an X, Y, X history.
28. **Verify needs a finished import.** Verify reports the source's
    registration and status and is not ok unless the source is registered
    and its last run finished. For a catalog source this is what ties "the
    rows exist" to "this dump was imported". Known limit: when a catalog row
    already existed (the existing row wins), verify cannot tell that the
    dump's other columns were not applied.
29. **Late spectrometer file.** A spectrometer settings file that first
    appears after the analysis naming it was collected cannot be attached
    (the analysis row is immutable). It is an `unparseable` conflict with
    reason `spectrometer_file_after_collection`, not a silent skip.
30. **"As of" is a position in the walk, not a time.** Git author dates tie
    and run out of order. The state as of an interpreted age is, for each
    kind, the last revision whose source commit is at or before the
    interpreted age's commit in the walk order of that source. Members from
    another source are not comparable.
