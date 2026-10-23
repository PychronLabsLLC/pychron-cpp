# Agent guide

## Workflow

Single developer. Two long-lived branches:

- `develop`: integration branch. All work lands here first.
- `main`: released or about to be. No pushes: changes only by pull request.
  GitHub does not enforce yet (repository public, so branch protection free;
  not switched on), so rule kept by hand. Repository stays public: update
  check (`2026-10-21-update-check-design.md`) reads releases without token.

Day to day:

- Work on branch (or worktree) cut from `develop`, named for kind of change:
  `feat/<topic>`, `fix/<topic>`, `chore/<topic>`, `docs/<topic>`,
  `refactor/<topic>`, `test/<topic>`, `ci/<topic>`.
- Commit messages follow Conventional Commits, component as scope:
  `feat(ui): a heaters dock`, `fix(canvas): ...`, `docs: ...`. Release notes
  and next version computed from them: `feat` minor bump, `fix` and `perf`
  patch, `feat!:` / `fix!:` or `BREAKING CHANGE:` footer breaking change
  (still minor bump while version is 0.x). `chore`, `docs`, `refactor`,
  `test`, `ci`, `build` release nothing on their own.
- To land: rebase branch on `origin/develop`, run static analysis
  (`python3 tools/quality_check.py`, below) and tests on the rebased branch,
  merge into `develop`, push. No pull request needed for `develop`. If
  `origin/develop` moves again before the push, that is another rebase and
  another run: what is pushed is what was tested.
- CI does not run on `develop` or work branches, only on `main` and pull
  requests into it. Local tests are the only ones before a release: run them.
  While a pull request from `develop` into `main` is open, every push to
  `develop` runs CI on it: read that run (`gh pr checks <n>`). A red run is
  fixed before other work lands on top of it.
- Branch landed: remove its worktree (`git worktree remove <path>`), then the
  branch. Each worktree holds its own build tree, 2 to 16 GB (October 2026:
  33 landed worktrees, 181 GB). Never one this session did not make.
  Exception: the nightly run names its worktrees
  `.claude/worktrees/nightly-<topic>` and never lands them itself, so a
  later nightly run removes one, and its branch, when every commit of the
  branch is on `origin/develop` and the worktree has no uncommitted change;
  never with `--force` or `-D`. A nightly run deletes the build tree of its
  worktree whose row is closed or whose plan is done; one whose row goes on
  keeps it for the next night.
- Never skip or disable failing test; find root cause.
- Never push to `main`. Do not open a pull request into `main` or merge one
  unless asked to release.

Releasing (`.github/workflows/release-please.yml`):

1. Open pull request from `develop` into `main`. CI runs on it.
2. Merge with merge commit, never squash: release-please reads individual
   commits.
3. release-please opens (or updates) release pull request into `main` with
   next version, `CHANGELOG.md`, and version in `version.txt`,
   `CMakeLists.txt`, `vcpkg.json`. Do not edit those by hand.
4. Merging release pull request tags `vX.Y.Z` and publishes GitHub release;
   `release` workflow builds installers and attaches them.
5. Same workflow merges `main` back into `develop`. If that job fails on a
   conflict, merge `main` into `develop` by hand.

Hotfix: `fix/` branch cut from `main`, merged into it by pull request;
reaches `develop` through step 5.

## Spec-driven development

Anything larger than small fix starts from written spec, not code:

1. Spec: `docs/superpowers/specs/YYYY-MM-DD-<topic>-design.md`. What is built
   and why, interfaces, rules that must hold, what is out of scope. Ends with
   `## Open questions`: one item per question only developer can answer, or
   `None.`. Settle what code, old implementation and this file settle; list
   only rest. Spec that lists questions is agreed with developer before
   anything else is written. Spec whose section says `None.` needs no
   agreement: plan follows.
2. Plan: `docs/superpowers/plans/YYYY-MM-DD-<topic>.md`. Spec cut into tasks
   small enough to test and commit one at a time, each naming its files and
   tests. Same `## Open questions` rule: none listed, build starts without
   review.
3. Implement plan task by task, test first. When code has to depart from
   spec, spec is changed in same branch: the two never disagree on `develop`.

caveman plugin keeps conversation and subagents cheap while this runs:

- Conversation with developer (questions, status, findings): caveman mode, at
  level session is set to. Warnings, confirmations of anything irreversible,
  and ordered steps that fragments would make ambiguous are written in full.
