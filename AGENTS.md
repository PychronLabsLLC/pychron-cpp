# Agent guide

## Workflow

Single developer. Two long-lived branches:

- `develop` is the integration branch: all work lands here first.
- `main` is what has been released or is about to be. Nothing is pushed to
  it: it changes only by pull request. GitHub does not enforce this yet
  (branch protection needs a paid plan for a private repository), so the rule
  is kept by hand.

Day to day:

- Work on a branch (or worktree) cut from `develop`, named for the kind of
  change: `feat/<topic>`, `fix/<topic>`, `chore/<topic>`, `docs/<topic>`,
  `refactor/<topic>`, `test/<topic>`, `ci/<topic>`.
- Commit messages follow Conventional Commits, with the component as the
  scope: `feat(ui): a heaters dock`, `fix(canvas): ...`, `docs: ...`. The
  release notes and the next version are computed from them: `feat` is a minor
  bump, `fix` and `perf` a patch, and `feat!:` / `fix!:` or a
  `BREAKING CHANGE:` footer a breaking change (still a minor bump while the
  version is 0.x). `chore`, `docs`, `refactor`, `test`, `ci` and `build`
  release nothing on their own.
- To land: rebase the branch on `origin/develop`, run the static analysis
  (`python3 tools/quality_check.py`, below) and the tests on the rebased
  branch, merge into `develop` and push. No pull request is needed for
  `develop`. If `origin/develop` moves again before the push, that is another
  rebase and another run: what is pushed is what was tested.
- CI does not run on `develop` or on work branches, only on `main` and on pull
  requests into it. The tests you run locally are the only ones before a
  release: run them.
- Never skip or disable a failing test; find the root cause.
- Never push to `main`, and do not open a pull request into `main` or merge
  one unless asked to release.

Releasing (`.github/workflows/release-please.yml`):

1. Open a pull request from `develop` into `main`. CI runs on it.
2. Merge it with a merge commit, never a squash: release-please reads the
   individual commits.
3. release-please opens (or updates) a release pull request into `main` with
   the next version, `CHANGELOG.md`, and the version in `version.txt`,
   `CMakeLists.txt` and `vcpkg.json`. Do not edit those by hand.
4. Merging the release pull request tags `vX.Y.Z` and publishes the GitHub
   release; the `release` workflow builds the installers and attaches them.
5. `main` is merged back into `develop` by the same workflow. If that job
   fails on a conflict, merge `main` into `develop` by hand.

A hotfix is a `fix/` branch cut from `main` and merged into it by pull
request; it reaches `develop` through step 5.

## Build and test

See `docs/dev_setup.md` for setup and `CMakePresets.json` for presets (CI uses
`dev`, and `dev-ui` for the UI job). Tests are GoogleTest, one `test_<component>.cpp` per component under
`tests/`, run with `ctest`.

- CI (`.github/workflows/ci.yml`) builds with clang + ASan/UBSan on macOS,
  gcc 14 and clang 18 + ASan/UBSan on Ubuntu 24.04, and MSVC.
- `-DPYCHRON_SANITIZE=address,undefined` enables ASan/UBSan, and with them
  the standard library's own checks (an empty optional dereferenced, an index
  past the end): static analysis does not look for those here, this does.
- gcc 13 (the Ubuntu 24.04 default `g++`) warns where the CI compilers do not;
  build it with `-DPYCHRON_WARNINGS_AS_ERRORS=OFF`.
- `-DPYCHRON_SCRIPTING=OFF` drops the embedded CPython dependency.
- `-DPYCHRON_VISION_OPENCV=AUTO|ON|OFF` (default `AUTO`) controls the optional
  OpenCV in `libs/vision` (`LegacyFinder`, `OpenCvSource`); without it those
  two files compile to stubs. OpenCV headers appear only in those two `.cpp`
  files. Only the macOS `ui` CI job installs OpenCV (Homebrew's brings TBB,
  which crashes at exit under the sanitizers).
- `libs/persistence` (DVC store, TinyORM on QtSql) builds only when Qt6 Core
  and Sql are found; `-DPYCHRON_PERSISTENCE=OFF` skips it. On Ubuntu:
  `apt install qt6-base-dev libqt6sql6-sqlite libqt6sql6-psql`. Qt must not
  appear in its public headers or in any other `libs/` library.
