# Persistence — ADR-0002: Database authoritative, git as mirror

Date: 2026-09-29
Status: Proposed
Deciders: Jake Ross
Depends on: `2026-09-29-instrument-control-design.md` (phase plan, repo
layout, config conventions).
Scope: the persistence layer for the later "DVC" phase of the C++ rewrite:
where analyses, reduction results, and lab reference data (flux, productions,
gains, scripts) live, and how they reach collaborators and public archives.
Out of scope: experiment engine, reduction math, UI.

## 1. Context

Pychron (Python) persists analytical data through three mutable stores, all
touched synchronously during data acquisition:

1. **SQL catalog** (`pychron/dvc/dvc_orm.py`). Analyses, samples, projects,
   irradiations, and a repository-association table that maps each analysis
   to the git repo holding its files. It is also the *coordination
   authority*: the next aliquot and step for an identifier are assigned by
   querying it (`dvc_database.py:1376`, `:1400`). Git cannot perform that job.
2. **Project repos** (one git repo per project, hosted on GitHub or a local
   git host). Each analysis is 6 JSON files (`<runid>.json`, `.intercepts`,
   `.blanks`, `.baselines`, `.icfactors`, `.data`) plus optional extraction,
   peak-center, monitor, log, and spectrometer-snapshot files
   (`dvc_persister.py:882-905`). Raw signal arrays are base64 blobs inside
   `.data.json`. Reduction results overwrite the per-analysis files in place.
3. **Meta repo** (one global git repo). Flux per irradiation position,
   production ratios, chronologies, detector gains and sensitivities,
   measurement scripts, holder geometries (`meta_repo.py`). Every instrument
   in the lab writes to it.

### 1.1 What the git model has delivered

- Citable, public, human-readable archives of every analysis (e.g. the
  NMGRLData GitHub organization). Data survives the software.
- Free hosting and off-site backup.
- Per-commit audit trail of reduction decisions with author and message;
  bookmarks and rollback-to-collection are built on it.
- Offline reading of a project with nothing but a clone.
- Cross-lab sharing without a shared database server.

### 1.2 What it costs

All items below are cited from the Python code, not inferred.

| Cost | Evidence |
|---|---|
| Per analysis: 4 add/commit cycles, a push, then meta pull, meta commit, meta push, all inside the acquisition save | `dvc_persister.py:272-458` |
| Save latency was a known problem: timings are instrumented and appended to `save_timing.csv` | `dvc_persister.py:480-512` |
| A failed push raises a "cancel the experiment?" prompt | `dvc_persister.py:423`, `:454` |
| Meta repo is a global write hotspot; every save pulls it with accept-ours | `dvc_persister.py:429` |
| `index.lock` contention handled by deleting the lock file and retrying | `git_archive/repo_manager.py:1520-1532` |
| Diverged repos are "repaired" by fetch + reset, discarding local commits | `dvc/repository_sync.py:27-80` |
| JSON files are unqueryable, so intercepts, blanks, baselines and ages were duplicated into a `CurrentTbl` shadow table behind a preference flag | `dvc_persister.py:667`, `dvc.py:1916-2047` |
| Loading analyses first syncs every involved repo, throttled by a pull-frequency cache, then goes through an in-memory LRU with a 15-minute TTL | `dvc/analysis_loading.py:62`, `dvc/repository_sync.py:98`, `dvc/cache.py` |
| Workaround modules: abandoned SQLite index, offline mode that clones repos and converts MySQL to SQLite, rsync transport as a git alternative, a `fix/` package of repair scripts | `dvc/offline_index.py`, `dvc/work_offline.py`, `dvc/rsync.py`, `dvc/fix/` |
| Package weight: ~12k lines in `pychron/dvc` plus ~1.5k in `git_archive/repo_manager.py`; the package's own doc calls it a "high-risk integration seam" | `pychron/dvc/architecture.md` |

The shadow value table is the decisive symptom. A catalog-only database was
not enough for the browser and pipeline, so values leaked into SQL and the
system now has two sources of truth for the same numbers.

### 1.3 Diagnosis

The pain is not git. It is three specific uses of git:

1. As a **write-ahead store** on the acquisition path (network and lock
   contention in the hot loop).
2. As a **query engine** for reduction and browsing (unqueryable, so caches,
   pre-load syncs, and the shadow table).
3. The **meta repo as shared mutable state** written by every instrument.

Everything git is genuinely good at (archive, exchange, audit, diff) survives
if git is moved off those three jobs.

## 2. Decision

The relational database is the single source of truth for the catalog, the
analytical values, the reduction history, and the lab reference data. Git
repos become a **mirror**: materialized from the database by a background
publisher in the existing JSON layout, and readable back through an importer.
No git operation runs on the acquisition path or the query path.

Chosen over: status quo (A), database-only with no git (B), files with a local
index and no git (D). See section 3.

## 3. Options considered

### A. Status quo: DB catalog + git values + meta repo

| Dimension | Assessment |
|---|---|
| Complexity | High. Three stores, two servers (SQL + git host), credentials for both. |
| Performance | Poor. Section 1.2. |
| Provenance | Strong, per commit. |
| Sharing / offline | Strong via GitHub; offline needs the clone-and-convert dance. |
| Team familiarity | High. |

Pros: proven over a decade; existing public repos; reviewers know it.
Cons: everything in 1.2; the rewrite would re-implement the workarounds.

### B. Database only, versioned rows, no git

| Dimension | Assessment |
|---|---|
| Complexity | Low. |
| Performance | Best. |
| Provenance | Good if revision rows are immutable. |
| Sharing / offline | Weak. Requires a shared server or dump files; no public citable archive. |