- Specs written compressed, caveman full: no articles, no filler, fragments
  allowed. Nothing technical dropped: every interface, rule, number, path and
  out-of-scope item stays; identifiers, code blocks, SQL, commands and quoted
  errors exact and uncompressed. A rule whose meaning depends on order or on
  a conjunction ("before", "unless", "only if") is written as a full
  sentence. Write new spec compressed from start; `caveman:compress` is for
  an existing one, and the `<file>.original.md` backup it leaves is not
  committed.
- Plans, code, comments, user docs, commit messages, pull request
  descriptions: normal prose.
- This file kept compressed too, same rules as a spec. Edits to it written
  compressed.
- Delegate with cavecrew agents (reports come back compressed):
  `cavecrew-investigator` to find where something lives while writing spec or
  plan, `cavecrew-builder` for plan task touching one or two files,
  `cavecrew-reviewer` to review task's diff against spec before it is merged.
  Task wider than two files: main thread or general agent.
- `caveman:caveman-commit` may draft commit message, but result must still
  meet rules under Workflow: Conventional Commits with component as scope,
  since release notes are computed from it.

## Delegated reviews

A review handed to a subagent (of a task's diff, of a fix round, of a whole
branch) runs on Sonnet: pass `model: sonnet` when dispatching it. The work
under review is written on whatever model the session uses.

## Build and test

Setup: `docs/dev_setup.md`. Presets: `CMakePresets.json` (CI uses `dev`, and
`dev-ui` for the UI job). Tests are GoogleTest, one `test_<component>.cpp` per
component under `tests/`, run with `ctest`.

Inner loop (keep waits short):

- Build only target being changed:
  `cmake --build --preset dev --target pychron_<component>_tests`.
- Run that binary direct, filtered, not `ctest`: one process, not one per
  case. `build/dev/tests/<component>/pychron_<component>_tests
  --gtest_filter='<Suite>.*'`.
- Wider check while working: `ctest --preset dev-quick` (parallel, skips
  importer suites `Engines/ProjectImportTest`, `Engines/MetaImportTest`,
  `ImportCmd*`: about half of serial test time).
- Before merge into `develop`: full `cmake --build --preset dev` and
  `ctest --preset dev`. Never `dev-quick` as final run.
- `dev` and `dev-quick` test presets run on every core (`CTEST_PARALLEL_LEVEL`
  empty); `dev-ui` runs 2 at a time, as CI. A test that fails only in parallel
  is a bug (shared file, port, settings): fix it, do not serialize suite.
- Use `dev-ui` only when change touches `apps/pychron-ui` or `tests/ui`. It
  builds and runs everything `dev` does and the UI tests: one run of it
  covers both.
- ccache used when installed (`cmake/PychronCcache.cmake`); base_dir is
  source tree, so new worktree reuses objects other checkouts built. First
  build in new worktree still configures and fetches dependencies.
- `gtest_discover_tests` lists cases when ctest runs (`PRE_TEST`), not after
  each link.

- CI (`.github/workflows/ci.yml`) builds with clang + ASan/UBSan on macOS,
  gcc 14 and clang 18 + ASan/UBSan on Ubuntu 24.04, and MSVC.
- Apple clang does not give those compilers' warnings, and each CI job stops
  at its first error. After changing C++, with the static analysis:
  `python3 tools/compiler_sweep.py` (every source through Homebrew's `g++`,
  `-fsyntax-only`, all warnings at once; needs `brew install gcc` and a
  configured build directory; the pre-push hook runs it on `build/dev`, so a
  change under `apps/pychron-ui` or `tests/ui` is swept by hand with
  `--build-dir build/dev-ui`). While working, `--changed`: only sources the
  branch's changes reach (own file, or header read when last built, from
  Ninja's dependency log; unknown → swept; changed CMake file → whole tree).
  One edited source: seconds, not minutes. Whole tree once before push (the
  hook). Nearest thing to CI a Mac has, not CI: gcc 16
  does not give every gcc 14 warning. Met in CI and not locally, October
  2026:
  - gcc: `EXPECT_*` under an unbraced `if` (`-Wdangling-else`); a local or a
    lambda parameter named as an outer one (`-Wshadow`); `const auto [a, b]`
    over pairs (`-Wrange-loop-construct`, gcc 14 only: write `const auto&`);
    a reference returned from a call given a temporary
    (`-Wdangling-reference`: take the argument by value); a `//` line ending
    in a backslash; a dropped `json::parse` result even under `(void)`.
  - clang 18: a designated initializer that leaves fields out
    (`-Wmissing-field-initializers`).
  - MSVC: `sscanf` and its kind (C4996); `const auto*` from a container's
    iterator, which is no pointer there; code after `FAIL()` or a `return`
    (C4702: `FAIL()` returns, so use `ADD_FAILURE()` before an abort); a raw
    string literal as argument of a gtest macro (C2017).
  - Qt on Ubuntu 24.04 is 6.4: an API newer than that is behind
    `QT_VERSION_CHECK`.
