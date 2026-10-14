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

### Task 4: Cheapest change for the largest cause (written by Task 3)

Placeholder kept so the order is visible. Candidates, depending on Task 3: a once-per-suite migrated empty store copied per test (SQLite file copy; PostgreSQL `CREATE SCHEMA` from a migrated template schema); git history built once per suite into a template repository and copied (`cp -R` of `.git`) for tests that only read it; fewer commits where a test makes many that assert nothing. Each with times before and after and coverage unchanged; `OneHistoryOneResult` kept whole.

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
