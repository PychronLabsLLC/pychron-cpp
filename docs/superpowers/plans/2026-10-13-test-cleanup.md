# Test Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The library test suites take less time and fewer lines to read, with the same protection: coverage of `libs/` never drops, and nothing a rule of the spec keeps is removed.

**Architecture:** Measure first (a per-test timing report from ctest, a local clang coverage build), then make the slow importer tests cheaper where the time actually is, then fold one-literal copies into tables, trim `elctl` import tests to command paths, and only last remove tests, one suite per commit with the test that still covers each one named.

**Tech Stack:** C++20, GoogleTest, CMake presets, ctest JUnit output, clang source-based coverage (`llvm-profdata`, `llvm-cov`), Python 3 tools.

**Spec:** `docs/superpowers/specs/2026-10-13-test-cleanup-design.md`, agreed 2026-10-13 (its section 7). Read it first; section numbers below are its. Section 4 gained, on this branch, the measurement below: it changes what Task 3 does first.

## What the code says

Read from `develop` `0f17653d`.

- Importer suites: `Engines/ProjectImportTest` (`tests/dvc/test_project_import.cpp:319`, 82 `TEST_P`) and `Engines/MetaImportTest` (`tests/dvc/test_meta_import.cpp:299`, 26) are both in `pychron_dvc_tests` (`tests/dvc/CMakeLists.txt:10`, ctest `TIMEOUT 600` at `:24`). Each test makes its own `World` (defined in each file: `test_project_import.cpp:63`, `test_meta_import.cpp:53`): a file-backed `TestDatabase` (`tests/persistence/store_fixture.hpp:39`–`97`: a temp SQLite file, or a fresh PostgreSQL schema when `PYCHRON_TEST_PG_URL` is set), `open_or_die` (migrations run on every open, `:99`–`106`), and for the project suite three catalog rows. `GitFixture::init()` (`tests/dvc/git_fixture.hpp:112`) is one `git init`, no commit. No `SetUpTestSuite` anywhere.
- `ImportCmd` (`apps/elctl/tests/test_import_cmd.cpp:82`, 37 `TEST_F`) and `ImportCmdParity` (`:1002`, 13) are in `pychron_elctl_tests` (`TIMEOUT 300`, `apps/elctl/tests/CMakeLists.txt:35`); SQLite only; `ImportCmdParity::build` (`:1018`) builds two repositories and imports them through `elctl` in every test.
- Measured 2026-10-13 (Debug, SQLite, one process): `ProjectImportTest` 125 s, median 0.49 s, fastest 0.17 s; `MetaImportTest` 34 s, median 0.28 s, fastest 0.13 s. `SetUp` is at most the fastest test: about 10% of a suite. Three bodies are 52 s of the first and 27 s of the second (spec section 4, measured).
- `OneHistoryOneResult` (`test_project_import.cpp:2564`, and the meta suite's) is required by `libs/ingest/AGENTS.md`: it holds one history with everything that has gone wrong at a batch boundary. It is made cheaper, never split or cut.
- `CMakePresets.json`: configure presets `dev` (`:87`, Debug) and `dev-ui` (`:96`), binary dir `build/<preset>`; test presets `dev` (`:165`), `dev-quick` (`:176`, excludes the importer suites), `dev-ui` (`:186`). No coverage option, preset or module (`cmake/` has `PychronSanitizers.cmake`, `PychronWarnings.cmake`, …). `xcrun --find llvm-cov` and `llvm-profdata` resolve on this Mac (Command Line Tools).
- No timing hook in the tests; environment variables used by tests: `PYCHRON_TEST_PG_URL`, `PYCHRON_TEST_SECRET_STORE`, `PYCHRON_TEST_CAMERA`, `PYCHRON_TEST_DATA_DIR`; benches use `PYCHRON_BENCH_*`.
- S2 candidates named by the spec: `tests/devices/spectrometer/test_thermo_qtegra_acquire.cpp:88`–`146`, `tests/devices/spectrometer/legacy/test_acquirers.cpp:176`–`207`. S4: `ImportCmdParity` against `InterpretedAgeBecomesARevision` (`tests/dvc/test_project_import.cpp:1857`).

## Global Constraints

- Root `AGENTS.md` stands: no test skipped or disabled; one failing only in parallel is fixed, not serialized; `*Steady` tests, `VirtualClock` pacing tests and `AnInterruptStopsAPacedQueueInRealTime` are kept.
- Spec section 2: a test that once caught a bug stays (commit history or comment); file-format tests and fixtures from another platform stay; PostgreSQL instances of `TEST_P` stay.
- `tests/ui` is out of scope.
- Every task that changes or removes a test: coverage report of `libs/` before and after (Task 2's tool), both summaries in the commit message, lines and branches not lower. Times before and after (Task 1's tool) in the commit message.
- A shared world is never written by a test; a test that writes gets its own copy.
- One suite per commit for removals and merges; the commit names each test dropped and the test that still covers it.
- Each task is one commit (Conventional Commits: `test(<component>)` for test changes, `build:` for presets, `chore(tools):` for tools). Before each: `python3 tools/quality_check.py` (exit 0), the changed test target built and run filtered. Before the branch lands: full `cmake --build --preset dev`, `ctest --preset dev`, `python3 tools/compiler_sweep.py`.
- `libs/ingest/AGENTS.md` rules hold for any change under `tests/dvc` or `apps/elctl/tests/test_import_cmd.cpp`: no test runs git against this repository; `GitFixture` keeps the user's git configuration off.

## File Structure

| File | Responsibility |
|---|---|
| `tools/test_times.py` (new) | Run ctest with JUnit output (or read one), print slowest tests and suites, compare two runs. |
| `cmake/PychronCoverage.cmake` (new) | `PYCHRON_COVERAGE` option: clang `-fprofile-instr-generate -fcoverage-mapping`; refused with gcc or MSVC. |
| `CMakePresets.json` | `coverage` configure and test preset (`build/coverage`, `LLVM_PROFILE_FILE` per process). |
| `tools/coverage_report.py` (new) | Merge profiles, `llvm-cov report` over `libs/` only, write a summary; compare two summaries. |
| `tests/dvc/test_project_import.cpp`, `tests/dvc/test_meta_import.cpp`, `tests/dvc/*support*.hpp` | Task 3's changes, decided from its profile. |
| `tests/devices/spectrometer/test_thermo_qtegra_acquire.cpp`, `tests/devices/spectrometer/legacy/test_acquirers.cpp` | S2 folds. |
| `apps/elctl/tests/test_import_cmd.cpp` | S4 trim. |
| `docs/dev_setup.md` | How to run the two tools. |

---

### Task 1: Per-test time report

**Files:** create `tools/test_times.py`; modify `docs/dev_setup.md` (a "Test times" paragraph).

- [ ] Step 1: Write `tools/test_times.py`:
  - `run --preset dev [--tests-regex R] --out times.xml`: runs `ctest --preset <p> -j1 --output-junit <out>` (one at a time, so a time is the test's own), exit status of ctest passed through.
  - `show times.xml [--top N] [--by suite|test]`: reads the JUnit (`testcase` `name`, `time`), groups by the part before the first `.` (gtest suite; `Engines/ProjectImportTest` kept whole), prints count, sum, median, min, max per suite and the top N tests.
  - `compare before.xml after.xml`: per suite, sum before and after and the difference; tests present in one only listed.
  - Standard library only; `--help` for each.
- [ ] Step 2: Test it by hand on `ctest --preset dev --tests-regex 'ProjectImportTest'` and check its numbers against gtest's own output (`pychron_dvc_tests --gtest_filter='Engines/ProjectImportTest.*'`): sums within 10%.
- [ ] Step 3: `docs/dev_setup.md`: when and how to run it; a full `-j1` run takes about 15 min.
- [ ] Step 4: Record a baseline of the whole tree in the commit message (top 15 suites by sum). Commit `chore(tools): a per-test time report from ctest`.

### Task 2: Local coverage build and report

**Files:** create `cmake/PychronCoverage.cmake`, `tools/coverage_report.py`; modify top-level `CMakeLists.txt` (include the module), `CMakePresets.json`, `docs/dev_setup.md`.

- [ ] Step 1: `cmake/PychronCoverage.cmake`: `option(PYCHRON_COVERAGE "clang source-based coverage" OFF)`; when ON and the compiler is not Clang or AppleClang, `FATAL_ERROR` naming the compiler; else add the two flags to compile and link options for every target (as `PychronSanitizers.cmake` adds its flags). Not combined with `PYCHRON_SANITIZE` (refused with a message).
- [ ] Step 2: `CMakePresets.json`: configure preset `coverage` (inherits `dev`, `PYCHRON_COVERAGE=ON`, `PYCHRON_WARNINGS_AS_ERRORS=ON`), test preset `coverage` (inherits `dev`'s environment, adds `LLVM_PROFILE_FILE=${sourceDir}/build/coverage/profiles/%p-%m.profraw`), excluding nothing.
- [ ] Step 3: `tools/coverage_report.py`:
  - `report --build-dir build/coverage --out summary.json`: `llvm-profdata merge -sparse` of `profiles/*.profraw`; `llvm-cov report` with every test binary under `build/coverage/tests` and `apps/elctl/tests` as `-object`, `--ignore-filename-regex` keeping only `libs/`; writes per-file and total line and branch percentages as JSON and prints the totals. Finds the tools with `xcrun --find` on macOS, `PATH` elsewhere; exit 2 with the reason when they are missing.
  - `compare before.json after.json`: totals and every file whose lines or branches covered went down; exit 1 when any did.
  - `--clean` empties `profiles/` before a run.
- [ ] Step 4: Run it once on the whole tree (`cmake --preset coverage`, build, `ctest --preset coverage`, report). Note the time it took and the totals in `docs/dev_setup.md`'s new "Coverage" paragraph and in the commit message.
- [ ] Step 5: `ctest --preset dev` still passes (the option is OFF there). Commit `build: a local clang coverage preset and report`.

### Task 3: Where the importer suites' time goes

A measuring task: it writes findings, not fixes. Its result decides Tasks 4 and 5.

**Files:** modify `docs/superpowers/specs/2026-10-13-test-cleanup-design.md` section 4 (findings), this plan (Tasks 4 and 5 written from them).

- [ ] Step 1: Profile the six slowest bodies (spec section 4's list) with `sample` on macOS while each runs alone (`--gtest_filter`, `--gtest_repeat=3` for `sample` to catch it): time in `git` subprocesses (`GitFixture`, `LegacyRepoBuilder` commits), in `run_import` (reader, writer, store), in `fresh_world` (migrations), in assertions (`snapshot_of`, history reads).
- [ ] Step 2: Count, for `OneHistoryOneResult` in both suites, the git commits it makes, the imports it runs and the worlds it opens; same for `ReadableRecordAfterAnUnreadableFirstRecordStartsTheCollection` and `VerifyAfterImportIsOk`.
- [ ] Step 3: Time `SetUp` alone: a `TEST_P` with an empty body under `--gtest_repeat=20` in a scratch build, not committed. Confirm the 10% bound or correct it.
- [ ] Step 4: Write the findings into spec section 4 (numbers, where the time is) and replace Tasks 4 and 5 below with concrete steps for the two largest causes. Commit `docs: where the importer tests spend their time`.

### Moved to its own row (T097): fewer git processes per import run (`GitReader`), was Task 4

Owner, 2026-10-18: this production change in `libs/dvc` is its own row, T097, not part of T036. It is kept here as T097's design; T036 does not build it.

Spec section 4, cause 1: every import run and every verify walk opens a new `GitReader`, which starts 4 git processes to open (`git --version`, then three `rev-parse`), and each batch starts 3 more (`log` for `commits`, `diff-tree` for `changes`, `cat-file --batch` for `fetch_blobs`). The importer tests drive hundreds of short runs on purpose, so these fixed costs are about 70% of both suites (14 056 git processes for one `ProjectImportTest` run, about 7 ms each). This task makes the reader start fewer processes for the same output; the tests are not changed. It changes production code in `libs/dvc` for speed, so it waits for the owner (Open questions) and comes with a measurement.

**Files:** modify `libs/dvc/src/git_reader.cpp` (`open` at `:388`, `commits` at `:537`, `changes` at `:586`), `libs/dvc/include/pychron/dvc/git_reader.hpp` (one new member), `libs/dvc/src/commit_walk.cpp` (`:116`–`120`); test `tests/dvc/test_git_reader.cpp`. Read `libs/ingest/AGENTS.md` first.

- [x] Step 1: Baseline. Build `pychron_dvc_tests` and `pychron_elctl_tests`; record `python3 tools/test_times.py run --preset dev --tests-regex 'ProjectImportTest|MetaImportTest|ImportCmd' --out before.xml`, and the git processes of one `Engines/ProjectImportTest.*` run (a logging `git` first on `PATH`, in `$TMPDIR`, as Task 3 did). Note the machine's load average: Task 3's numbers were taken at 8 to 10.
  - Done 2026-10-19 on `develop` `e76218f9`, Debug, SQLite, git 2.50.1 (Apple Git-155), 18 cores, load average 30 to 48 (other sessions; Task 3's was 8 to 10). `tools/test_times.py` is on T036's branch, not yet on `develop`: run from a scratch copy. ctest `-j1`: `Engines/ProjectImportTest` 255.2 s, `Engines/MetaImportTest` 50.5 s, `ImportCmd` 21.8 s, `ImportCmdParity` 15.5 s, 160 tests 346.0 s summed. Git processes (logging `git` first on `PATH`): `Engines/ProjectImportTest` 14 056 (2 702 `diff-tree`, 2 663 `rev-parse`, 2 600 `log`, 772 `--version`), `Engines/MetaImportTest` 3 720, `ImportCmd*` 2 850.
- [x] Step 2: `git --version` once per process. In `GitReader::open`, keep the version check's result (version, or the error) in a function-local static guarded by `std::once_flag`, keyed by nothing: the git on `PATH` does not change under a running process. Test first in `tests/dvc/test_git_reader.cpp`: two `open` calls on one `GitFixture` both succeed; the too-old-version path stays covered by the existing test of `parse_git_version`. Commit `perf(dvc): git's version is read once per process`.
  - Done: kept under a mutex in a function-local `std::optional`, not a `std::once_flag`, so that a failed read (git missing, a timeout) is asked again by the next open rather than kept; a version read is kept. Commit `perf(dvc): git's version is read once per process`.
- [x] Step 3: one `rev-parse` to open. Replace the three calls at `:427`–`444` by one: `git rev-parse --git-dir --is-shallow-repository --verify --quiet --end-of-options refs/heads/<branch>^{commit}` (checked on git 2.50: prints the git dir, `false`, the sha, exit 0; exit 1 with the first two lines when the branch is missing). On exit 1, fall back to today's code for the bare name and the error messages (not a repository, shallow, no commits, branch not found), so every error text stays as it is. Tests first: open on a repository, on a shallow clone (`GitFixture::clone_bare` with `--depth=1`), on a missing branch, on a branch named only by its bare name, outside a repository: each gives today's result and message. Confirm `--end-of-options` with these options on git 2.32 (minimum, `kMinimumGitVersion`) from git's release notes or a run on the Ubuntu CI image (git 2.43); if 2.32 refuses it, keep the separate calls for older versions. Commit `perf(dvc): a repository is opened with one rev-parse`.
  - Done: the fast path returns only on exit 0 with three lines (git dir, `false`, a sha); anything else falls through to the old calls unchanged. `rev-parse --end-of-options` is in git 2.30 and was already used here, so no version split. Tests: `OpensTwiceInOneProcess`, `OpensABranchGivenByAnotherName` (by ref, tag, sha), `OpenRejectsShallowClone` with a missing branch as well; the existing open tests (not a repository, a directory inside one, empty repository, missing branch) unchanged and passing.
- [x] Step 4: one `git log` for a batch's commits and changes. Add `GitReader::commits_and_changes(std::span<const std::string>)` returning both, from `git log --no-walk=unsorted --stdin -z --no-show-signature --format=<today's> --raw --root --diff-merges=first-parent --no-renames --no-abbrev`; `commits` and `changes` stay as they are (other callers: `project_map.cpp:277`). Test first: on fixture histories with a root commit, an empty commit, a merge, a deletion, a mode change, a non-ASCII path and a message holding a newline, the new call returns exactly what `commits` plus `changes` return. Then `commit_walk.cpp:116`–`120` uses it. Commit `perf(dvc): a batch's commits and changes come from one git log`.
  - Done: `commits_and_changes` also passes `-r`, `--no-relative`, `--no-color`, `--no-ext-diff` and an empty order file (`-O` null device), so `git log`'s own configuration (`diff.relative`, `diff.orderFile`) cannot make it differ from `diff-tree`. The test also covers an empty root commit, a merge equal to its first parent, a message line that looks like a change, another order, one commit and none (`CommitsAndChangesFromOneLogMatchCommitsPlusChanges`, `CommitsAndChangesRejectsWhatIsNotASha`).
- [x] Step 5: After. Same runs as Step 1 into `after.xml`; `python3 tools/test_times.py compare before.xml after.xml`; git process count after. Coverage of `libs/` before and after (Task 2's tool, as Global Constraints ask): lines and branches not lower. `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed`, `pychron_dvc_tests` and `pychron_elctl_tests --gtest_filter='ImportCmd*'` run whole, and the importer suites under `PYCHRON_TEST_PG_URL` too. Times, process counts and coverage go into the last commit's message (or a `docs:` commit that updates spec section 4 with them).
  - Done 2026-10-19, same machine and load as Step 1. ctest `-j1` (`test_times.py compare`): `Engines/ProjectImportTest` 255.2 -> 191.6 s, `Engines/MetaImportTest` 50.5 -> 51.5 s, `ImportCmd` 21.8 -> 33.9 s, `ImportCmdParity` 15.5 -> 22.4 s, total 346.0 -> 302.0 s; at this load a single run is noise for the small suites, so both builds were also run three times interleaved, one process each: both importer suites 346.7 -> 258.2 s wall, 242.8 -> 177.1 s user+sys (-27%); `ImportCmd*` 61.1 -> 56.2 s wall, 44.3 -> 34.1 s user+sys (-23%). Git processes: `Engines/ProjectImportTest` 14 056 -> 9 458 (`diff-tree` 2 702 -> 170, `rev-parse` 2 663 -> 1 286, `--version` 772 -> 83, the rest from `SKIP_WITHOUT_GIT`), `Engines/MetaImportTest` 3 720 -> 2 463, `ImportCmd*` 2 850 -> 2 187.
  - Coverage of `libs/` not measured: Task 2's tool is not on `develop` yet. No test removed; four added, and two (`IgnoresInheritedGitEnvironment`, `OpenRejectsShallowClone`) gained assertions; the old open path is still reached by every open-failure test.
  - `quality_check.py` 0 findings, `compiler_sweep.py --changed` 0 warnings, full `ctest --preset dev` 5661 passed, importer suites under `PYCHRON_TEST_PG_URL` 220 passed.

### Task 5: Second cause (written by Task 3)

As Task 4.

### Task 6: S2 folds in the Qtegra acquire tests

**Files:** modify `tests/devices/spectrometer/test_thermo_qtegra_acquire.cpp` (lines 88–146).

- [ ] Step 1: Read the tests in that range; list the ones whose bodies differ only in a value and an expectation. Any that differ in setup or in what they assert stay as they are.
- [ ] Step 2: Fold that list into one table test (a `struct Case` vector, loop with `SCOPED_TRACE(case.name)`), or a `TEST_P` with `INSTANTIATE_TEST_SUITE_P` naming each case, keeping every case's value and expectation. `QtegraAcquireSteady` cases are not touched.
- [ ] Step 3: Build `pychron_devices_tests` (`tests/devices/CMakeLists.txt:6`), run the file's suite; coverage before and after (Task 2) for `libs/devices`; commit `test(devices): one table for the Qtegra acquire cases that differ by a value`.

### Task 7: S2 folds in the legacy acquirers tests

As Task 6 for `tests/devices/spectrometer/legacy/test_acquirers.cpp` lines 176–207. Commit `test(devices): one table for the legacy acquirer cases that differ by a value`.

### Task 8: S4, `ImportCmdParity` to command paths

**Files:** modify `apps/elctl/tests/test_import_cmd.cpp` (`:1002` onward); possibly `tests/dvc/test_project_import.cpp` (an assertion moved to the engine test).

- [ ] Step 1: For each of the 13 tests, list its assertions; mark each as command path (argument parsing, exit status, output text, the store being the one named) or engine result (what the import wrote).
- [ ] Step 2: Every engine-result assertion: find the engine test that already makes it (`InterpretedAgeBecomesARevision` `:1857` and its neighbours); one that has none is moved there first, as its own commit, with the engine test passing.
- [ ] Step 3: Remove engine-result assertions from the parity tests; a parity test left with no command-path assertion is removed, named in the commit with its engine cover. One `build()` shared by what is left, if the remaining tests only read.
- [ ] Step 4: Times (Task 1) and coverage (Task 2) before and after; `pychron_elctl_tests --gtest_filter='ImportCmd*'`. Commit `test(elctl): import parity tests check the command, the engine tests check the import`.

### Task 9 onward: S1 and S3, one suite per commit

Not cut yet: the candidates (spec section 3, about 60 S1 names and the S3 names) are judged by reading bodies, suite by suite. Cut into tasks after Task 8 lands, from a list made then: `grep` of the S1 shapes per suite, read, and S3 twins by body. Each such task: list, removal with named cover, coverage and time before and after.

## Self-review

- Spec section 6's order kept: tooling (Tasks 1, 2), importer fixtures (3–5), S2 (6, 7), S4 (8), S1 and S3 (9 on). Section 4's "measure SetUp first" is Task 3, and its premise is already measured to be false for most of the time, so Tasks 4 and 5 are left to be written from Task 3's findings rather than guessed.
- Coverage gate (owner's answer 4) is a constraint on every task from 4 on; it needs Task 2 first.
- UI suites (answer 5): none touched.
