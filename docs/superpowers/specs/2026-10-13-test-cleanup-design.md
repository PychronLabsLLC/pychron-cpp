# Test cleanup: what counts as superfluous

Draft for T036 ("cleanup tests. remove superfluous or excessive tests").
Agreed with owner 2026-10-13 (section 7). Nothing removed yet. UI suites
(`tests/ui`) out of scope.

## 1. Why

- Size: about 4600 GoogleTest cases (`TEST`, `TEST_F`, `TEST_P` at line
  start, `tests/` and `apps/elctl/tests`) plus about 830 QtTest slots in
  `tests/ui` (51 files). Tests 141 000 lines against 176 000 of source.
- Time (`ctest --preset dev-ui`, 2026-10-13): 932 s summed, about 8 min wall.
  Importer suites a third: `Engines/ProjectImportTest` 82 tests 243 s,
  `Engines/MetaImportTest` 26 tests 40 s, `ImportCmd` with parity 50 tests
  36 s. Then `LabSim` 9 tests 41 s, `ui.test_experiment_window` 31 s,
  `LabSessionTest` 27 tests 26 s, `ui.test_flux_window` 24 s.
- `dev-quick` already leaves the importer suites out
  (`CMakePresets.json:176`–`183`), so the cost lands on every full run
  before a merge and on CI.

Goal: same protection, less time and fewer lines to read. Removing a test is
a last resort; making it cheaper or folding it into a table comes first.

## 2. Rules that must hold

- Root `AGENTS.md` stands: a failing test is never skipped or disabled; a
  test failing only in parallel is fixed, not serialized; `*Steady` tests
  (`ExampleLineSteady`, `NgxLinkSteady`, `QtegraAcquireSteady`,
  `AdcBankSteady`), the pacing tests of `VirtualClock` and
  `AnInterruptStopsAPacedQueueInRealTime` are kept by design.
- A test that once caught a bug stays (its commit history or a comment says
  so), whatever the rules below say.
- Each removal or merge commit names the test(s) it drops and the test(s)
  that still cover the same behaviour. One suite per commit; full
  `ctest --preset dev` (or `dev-ui`) before each.
- A file-format test (bytes, sha, `TOML_FLOAT_CHARCONV`, legacy model
  strings, changeset messages) and a test against a fixture written on
  another platform are never removed.
- PostgreSQL runs of `TEST_P` suites (`store_fixture.hpp:32`) are kept: the
  instance count is the engines', not duplication.
- Coverage (lines and branches of `libs/`) measured before and after each
  suite's change does not drop. Measured how: section 5.

## 3. What counts as superfluous (proposal)

A test is a candidate when one of these holds and section 2 lets it go:

S1. **Asserts nothing behaviour-shaped**: only that a constructor's default
    or a getter returns what was set, with no rule behind it. (A default that
    is a rule, such as `DefaultsComeFromTheSchema`
    `tests/processing/test_options.cpp:29`, is not one.) About 60 names look
    like this; each read before it is called one.
S2. **A one-literal copy**: two or more tests whose bodies differ in a value
    and an expectation. Folded into one table test or `TEST_P`, not deleted.
    Candidates: `tests/devices/spectrometer/test_thermo_qtegra_acquire.cpp:88`–`146`,
    `tests/devices/spectrometer/legacy/test_acquirers.cpp:176`–`207`.
S3. **Covered twice at the same level**: same behaviour, same layer, same
    inputs, different name. Candidates by name, not yet by body:
    `SecondRunIsNoOp`, `OneHistoryOneResult`, `VerifyAfterImportIsOk`,
    `OpenAndPlanErrors`, `UnreadableVersionIsSupersededByAReadableOneOrADeletion`
    in `tests/dvc/test_project_import.cpp`, `test_meta_import.cpp`,
    `test_catalog_db.cpp`. Likely per-format twins (different code paths):
    kept unless their bodies show the same path.
S4. **CLI test that re-proves the engine**: an `elctl` test whose assertions
    are the engine test's, through the command line. Kept: one per command
    path (argument parsing, exit status, output text); assertions on the
    engine's result beyond that move to the engine test. Candidates:
    `ImportCmdParity` (`apps/elctl/tests/test_import_cmd.cpp:1002`, 13 tests)
    against `InterpretedAgeBecomesARevision`
    (`tests/dvc/test_project_import.cpp:1857`).