Pros: one store, one transaction per analysis.
Cons: loses the public archive and the human-readable file format. Not
acceptable for a lab whose published data lives in git today.

### C. Database authoritative, git as async mirror (chosen)

| Dimension | Assessment |
|---|---|
| Complexity | Medium. One authoritative store plus a publisher and an importer. |
| Performance | Best on the write and query paths; git cost moves to a background job. |
| Provenance | Immutable revision rows in the DB, mirrored as commits. |
| Sharing / offline | Kept. Repos still exist on GitHub; offline is a SQLite file or a clone. |

Pros: removes every item in 1.2 while keeping every item in 1.1. Existing
repos stay readable and continue to be produced in the same layout.
Cons: two-way sync is not free; see section 5.

### D. Files on disk + local SQLite index, no git

| Dimension | Assessment |
|---|---|
| Complexity | Low to medium. |
| Performance | Good. |
| Provenance | Weak. No diff, merge, or history tooling; would have to be built. |
| Sharing / offline | Medium. rsync or cloud storage. |

Pros: simple. Cons: reinvents the half of git that is actually valuable.

### E. Patch A in place (Python only)

Batch to one commit per analysis, push on a background thread, take the meta
repo out of the save path, delete the shadow table. Not a rewrite option, but
the correct move for the Python codebase while it remains in service.

## 4. Design shape

### 4.1 Store

- Engine: SQLite for single-instrument and offline use; PostgreSQL for a
  multi-instrument lab. Same schema, one access layer. MySQL is already a
  hard requirement of the Python system, so requiring a server for the
  multi-instrument case is not a regression.
- Catalog tables carry over conceptually from `dvc_orm.py`: principal
  investigator, project, sample, material, irradiation, level, position,
  identifier, load, analysis, analysis group, user, mass spectrometer,
  extraction device.
- **Analysis values** live in the database, not in files: per-isotope
  intercept, baseline, blank, IC factor, fit descriptors, and derived ages.
- **Reduction revisions** are append-only. Each write of blanks, fits, IC
  factors, tags, flux, or interpreted ages creates a new revision row with
  author, timestamp, message, and a parent pointer. A per-analysis "current
  revision" pointer selects the active values. Rollback is a pointer move.
  This replaces git history as the audit trail.
- **Reference data** (flux, productions, chronology, gains, sensitivities,
  holder geometries) is versioned the same way. There is no meta repo as a
  live object; there is a mirrored export of it.
- **Raw signals** (signal, baseline, sniff arrays, peak centers) are stored
  content-addressed by hash, either as DB blobs or as Arrow/Parquet files
  beside the database, referenced by hash from the analysis row. They are
  immutable after collection.
- Coordination (next aliquot, next step, identifier reservation) is a DB
  transaction, as it is today.

### 4.2 Publisher

- A background service, never invoked from acquisition or reduction code.
- Materializes each project repo in the existing pychron JSON layout
  (`<repo>/<head>/<runid>.json` and siblings, `extraction/`, `tags/`, and so
  on) so that current tooling and existing public repos remain valid.
- One commit per batch (configurable: per analysis, per N minutes, on
  demand), pushed when a remote is reachable. Failure to push is a queued
  retry, never a prompt to the operator.
- Also exports the meta mirror from the versioned reference tables.
- Commit author and message come from the revision rows, so the git history
  mirrors the DB audit trail one-to-one.

### 4.3 Importer

- Reads the current repo JSON layout into the database. Required before
  anything else: roughly a decade of data exists only in those repos.
- Also the path for cross-lab exchange: a collaborator's repo is imported,
  not mounted.
- Direction of truth is one way. A repo edited by hand outside the
  application is treated as an import with explicit conflict reporting, not
  as an authoritative change.

### 4.4 Offline

Offline work is a SQLite copy of the relevant projects, produced by the
application, with local signal files. No git clone is required to read data
offline, although a clone still works through the importer.

## 5. Trade-offs and consequences

Easier:

- Acquisition save becomes one local transaction. No network, no locks, no
  operator prompt.
- Browser and pipeline queries hit indexed values directly. The shadow table,
  the LRU cache, the pull-frequency cache, and the pre-load repo sync all
  disappear.
- Multiple instruments never contend on a shared git repo or the meta repo.
- Offline is a file copy.

Harder:

- Two-way sync. Deliberately not supported. The DB writes, git mirrors.
  Manual edits in a repo are imports with conflict handling.
- Reviewer trust. Some users treat git history as the audit trail. The
  revision table is the audit trail; the mirrored commits reproduce it. This
  needs documenting.
- Meta repo workflow. Editing flux or productions becomes a database edit
  that appears in the meta mirror on the next publish, rather than a commit
  to a shared repo.
- Infrastructure. A multi-instrument lab needs PostgreSQL rather than MySQL.
  Same class of dependency as today.

Revisit later:

- Signal blob format (DB blob vs Arrow files) once real volumes are measured.
- Whether the mirrored JSON layout should be frozen as a versioned exchange
  format with a schema, independent of pychron.
- Whether reference-data edits need a review or approval step before they
  publish.

## 6. Action items

1. [ ] Accept or amend this ADR before the DVC phase begins.
2. [ ] Write the schema spec: catalog, values, revision tables, reference
       data, signal store. Immutable revisions first.
3. [ ] Build the importer against real NMGRLData repos and prove a full
       project round-trips into the database with identical derived values.
4. [ ] Build the publisher and prove the exported repo is byte-comparable
       to the imported one for unchanged analyses.
5. [ ] Define the offline SQLite export.
6. [ ] Python pychron, in parallel and independent of the rewrite: apply
       option E (one commit per analysis, background push, meta repo out of
       the save path, remove `CurrentTbl`).
