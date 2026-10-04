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
- The schema source is `libs/persistence/migrations/pg/`. After editing it,
  run `python3 tools/ddl_sqlite.py` and commit the regenerated SQLite file.
  Never edit an applied migration; add `NNNN_<name>.sql`.
- Ubuntu 24.04's cmake 3.28 is too old for this tree (`pip install cmake`).

Compilers disagree about undefined behaviour: a test that passes under clang
can abort under gcc, and the reverse. A failure on one compiler only is a real
bug until shown otherwise.

## Lifetime rules

Systems components take non-owning references (`Scheduler&`, `SignalBus&`,
`Clock&`, role pointers such as `IIntensityAcquirer*`). Whatever they point at
must outlive them. In tests, declare the fakes before the fixture that owns the
component: locals are destroyed in reverse order of declaration.