Not superfluous: a UI test of what a library test also covers, when the UI
test checks the window's wiring (an action enabled, a status line, a
signal). Long tests are not superfluous for being long.

## 4. Making the slow ones cheap (proposal, before any removal)

- `ProjectImportTest` (`tests/dvc/test_project_import.cpp:319`),
  `MetaImportTest` (`tests/dvc/test_meta_import.cpp:299`) and `ImportCmd`
  (`apps/elctl/tests/test_import_cmd.cpp:82`) rebuild a git repo, a store and
  a catalog in every `SetUp`. No `SetUpTestSuite` exists in the tree.
  First measure `SetUp` against the test body (section 5). If `SetUp`
  dominates: build the repo and the empty store once per suite and copy the
  directory (and the SQLite file) per test; PostgreSQL gets a template
  database (`CREATE DATABASE ... TEMPLATE`). A test that only reads
  (`VerifyAfterImportIsOk` :2708, `OneHistoryOneResult` :2564) may share one
  imported world.
- Rule: a shared world is never written by a test; a test that writes gets a
  copy. A test that fails only when run with its suite is a bug (root
  `AGENTS.md`).
- Measured 2026-10-13 (`pychron_dvc_tests`, Debug, SQLite, M-series Mac,
  one process): `ProjectImportTest` 82 tests 125 s, median 0.49 s, fastest
  0.17 s; `MetaImportTest` 26 tests 34 s, median 0.28 s, fastest 0.13 s.
  `SetUp` is at most the fastest test, so at most about 10% of a suite:
  sharing it per suite is not the first thing. Three bodies hold 52 s of
  `ProjectImportTest` (`OneHistoryOneResult` 28 s,
  `ReadableRecordAfterAnUnreadableFirstRecordStartsTheCollection` 18 s,
  `VerifyAfterImportIsOk` 7 s) and 27 s of `MetaImportTest`
  (`OneHistoryOneResult` 18 s, `VerifyAfterImportIsOk` 5 s,
  `UnreadableVersionIsSupersededByAReadableOneOrADeletion` 4 s). So:
  profile those bodies first; a shared world only where `SetUp` is shown
  to matter. `OneHistoryOneResult` is kept whole whatever it costs
  (`libs/ingest/AGENTS.md`): made cheaper, never split or cut.
- `LabSim`, `LabSessionTest`, `ui.test_experiment_window`: look for real
  waits that a `VirtualClock` would remove before anything else.

## 5. Measuring

- Time per test: `ctest --preset dev -j1 --output-junit` timings, and
  GoogleTest's own per-test times; `SetUp` timed with a scoped timer in the
  fixture under an environment variable (`PYCHRON_TEST_TIMING`), not left on.
- Coverage: a `coverage` configure preset (`-fprofile-instr-generate
  -fcoverage-mapping`, clang), `llvm-cov report` on `libs/` before and after
  each suite's change; the two reports in the commit message. Not in CI.
- Per-test unique coverage (which test is the only one to reach a line) is
  out of reach without per-test profiles; S3 is judged by reading bodies.

## 6. Order of work (for the plan)

1. Measurement tooling (timing hook, coverage preset), no test changed.
2. Importer fixtures shared per suite (section 4), times before and after.
3. S2 folds, one file per commit.
4. S4: `ImportCmdParity` trimmed to command paths.
5. S1 and S3 removals, one suite per commit, each with its named cover.

## 7. Decisions for owner

1. Goal in numbers: a wall-time target for `ctest --preset dev`, or only "less"?
2. Rules S1 to S4: agreed, changed, or more?
3. Section 4 before any removal: agreed?
4. Coverage gate (section 2, last rule): wanted, given it needs a local
   coverage build?
5. UI suites (`tests/ui`) in scope, or library suites only for now?

Owner's answers (2026-10-13): (1) less, no number; (2) S1 to S4 agreed;
(3) section 4 before any removal: agreed; (4) coverage gate wanted; (5)
library suites only: `tests/ui` out of scope.

## 8. Out of scope

- Adding tests for gaps found on the way: noted in the task inbox, not done
  here.
- CI matrix changes, test sharding, ccache.
- Changing what `dev-quick` leaves out.