- Persistence tests always run on SQLite. Set
  `PYCHRON_TEST_PG_URL=postgresql://user:pw@host/db` to run them on
  PostgreSQL as well (each test uses a throwaway schema).
- `libs/ingest` and `libs/dvc` build only with persistence; their tests (and
  `elctl`'s import tests) need `git` >= 2.32 on PATH and `tzdata`. The
  real-data check is `tools/import_fixture_check.sh build/dev` (network, not
  in CI); user docs are in `docs/legacy_import.md`.
- Importer rules that are easy to break (spec section 10 of
  `docs/superpowers/specs/2026-10-03-legacy-ingestion-design.md`): the store
  must end the same however a walk is cut, stopped, resumed or replayed, so
  an adapter decides from the walk (paths and commits), never from what an
  earlier run happened to leave in memory, and never orders anything by
  address. Each adapter's `OneHistoryOneResult` test holds one history with
  everything that has gone wrong at a batch boundary; a change to a walk or
  to the writer adds its case there (`same_at_every_cut` in
  `tests/dvc/verify_support.hpp` does the same for a small history). Ids
  derive from the normalized source url and from commit and path: changing
  `normalize_source_url` or an id recipe orphans every existing import. The
  words that make a conflict a warning live in
  `libs/ingest/include/pychron/ingest/conflict_markers.hpp`.
- `GitFixture` switches the user's and the machine's git configuration off
  for the test process (`GitReader::mirror` reads it); a test that wants one
  sets `GIT_CONFIG_GLOBAL` itself. No test may run git against this
  repository.
- One importer at a time per database. Windows CI builds without
  persistence, so `libs/ingest`, `libs/dvc` and `elctl import` are not built
  or tested there.
- The schema source is `libs/persistence/migrations/pg/`. After editing it,
  run `python3 tools/ddl_sqlite.py` and commit the regenerated SQLite file.
  Never edit an applied migration; add `NNNN_<name>.sql`. A statement only
  PostgreSQL understands is preceded by `-- @sqlite skip`; its SQLite
  counterpart, when one is needed, is given as `-- @sqlite exec <statement>`.
- Before adding a migration or a query against the store, read "Schema and
  queries" below: a migration costs every lab a step, and a query's cost is
  paid once per analysis.
- A sample's location is one PostGIS `geometry(Point, 4326)` column, `geom`
  (migration 0004); SQLite keeps the same point as EWKT text. The catalog API
  still speaks `lat` and `lon` (`SampleFields`, the `lat`/`lon` edit fields):
  `libs/persistence/src/sql/geometry.hpp` converts, and every read of the
  column goes through `geom_read()` (`ST_AsEWKT` on PostgreSQL). Half a point
  is refused. PostgreSQL needs PostGIS; the tests on it do too.
- The publication data report (`libs/processing` `report.hpp`, Schaen et al.
  2021) is Qt-free and reads only the `Analysis` model: metadata it needs
  (sample location and lithology, the flux monitor, the reactor) is carried
  by `Analysis::sample_info`, `Analysis::monitor` and
  `ReductionContext::reactor`, filled by the store source. A column added to
  a table gets a row in `tests/processing/test_report.cpp`; the CSV must stay
  RFC 4180 and every row the width of its header. `elctl export` is split
  into `export.cpp` / `export_stub.cpp` like `import`. User guide:
  `docs/export.md`.
- `libs/entry` (sample and package entry) builds only with persistence, like
  `libs/ingest`. Entry writes catalog rows only through
  `IStore::apply_catalog_edits` (field-value compare-and-swap, one
  transaction) and identifiers only through `allocate_identifiers`; never
  through ad hoc UPDATEs. User guide: `docs/entry.md`.
- Flux fitting: the math is `libs/reduction` `flux.hpp`, the pure fit of a
  level `libs/processing` `flux_fit.hpp`, the monitor sets and
  `load_level` / `save_level` the `processing_store` adapter's
  `flux_store.hpp`. `tests/reduction/flux_golden.hpp` is generated by
  `tools/flux_reference.py`, never edited by hand. The legacy model strings
  and the changeset message `fit flux for <irrad><level>` are a file format.
  A position's hole is `ordinal + 1`, never `hole_id`. `elctl flux` is split
  into `flux.cpp` / `flux_admin.cpp` / `flux_stub.cpp`. The flux store tests
  (`tests/processing/test_flux_store.cpp`) run on PostgreSQL too when
  `PYCHRON_TEST_PG_URL` is set; `elctl`'s flux tests run on SQLite only. A
  level's saved fit (monitor set, sample, all positions) is one revision's,
  and is repeated only under its own monitor set. User guide: `docs/flux.md`.
  The flux window (`apps/pychron-ui` `flux_window.cpp`) computes nothing: its
  scene, options schema and status line are in `libs/processing`
  `flux_view.hpp` and are the window's own; the warnings and the CSV text
  there are shared with `elctl flux`.
- An instrument install has a database and setup seeds it from the install's
  `seed.toml` (`libs/entry` `seed.hpp`; `elctl entry seed` by hand): the
  `references` project, a sample and special identifier for each kind of
  reference run, the Triga production ratios (those of the NM-293 fixture).
  The seed only ensures: it never edits or removes a row or a reactor that
  exists, and a failed seed never fails an install. `seed.toml`,
  `defaults.toml` and the example queue name the same samples;
  `tests/setup/test_profiles.cpp` holds them together. `libs/setup` only
  computes the database url; the apps open the store.
- A file the application writes for the user to open elsewhere (a report, a
  figure, a template, a sheet) goes through `pychron::mark_as_user_file`
  (`libs/core` `user_file.hpp`) after it is written: a downloaded, unsigned
  macOS application quarantines what it writes, and Gatekeeper then refuses
  the file. Files pychron reads back itself (configs, stores) do not.
- The macOS `.dmg` is signed (hardened runtime) by `packaging/macos/sign_app.sh`
  from CPack (`packaging/macos/cpack_sign.cmake`, identity in
  `PYCHRON_CODESIGN_IDENTITY`, `-` for ad hoc), then signed, notarized and
  stapled in `release.yml` when the signing secrets are set
  (`docs/installation_runbook.md` section 1.4). Something the programs newly
  need under the hardened runtime (a device, a kind of library) is an
  entitlement in `packaging/macos/entitlements.plist`.
- The simulated lab's physics is `libs/sim` `GasNetwork` on `LinearFlow`: gas
  by species in volumes, solved exactly between valve events, pure and
  clock-free (standard library and `core/error.hpp` only). `SimSystem` holds
  the clock, the mutex and the device sims and moves the network to
  `clock.now()` on each query. The noise on a reading is keyed by (seed,
  name, time) (`keyed_noise.hpp`: `keyed_gauss` for Faradays and gauges,
  `keyed_poisson` for counters, one key for one of the two), so a simulated
  run gives the same numbers every time: no code draws simulator noise from
  a shared generator or a `<random>` distribution. Gas enters through
  `SimSystem::inject` or a volume's source term, never through the beam;
  `feed_beam_from_line` (`libs/systems` bringup) is the one place a beam is
  joined to a line, and the two share a clock that outlives the beam.
  Equilibration takes time: a test that opens a valve advances the clock
  before it reads, and zeroes outgassing where it asserts an exact hold. A
  `sim.toml` key is read and range-checked in `sim_config.cpp`, listed
  commented out with its default in `configs/examples/sim.toml`
  (`SimConfig.TheExampleFileIsTheDefaults` holds the two together) and given
  a row in the user guide, `docs/simulator.md`. The example lab's five tuned
  numbers are pinned by that test too, and a line number of
  `sim_extract.py` by the script-editor test (`tests/ui/test_script_editor.cpp`),
  so retuning the example or editing that script means editing those tests.
  `[defaults] seed` is the one seed of the lab: `feed_beam_from_line` gives
  it to the beam when it is not `SimSettings`' default.
- `libs/metrics` (the Prometheus endpoint, `[metrics]`) is Qt-free and builds
  everywhere; `-DPYCHRON_METRICS=OFF` skips it and the exporters. Metrics come
  from bus events (`CoreExporter`, `ExperimentMetrics` in `libs/experiment`),
  not from instrumenting the control path. Three rules: a label value is a
  configured name or an enumeration, never a run id, an identifier or a
  message, and a counter is created at zero as soon as what it counts is
  known (one that first appears at 1 shows no increase); a duration is a
  difference of event `ts` (the line's clock may be simulated) and an age is
  measured here on `RealClock` and exported as seconds, never as a timestamp
  for the box to subtract from its own clock; a
  metric added to an exporter needs a panel in
  `packaging/observability/grafana/dashboards`, and a renamed one its panel
  renamed (`MetricsPackaging` in `tests/integration` fails otherwise). The
  tests check names, not that a query returns anything or that Grafana
  accepts a file: after changing a dashboard or the alert, load it in the
  virtual box (`packaging/observability/box`, `docker compose up -d`) and
  look. User guide: `docs/observability.md`.
- File > Preferences keeps the line's `[logging]` and `[metrics]` in the
  line's local override file, not in QSettings: `elctl` must follow them.
  `config::replace_local_table` (`libs/core` `local_file.hpp`) is the only
  thing that writes that file after the setup wizard; it replaces one table
  and leaves the rest byte for byte. A table read over another (the local
  file's over the main one's) must leave alone every key it does not name:
  a new key in `parse_logging` or `parse_metrics` gets a case in
  `ConfigOverride.Local...KeysWinAndTheRestStay`.
- Ubuntu 24.04's cmake 3.28 is too old for this tree (`pip install cmake`).

Compilers disagree about undefined behaviour: a test that passes under clang
can abort under gcc, and the reverse. A failure on one compiler only is a real
bug until shown otherwise.

## Schema and queries

The store (`libs/persistence`) is read one analysis at a time: a figure of
2000 analyses runs every per-analysis statement 2000 times. In October 2026
an ideogram of 24 analyses took 12 s, and none of it was calculation
(reduction 3 ms, the figure 1 ms): it was four mistakes in how rows were
found. The rules below are those mistakes, generalised. The commits are
`57621f0` and `1786fa8`.

Sizes to think with, from one lab's imported store: 8.7 thousand analyses,
312 thousand revisions (about 36 an analysis), 21 thousand reference objects,
19 thousand irradiation positions, 15 thousand identifiers, 6.5 thousand
samples. A lab that has run for a decade is ten to a hundred times that. A
test fixture has five rows of each, where every plan is instant: a query
that passes its test has told you nothing about its cost.

### Before adding a migration

- Every migration locks every existing store out of `pychron-ui`, `export`,
  `flux` and `entry` until someone runs `elctl db migrate --db <url>` on it
  (they open with `migrate = false` and refuse a schema that is behind). So:
  add one when it earns that step, put what belongs together into one
  migration rather than three in a week, and give the commit a
  `BREAKING CHANGE:` footer that says to run the command.
- Prefer a change old builds can live with. A build checks only the
  migrations it carries and ignores later ones, so a migration that only
  adds (an index, a nullable column, a table) leaves a migrated store usable
  by the build before it. One that drops, renames or tightens does not:
  labs then cannot go back.
- Can the query be written so it needs no schema change? Ask first. The
  sample counts were going to get two indexes; rewritten to count once
  instead of per row they needed none, and were quicker than with them.
- The mechanics: the source is `libs/persistence/migrations/pg/NNNN_<name>.sql`;
  run `python3 tools/ddl_sqlite.py` and commit the SQLite file it writes;
  never edit an applied migration; `-- @sqlite skip` and `-- @sqlite exec`
  for what only one engine understands. `SchemaTest.MigrateIsIdempotentAndRecordsChecksums`
  lists the migrations by number and name: add yours.

### Indexes

- A foreign key is not an index. Neither PostgreSQL nor SQLite makes one for
  the referring column: `REFERENCES sample` on `identifier.sample_uuid` gives
  no way to find the identifiers of a sample but to read them all.
- `UNIQUE (a, b)` finds rows by `a`, or by `a` and `b`. Not by `b`.
  `ref_object` had `UNIQUE (ref_type, key)`, and looking a reference up by
  its position read every object of the type.
- So: every column a query filters or joins on, when that query runs per
  analysis or over a listing, needs an index that starts with it. When you
  add a column that will be looked up by, add its index in the same
  migration.
- A column that is null for most rows gets a partial index
  (`... (position_uuid) WHERE position_uuid IS NOT NULL`): smaller, and
  both engines use it for an equality, which implies not null.
- An index on an expression (`lower(name)`) serves only a query that writes
  the same expression.
- An index is not free: each one is written on every insert. Add the ones a
  query needs and can be shown to use, not one per column.
- A new index gets a test that reads the plan: `SchemaIndexes` in
  `tests/persistence/test_schema.cpp` runs `EXPLAIN QUERY PLAN` on the real
  statement and requires the index by name and no `SCAN`. That test fails
  when a later rewrite of the query stops using it.

### Queries

- Fetch a row by its uuid. A name is not a key: a monitor's sample exists
  once per project (97 times in that store), and matching by name and
  project name is a guess. If the query that found the analysis already
  joined the row you want, carry its uuid out (`BrowseRow::sample_uuid`)
  instead of looking it up again by what it is called.
- Do not use a search to do a lookup. `samples({.text = name})` is
  `LIKE '%name%'`: no index can serve a pattern that starts with `%`, it
  returns every name containing the text, and it stops at its `LIMIT`, so
  the row you wanted may not be among them.
- Never count in a subquery per row
  (`(SELECT count(*) FROM ... WHERE x = outer.uuid) AS n`). It runs once for
  every row listed. Aggregate once for all rows in a `WITH` clause and
  `LEFT JOIN` it, `coalesce(n, 0)`: `kSampleCounts` in `sql/catalog.hpp` is
  the model. Listing 500 samples went from 1.3 s to 0.02 s. A count per
  row that an index answers (one seek each) is bearable for a short
  listing; `kIrradiations` and `kSheetPositions` in that file are still
  written that way and have not been measured on a large store. Measure
  before copying them.
- No `OR` between columns of different tables, least of all across an outer
  join (`WHERE ip.sample_uuid = ? OR i.sample_uuid = ?`): no index serves it
  and the tables are read whole. Write the two cases as a `UNION`. An `OR`
  between columns of one table is fine when each side has its index (the
  plan says `MULTI-INDEX OR`).
- What costs and is not always wanted is asked for, not given: a listing's
  counts are behind `SampleQuery::counts`, and the loader, which wants the
  row, turns them off.
- `ORDER BY ... LIMIT` over a large table needs an index in that order, or
  everything is sorted to return the first page (`USE TEMP B-TREE FOR ORDER BY`
  in the plan). Fine for a few hundred rows that a filter already chose; not
  for `analysis` or `revision`.
- A new read that happens for every analysis is multiplied by every
  analysis. Before adding one to `StoreSource::load`, ask whether it is the
  same for every analysis of a level or an irradiation (a production, a
  chronology) and should be read once and shared, and whether it can be one
  statement for many analyses (`WHERE analysis_uuid IN (...)`) rather than
  one each. Loading is still one analysis at a time, several small
  statements each (the row, its heads, a payload per head, its references,
  a payload per reference); that is the next thing to change, not a pattern
  to copy.
- Both engines run every statement. Standard SQL, and where they differ,
  the `Dialect` switch (`sql::ts`, `geom_read`), not two copies of a query.
  Times are read as text through `sql::ts`. TinyORM's `return_qdatetime`
  stays off for SQLite (`tiny/db.cpp`): on, every text value read is tried
  as a date, which was most of the time of a load and reworded any text
  that looked like one.

### How to know

Reading a query does not tell you its cost; the plan and a clock do.

- The plan, on a store of real size (ask for a copy of a lab's; work on the
  copy):

      sqlite3 store.db
      .timer on
      EXPLAIN QUERY PLAN <the statement, with real values>;

  Bad signs: `SCAN <a large table>`; `CORRELATED SCALAR SUBQUERY` under a
  listing; `USE TEMP B-TREE` over many rows; a `SEARCH` by an index's first
  column only when you filter on more. On PostgreSQL:
  `EXPLAIN (ANALYZE, BUFFERS)`, and look for `Seq Scan` on a large table.
- The clock: `tests/processing/test_store_load_timing.cpp` runs the pipeline
  a figure window runs and prints the time of each node. It does nothing
  unless told which store:

      PYCHRON_BENCH_DB=sqlite:/path/to/store.db PYCHRON_BENCH_N=400 \
        build/dev/tests/processing/pychron_processing_store_tests --gtest_filter='StoreLoadTiming.*'

  Run it before and after any change to loading, to a query the loader
  uses, or to the schema, at 24, 400 and 2000 analyses, and put the numbers
  in the commit message. Time that grows faster than the count is a query
  that reads a table per analysis.
- When it is slow and the plan looks right, sample the process (`sample <pid> 5`
  on macOS, `perf` on Linux) before changing anything. The costliest of the
  four causes was not in a query: it was the library parsing dates.
- What goes in CI is the plan test, not the clock: a time limit fails on a
  loaded runner and passes on a fast one. A slow query has a shape (a scan,
  a subquery per row); assert the shape.
- The persistence tests run on SQLite. Set `PYCHRON_TEST_PG_URL` and run
  them on PostgreSQL as well before a release that changes a query or adds
  a migration: the two planners do not make the same choices.

## Static analysis

After changing C++ and before running the tests, run
`python3 tools/quality_check.py` (cppcheck, then clang-tidy, on the lines that
differ from `origin/develop`; `--json` for a machine-readable result). It
needs a configured build directory; `docs/dev_setup.md`, "Static analysis",
has the setup.

- Exit status 1 means findings on lines you changed: fix the code. Status 2
  means the check did not run (a missing tool, no compile database, a source
  that does not parse): fix that, do not go on as if it had passed.
- A finding that is wrong for this code is silenced on its line with
  `// NOLINT(<check>): <why>` or `// cppcheck-suppress <id>`, never by taking
  the check out of `.clang-tidy` or adding to `cmake/cppcheck.supp` to get one
  change through.
- `--fix` applies clang-tidy's fix-its; read the diff it makes and rebuild.
- A push is refused while it reports anything: `tools/githooks/pre-push` runs
  it. The hook is switched on once per clone with
  `git config core.hooksPath tools/githooks` (every worktree of the clone then
  has it); check `git config --get core.hooksPath` in a fresh clone and set it
  if it is empty. Do not push with `--no-verify` to get past a finding.
- It is not in CI, and it does not replace building with
  `-DPYCHRON_SANITIZE=address,undefined` and running the tests.
- The tree is not at zero: about 670 findings of the performance and C-array
  checks are left in old code
  (`docs/superpowers/plans/2026-10-08-static-analysis-backlog.md`, Phase 3).
  Only lines you change are held to the checks; when you are changing a
  function anyway, clear what it has.

## Lifetime rules

Systems components take non-owning references (`Scheduler&`, `SignalBus&`,
`Clock&`, role pointers such as `IIntensityAcquirer*`). Whatever they point at
must outlive them. In tests, declare the fakes before the fixture that owns the
component: locals are destroyed in reverse order of declaration.

## Time

Time is injected (`pychron::Clock`, `libs/core` `clock.hpp`). On hardware it
is a `SteadyClock`. A simulated run uses a `VirtualClock`, which stands still
while any thread taking part in it has work to do and jumps to the next
deadline when all of them are waiting, so a simulated hour costs its CPU time
(`docs/superpowers/specs/2026-10-06-virtual-clock-design.md`). The clock only
knows about waits made through it: a thread blocked any other way looks busy
and time stops; a thread woken any other way is not heard. Hence:

- A thread on a simulated path (anything under the line, the spectrometer,
  the executor or a script) constructs a `Clock::Participant` first thing.
  Its starter makes a shared `Clock::Hold` before `std::thread`, the thread
  drops its copy once its `Participant` exists, and the starter drops its own
  after the thread is created: otherwise time can jump past a thread the
  clock has not heard of yet.
- It is joined after it has said it is done: as its last act, while still a
  participant, it sets a flag under the owner's mutex and notifies through
  the clock; the joiner waits for the flag with `clock.wait` and then calls
  `join()`. A bare `join()` of a thread that still has to wait in the clock
  stalls. Never wrap the join in `Clock::Detached`: that is for waits on the
  outside world (a real program, the operator), and around a wait for a
  participant it lets time run on without the waiter.
- It waits for time, or for another such thread, only through the clock
  (`wait`, `wait_until`, `sleep_for`). A condition variable waited on that
  way is notified through the clock (`clock.notify_all(cv)`), after the state
  its waiters test was changed under the mutex they hold. No `cv.wait_for`,
  `std::this_thread::sleep_for`, `future.get()` or polling against
  `steady_clock`.
- A mutex held across clock time (a transport or device call, a settle, any
  `clock.wait*`) is a `ClockMutex` or `RecursiveClockMutex`
  (`clock_mutex.hpp`): a second participant blocked on a `std::mutex` looks
  busy, and the holder's wait never ends. A mutex that guards a few fields,
  or belongs to a condition variable, stays `std::mutex` and is never held
  across such a call.
- A scripting host call that waits or talks to a device releases the
  interpreter lock first (`host_state.cpp`): the lock is a plain mutex to the
  clock, and held across simulated time it stalls the next script thread.
- One clock per `CancelToken` and per `LaserSystem`: a token is waited on and
  woken through the clock of its waiters, and a laser system's camera is on
  the clock the system was given (one on another is refused).
- Timestamps that are written down come from `clock.wall_now()`, not
  `system_clock::now()`, so a simulated session is stamped in simulated time.
  What the UI shows against such a stamp ("since", "ago", an alarm's time)
  reads the same clock (`CoreBridge::wall_now()` and `now()`), never
  `QDateTime::currentDateTime()` or `steady_clock::now()`.
- On `SteadyClock` all of this is a pass-through: `Participant`, `Hold` and
  `Detached` do nothing and the clock mutexes behave as plain mutexes (each
  is still a flag and a condition variable, not a `std::mutex`). What is left
  on real time on purpose (the log hub, the camera's live timeouts, a
  script's `max_wall_time`, the notifier, the UI's own threads) is listed in
  the spec, sections 4.4 and 4.5; add to that list rather than to the code.

Tests (`tests/support/virtual_time.hpp`, namespace `pychron::testing`):

- A test in simulated time derives from `VirtualTimeTest`, declares the
  `VirtualClock` first, makes its own thread a `Clock::Participant` and moves
  time with `clock.sleep_for(d)`: time does not move while the test's thread
  is running, so what it then asserts about `clock.now()` is exact. With no
  participant at all a timed wait on a `VirtualClock` returns at once, at its
  deadline: a test whose thread is not one has nothing holding time back.
- Its other threads are started with `Crew`, which follows the start and
  join rules above. `await_waiters(clock, n)` waits (in real time, with a
  limit) until `n` threads are asleep in the clock.
- In a simulated-time test, no real sleeps for correctness, and a real-time
  bound is an upper bound of 5 s or more, there only to tell "took no real
  time" from a stall. The `*Steady` tests (`ExampleLineSteady`,
  `NgxLinkSteady`, `QtegraAcquireSteady`, `AdcBankSteady`) are the exception
  by design: they keep the hardware arrangement under test on a
  `SteadyClock`, with short real sleeps and real lower bounds. So are the
  pacing tests of `VirtualClock` (what a paced jump costs in real time is the
  thing tested, and its real lower bound is the point) and `elctl`'s
  interrupt test (`AnInterruptStopsAPacedQueueInRealTime`); their upper
  bounds follow the rule all the same.
- A stuck test does not hang: the fixture's dead-man always aborts it after
  30 s of real time, with a message. When some thread is waiting in the clock
  for a deadline, the clock also reports `virtual clock stalled; runnable:
  <name>` on stderr after 10 s, naming the participants that are not waiting
  in it (start looking there: a raw wait, a plain mutex, a bare join). With
  no timed waiter (everybody waiting untimed, say for a notify that never
  comes) there is no such report, only the abort.
- `ManualClock` remains for single-threaded unit tests that step time by
  hand. Nothing advances it for a waiting thread: `advance()` wakes no
  untimed waiter, and a thread waiting on one that nobody advances waits for
  good.

Apps: `--sim` alone keeps real time. `elctl exp run --sim-speed N|max` and
`pychron-ui --sim --sim-speed N` run on a `VirtualClock` at N simulated
seconds a second (`max`, `elctl` only: no waiting at all).