- Reading CI: `gh pr checks <n>`. Log of a failed job while other jobs still
  run: `gh api --allow-escape-sequences
  repos/<owner>/<repo>/actions/jobs/<job id>/logs` (`gh run view
  --log-failed` gives nothing until the whole run ends). What a failed test
  printed is in the log's tail (`tools/ci_failed_output.py`), before the
  rerun.
- Bytes that are a file format do not depend on the compiler:
  `TOML_FLOAT_CHARCONV=0` (`libs/core/CMakeLists.txt`) makes toml++ write a
  float the same under MSVC as under gcc and clang, so a record has one sha.
  Never removed; a new writer of a stored format is checked on Windows CI
  against a fixture written elsewhere.
- `-DPYCHRON_SANITIZE=address,undefined` enables ASan/UBSan, and with them
  standard library's own checks (empty optional dereferenced, index past the
  end): static analysis does not look for those here, this does.
- gcc 13 (Ubuntu 24.04 default `g++`) warns where CI compilers do not; build
  it with `-DPYCHRON_WARNINGS_AS_ERRORS=OFF`.
- `-DPYCHRON_SCRIPTING=OFF` drops embedded CPython dependency.
- `-DPYCHRON_VISION_OPENCV=AUTO|ON|OFF` (default `AUTO`) controls optional
  OpenCV in `libs/vision` (`LegacyFinder`, `OpenCvSource`); without it those
  two files compile to stubs. OpenCV headers appear only in those two `.cpp`
  files. Only macOS `ui` CI job installs OpenCV (Homebrew's brings TBB, which
  crashes at exit under sanitizers).
- `-DPYCHRON_PUBLISH=AUTO|ON|OFF` (default `AUTO`) controls optional Apache
  Arrow (with Parquet) in `libs/publish`: the GeoParquet tables of a release.
  Without it `make_parquet_writer()` is null and a build writes everything
  but `data/*.parquet`. Arrow headers appear only in
  `libs/publish/src/parquet_writer.cpp` (and the test that reads back). Only
  macOS `ui` CI job installs it (`brew install apache-arrow`); the others run
  the stub and the row-hash fixture test (`tests/publish/fixtures`).
- `libs/persistence` (DVC store, TinyORM on QtSql) builds only when Qt6 Core
  and Sql are found; `-DPYCHRON_PERSISTENCE=OFF` skips it. On Ubuntu:
  `apt install qt6-base-dev libqt6sql6-sqlite libqt6sql6-psql`. Qt must not
  appear in its public headers or in any other `libs/` library but
  `libs/earthbank_qt`, HTTPS adapter of `libs/earthbank`.
