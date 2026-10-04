# Agent guide

## Workflow

- Single developer. Do not open pull requests. Commit on a branch (or worktree),
  then merge into `main` and push.
- Before pushing, bring `main` up to date with `origin/main` (rebase local work
  on top of it) and run the tests.
- Never skip or disable a failing test; find the root cause.

## Build and test

See `docs/dev_setup.md` for setup and `CMakePresets.json` for presets (CI uses
`dev`). Tests are GoogleTest, one `test_<component>.cpp` per component under
`tests/`, run with `ctest`.

- CI (`.github/workflows/ci.yml`) builds with clang + ASan/UBSan on macOS,
  gcc 14 and clang 18 + ASan/UBSan on Ubuntu 24.04, and MSVC.
- `-DPYCHRON_SANITIZE=address,undefined` enables ASan/UBSan.
- gcc 13 (the Ubuntu 24.04 default `g++`) warns where the CI compilers do not;
  build it with `-DPYCHRON_WARNINGS_AS_ERRORS=OFF`.
- `-DPYCHRON_SCRIPTING=OFF` drops the embedded CPython dependency.
- `-DPYCHRON_VISION_OPENCV=AUTO|ON|OFF` (default `AUTO`) controls the optional
  OpenCV in `libs/vision` (`LegacyFinder`, `OpenCvSource`); without it those
  two files compile to stubs. OpenCV headers appear only in those two `.cpp`
  files. Only the macOS CI job installs OpenCV.
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
  Never edit an applied migration; add `NNNN_<name>.sql`.
- `libs/entry` (sample and package entry) builds only with persistence, like
  `libs/ingest`. Entry writes catalog rows only through
  `IStore::apply_catalog_edits` (field-value compare-and-swap, one
  transaction) and identifiers only through `allocate_identifiers`; never
  through ad hoc UPDATEs. User guide: `docs/entry.md`.
- Ubuntu 24.04's cmake 3.28 is too old for this tree (`pip install cmake`).

Compilers disagree about undefined behaviour: a test that passes under clang
can abort under gcc, and the reverse. A failure on one compiler only is a real
bug until shown otherwise.

## Lifetime rules

Systems components take non-owning references (`Scheduler&`, `SignalBus&`,
`Clock&`, role pointers such as `IIntensityAcquirer*`). Whatever they point at
must outlive them. In tests, declare the fakes before the fixture that owns the
component: locals are destroyed in reverse order of declaration.
