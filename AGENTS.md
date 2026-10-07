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
- To land: rebase the branch on `origin/develop`, run the tests, merge into
  `develop` and push. No pull request is needed for `develop`.
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
- `-DPYCHRON_SANITIZE=address,undefined` enables ASan/UBSan.
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
- Ubuntu 24.04's cmake 3.28 is too old for this tree (`pip install cmake`).

Compilers disagree about undefined behaviour: a test that passes under clang
can abort under gcc, and the reverse. A failure on one compiler only is a real
bug until shown otherwise.

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
  is running, so what it then asserts about `clock.now()` is exact.
- Its other threads are started with `Crew`, which follows the start and
  join rules above. `await_waiters(clock, n)` waits (in real time, with a
  limit) until `n` threads are asleep in the clock.
- In a simulated-time test, no real sleeps for correctness, and a real-time
  bound is an upper bound of 5 s or more, there only to tell "took no real
  time" from a stall. The `*Steady` tests (`ExampleLineSteady`,
  `NgxLinkSteady`, `QtegraAcquireSteady`, `AdcBankSteady`) are the exception
  by design: they keep the hardware arrangement under test on a
  `SteadyClock`, with short real sleeps and real lower bounds.
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