- Cache of ages (`derived_value`; spec
  `docs/superpowers/specs/2026-10-09-age-cache-filter-design.md`): browser's
  Age column and age filter read it, nothing calculates with it. Three rules:
  - Cache write uses fingerprint analysis was loaded with
    (`Analysis::input_fingerprint`, set by `StoreSource::load`), never one
    computed at write time.
  - At most one fingerprint per analysis after any write (`put_derived_many`
    deletes other fingerprints' rows first).
  - Count and facet statements join `derived_value` only when an age bound is
    set; page statement always joins, by primary key (`dv.name || '' = 'age'`
    without a bound).
- `libs/ingest`, `libs/dvc`, `libs/experiment` do not reduce and gain no
  dependency on `reduction` or `processing`; import's derive pass is in
  `apps/elctl`, after walk ends.
- A file the application writes for the user to open elsewhere (report,
  figure, template, sheet) goes through `pychron::mark_as_user_file`
  (`libs/core` `user_file.hpp`) after it is written: downloaded, unsigned
  macOS application quarantines what it writes, and Gatekeeper then refuses
  the file. Files pychron reads back itself (configs, stores) do not.
- Ubuntu 24.04's cmake 3.28 too old for this tree (`pip install cmake`).

Compilers disagree about undefined behaviour: test that passes under clang can
abort under gcc, and reverse. Failure on one compiler only is real bug until
shown otherwise.

## Component rules

Rules of one component live beside it, not here. Read the file before
changing anything under the paths it covers (a session in Claude Code is
given it when it reads a file there; any other agent opens it itself).

| Before changing | Read |
|---|---|
| `libs/persistence`, `tests/persistence`, a migration, any query against the store | `libs/persistence/AGENTS.md` (has "Schema and queries": a migration costs every lab a step, a query is paid once per analysis) |
| `libs/ingest`, `libs/dvc`, `tests/dvc`, `elctl import` | `libs/ingest/AGENTS.md` |
| `libs/processing`, `tests/processing`: publication report, flux fitting, `elctl export`, `elctl flux`, flux window | `libs/processing/AGENTS.md` |
| `libs/entry`, `tests/entry`, an install's `seed.toml` | `libs/entry/AGENTS.md` |
| `libs/earthbank`, `libs/earthbank_qt`, `tests/earthbank`, `elctl earthbank`, EarthBank page and Publish dialog | `libs/earthbank/AGENTS.md` |
| `libs/experiment` record, store adapter, `tests/experiment`, how a run is saved, executor hold, safe state, gap rule | `libs/experiment/AGENTS.md` |
| `profiles/`, `configs/examples`, shipped plans, queue, field tables, `tests/setup`, `tests/integration/test_install_e2e.cpp` | `libs/setup/AGENTS.md` |
| `libs/devices`, `tests/devices`, a driver's command or wire call | `libs/devices/AGENTS.md` (which command may be sent twice; a new command has a row in `test_repeat_tables.cpp`) |
| `libs/sim`, `sim.toml`, simulator noise or gas | `libs/sim/AGENTS.md` |
| `libs/metrics`, an exporter, a dashboard | `libs/metrics/AGENTS.md` |
| `apps/pychron-ui` Preferences, the line's local override file | `apps/pychron-ui/AGENTS.md` |
| `libs/processing` `domain.hpp`, data windows of `apps/pychron-ui` (browser, figure, recall), `elctl` export and flux | No file: new figure, exporter, derived quantity or recall section is a `Domain` entry, never a literal in a window. `libs/processing` names no isotope outside `domain_arar.cpp` until it moves. |
| `packaging/`, signing, entitlements | `packaging/AGENTS.md` |

A rule that is one component's goes in that file; one that every change must
keep stays here. A new component with rules easy to break gets its own file,
a `CLAUDE.md` beside it holding `@AGENTS.md`, and a row above.

## Static analysis

After changing C++ and before running the tests, run
`python3 tools/quality_check.py` (cppcheck, then clang-tidy, on the lines that
differ from `origin/develop`; `--json` for machine-readable result). Needs a
configured build directory; setup in `docs/dev_setup.md`, "Static analysis".

- Exit status 1: findings on lines you changed: fix the code. Status 2: check
  did not run (missing tool, no compile database, source that does not
  parse): fix that, do not go on as if it had passed.
- A finding that is wrong for this code is silenced on its line with
  `// NOLINT(<check>): <why>` or `// cppcheck-suppress <id>`, never by taking
  the check out of `.clang-tidy` or adding to `cmake/cppcheck.supp` to get one
  change through.
- `--fix` applies clang-tidy's fix-its; read the diff it makes and rebuild.
- A push is refused while it reports anything: `tools/githooks/pre-push` runs
  it, then `tools/compiler_sweep.py` on `build/dev` when the branch changes
  C++ and the machine has Homebrew's `g++-NN` and that build directory
  (about a minute; left out without either). Hook is switched on once per
  clone with
  `git config core.hooksPath tools/githooks` (every worktree of the clone then
  has it); check `git config --get core.hooksPath` in a fresh clone and set it
  if empty. Do not push with `--no-verify` to get past a finding or a
  warning.
- Not in CI, and does not replace building with
  `-DPYCHRON_SANITIZE=address,undefined` and running the tests.
- Tree is not at zero: about 670 findings of the performance and C-array
  checks are left in old code
  (`docs/superpowers/plans/2026-10-08-static-analysis-backlog.md`, Phase 3).
  Only lines you change are held to the checks; when changing a function
  anyway, clear what it has.

## C++ conventions

C++20 (`CMAKE_CXX_STANDARD` in top-level `CMakeLists.txt`); do not set a
standard per target. Follow C++ Core Guidelines where this file is silent.

- Errors are values: a function that can fail returns `pychron::Result<T>`
  (`libs/core` `error.hpp`). `libs/` throws only for a mistake in the calling
  code (`Scheduler::stop()` from a job on its own dispatcher) and, in
  `libs/scripting`, to hand a Python error back to pybind11.
- Ownership is RAII. Prefer a value or a stack object; `std::unique_ptr` when
  heap is needed; `std::shared_ptr` only when ownership really is shared. A
  `new` goes straight into a smart pointer (a factory with a private
  constructor cannot use `std::make_unique`), with two exceptions: Qt code,
  where a parent owns the child, and an object meant to live for the whole
  process. No `delete` outside Qt code.
- Rule of zero: a type that manages nothing declares no destructor, copy or
  move. A type that does manage a resource declares all five, or deletes the
  ones it does not support.
- `const` by default, `constexpr` where value is known at compile time,
  `[[nodiscard]]` on a result that must not be dropped. A mistake the
  compiler can refuse (strong type, `concept`, `static_assert`) is better
  than one a test has to find.
- Standard algorithm or range instead of hand-written loop when it says the
  same thing more plainly; not when it does not.
- Headers use `#pragma once`.
- Code that adds a thread, or shares state with one, is run under
  `-DPYCHRON_SANITIZE=thread` before it lands: CI runs ASan and UBSan only.
  TSan and ASan cannot be combined in one build. Qt here not built with
  TSan: hand-off inside QtCore invisible to it, so data captured in a queued
  functor (`invokeMethod` lambda) and read on other thread is reported as
  race, some runs only. Hand data over under own mutex, queued call carries
  nothing (`SpectrometerBridge` Gate). No TSan suppressions file: a frame
  match would hide real races in everything run on that thread.
- A change made for speed comes with a measurement, before and after.

## Lifetime rules

Systems components take non-owning references (`Scheduler&`, `SignalBus&`,
`Clock&`, role pointers such as `IIntensityAcquirer*`). Whatever they point at
must outlive them. In tests, declare fakes before the fixture that owns the
component: locals are destroyed in reverse order of declaration.

A component whose work runs on another thread (a scheduler job, a bus
handler) is not gone when its state says idle: its destructor waits until
that thread has left it. `JobRunner` publishes `JobFinished` after it is idle
again, so it counts the threads inside `execute()` and waits for them; before
it did, ASan on Linux read a destroyed runner.

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
  on real time on purpose (log hub, camera's live timeouts, a script's
  `max_wall_time`, the notifier, the UI's own threads) is listed in the spec,
  sections 4.4 and 4.5; add to that list rather than to the code.

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
- A real-time upper bound has room. Under ASan on CI's four-core Linux
  runner a test costs up to 1.5 times what it costs alone when every core is
  busy, and macOS runners stretch a 40 ms timed wait to 50 to 75 ms. So: the
  bound is at least three times the slowest time CI has shown for the test;
  the fixture's dead-man is above the bound; ctest's `TIMEOUT` for the
  target is above the dead-man. Three numbers, moved together. No test
  asserts a rate, or an order of events, that rests on a real sleep of under
  100 ms: put the thing on an injected clock (`LaserBridgeDeps::video_clock`
  is the model).
- A stuck test does not hang: the fixture's dead-man always aborts it after
  30 s of real time (or what its fixture gives it), with a message. When some thread is waiting in the clock
  for a deadline, the clock also reports `virtual clock stalled; runnable:
  <name>` on stderr after 10 s, naming the participants that are not waiting
  in it (start looking there: a raw wait, a plain mutex, a bare join). With
  no timed waiter (everybody waiting untimed, say for a notify that never
  comes) there is no such report, only the abort.
- `ManualClock` is for single-threaded unit tests that step time by
  hand. Nothing advances it for a waiting thread: `advance()` wakes no
  untimed waiter, and a thread waiting on one that nobody advances waits for
  good.

Apps: `--sim` alone keeps real time. `elctl exp run --sim-speed N|max` and
`pychron-ui --sim --sim-speed N` run on a `VirtualClock` at N simulated
seconds a second (`max`, `elctl` only: no waiting at all).
