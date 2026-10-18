# A resumed step heat keeps its aliquot: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After a restart (or in a later queue), the steps B, C ... of a heating with no fixed aliquot join the aliquot of the identifier's last saved step instead of taking a new one.

**Architecture:** `IAnalysisPersister` gains a read, `last_run(identifier)`, with a default that answers "none". `FilePersister` answers from the record file names, `StorePersister` from `IStore::last_analysis` plus the spool, and the `Acquisition` proxy forwards it to its worker thread. `AliquotAllocator::allocate` asks it once per identifier per session, only for a step after A when nothing is remembered in the session, and reuses the aliquot when the asked step comes after the saved one.

**Tech Stack:** C++20, GoogleTest, `libs/experiment`, `libs/experiment/adapters/store` (TinyORM on QtSql, SQLite and PostgreSQL), `apps/elctl`.

**Spec:** `docs/superpowers/specs/2026-10-13-resume-aliquot-design.md` (agreed 2026-10-15). Builds on T090 (`f56e037f`).

## Global Constraints

- Read `libs/experiment/AGENTS.md` before touching `libs/experiment` or `tests/experiment`. `libs/experiment` never links `libs/persistence`; only the store adapter does.
- No change to the store schema, the record format, `executor_state.json` or the queue file (spec 5).
- `last_run` is a read; it never issues a number and never changes what `next_aliquot` hands out (spec 5).
- Never reuse an (identifier, aliquot, step) already saved: reuse only when the step asked for is after the newest saved step of the newest aliquot (spec 5).
- An error from `last_run` fails the allocation with that error; there is no fallback to a new aliquot (spec 4.3).
- Store and file persisters answer the same for the same runs (spec 5).
- `AliquotAllocator::mutex_` is never held across a persister call (as today).
- Errors are `pychron::Result<T>` values. Headers `#pragma once`. `[[nodiscard]]` is not added to the existing virtuals (none of them have it); keep the style of the interface.
- After each C++ task: `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed`, build only the changed test targets and run them filtered. The full build, `ctest --preset dev` and a whole-tree sweep are run once, in Task 5.

## Review Focus

1. A step heat whose last saved run has no step (a plain `12345-3.json` after `12345-2B.json`): B must take a new aliquot, never 2 (the newest aliquot is 3, step -1). Test: Task 2 table row "newest aliquot has no step".
2. A spooled record (not yet in the store) newer than the store's last analysis: the store persister must answer the spooled one, or a resume while the database is down splits the heating. Test: Task 3 `LastRunCountsARecordWaitingInTheSpool`.
3. The UI and `elctl --db` reach the store persister through the `Acquisition` proxy (`Marshalled`, `store_persister.cpp:401`); if the proxy does not forward `last_run`, the default "none" is used silently and the fix does nothing there. Test: Task 3 `AcquisitionTest.ForwardsLastRun`.
4. The `.extraction.json` file of a run whose analysis was never saved: it counts as a saved step (its number is spent; `highest_on_disk` already counts it), so the next B after it is not given that same step again. Test: Task 1 `FilePersisterLastRunCountsAnExtractionFile`.
5. Database unreachable at resume: with a cached answer for the identifier the cached answer is used; with none, the run does not start and the operator sees the store's error. Test: Task 3 `LastRunSurvivesAnUnreadableStore`, Task 2 `AliquotAllocatorFailsWhenLastRunFails`.

---

### Task 1: `LastRun`, `IAnalysisPersister::last_run` and `FilePersister::last_run`

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/persist/persister.hpp` (struct `LastRun` above `IAnalysisPersister`; virtual `last_run` in it; override in `FilePersister`)
- Modify: `libs/experiment/src/persist/persister.cpp:64-79` (`highest_on_disk` and the new `last_run` share one file-name parser)
- Test: `tests/experiment/test_persist.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct LastRun {
    int aliquot = 0;
    int step = -1;  // step_index of its step; -1: no step
    bool operator==(const LastRun&) const = default;
  };
  // in IAnalysisPersister:
  // The identifier's run with the highest aliquot, and within it the highest
  // step, among runs saved or waiting to be sent. nullopt: none, or this
  // persister cannot say (the default).
  virtual Result<std::optional<LastRun>> last_run(const std::string& identifier) {
    return std::optional<LastRun>{};
  }
  // in FilePersister:
  Result<std::optional<LastRun>> last_run(const std::string& identifier) override;
  ```
- Consumes: `step_index(std::string_view)` from `pychron/experiment/factory/increments.hpp` ("A" → 0, "" → -1).

- [x] **Step 1: Write the failing tests** in `tests/experiment/test_persist.cpp`, using `PersistTest::dir_`. A helper writes an empty file `<dir_>/records/12345/<name>`.

```cpp
TEST_F(PersistTest, FilePersisterLastRunIsTheHighestAliquotThenItsHighestStep) {
  touch("12345-3A.json"); touch("12345-3B.json"); touch("12345-2.json");
  touch("99999-7C.json");  // another identifier
  FilePersister files(dir_ / "records");
  auto last = files.last_run("12345");
  ASSERT_TRUE(last) << last.error().what;
  ASSERT_TRUE(last->has_value());
  EXPECT_EQ(**last, (LastRun{3, 1}));
}
TEST_F(PersistTest, FilePersisterLastRunIsNoneWithoutFiles) {
  FilePersister files(dir_ / "records");
  auto last = files.last_run("12345");
  ASSERT_TRUE(last);
  EXPECT_FALSE(last->has_value());
}
TEST_F(PersistTest, FilePersisterLastRunOfANameWithNoStepHasStepMinusOne) {
  touch("12345-2B.json"); touch("12345-3.json");
  FilePersister files(dir_ / "records");
  EXPECT_EQ(files.last_run("12345").value(), (LastRun{3, -1}));
}
TEST_F(PersistTest, FilePersisterLastRunCountsAnExtractionFile) {
  touch("12345-3A.json"); touch("12345-3B.extraction.json");
  FilePersister files(dir_ / "records");
  EXPECT_EQ(files.last_run("12345").value(), (LastRun{3, 1}));
}
TEST_F(PersistTest, FilePersisterLastRunRefusesAnIdentifierThatIsNotAPlainName) {
  FilePersister files(dir_ / "records");
  EXPECT_FALSE(files.last_run("../x"));
}
TEST_F(PersistTest, FilePersisterLastRunIssuesNothing) {
  touch("12345-3B.json");
  FilePersister files(dir_ / "records");
  (void)files.last_run("12345");
  EXPECT_EQ(files.next_aliquot("12345").value(), 4);
}
```

Also, in the existing `FlakyPersister` (`test_persist.cpp:40`), nothing changes: a test `DefaultLastRunIsNone` asserts `FlakyPersister{}.last_run("12345")` is a value holding `nullopt` (the default stays).

- [x] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev --target pychron_experiment_tests && build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='PersistTest.*LastRun*'`
Expected: does not compile (`last_run`, `LastRun` not declared).

- [x] **Step 3: Implement**

In `persister.cpp`'s anonymous namespace, `std::optional<LastRun> parse_run_name(std::string_view name, std::string_view prefix)`: name must start with `<identifier>-` and end with `.json`; digits after the prefix are the aliquot (none: not a run); letters after them are the step, which must be followed by `.` (else not a run); no letters: step -1; `step_index` of the letters. `highest_on_disk` is left as it is (its digit scan counts names this parser refuses; its numbering must not change). `FilePersister::last_run` refuses a name `safe_name` refuses (`ErrorKind::Config`, as `next_aliquot`), walks `<root>/<identifier>/`, keeps the maximum by `(aliquot, step)`; a missing directory is `nullopt`, not an error. It does not take `mutex_` and does not touch `issued_`.

- [x] **Step 4: Run them to see them pass**

Run: `build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='PersistTest.*'`
Expected: all PASS (the existing `FilePersisterWritesRecordsAndCountsAliquots` too).

- [x] **Step 5: Checks**

Run: `python3 tools/quality_check.py && python3 tools/compiler_sweep.py --changed`
Expected: exit 0 from both.

- [x] **Step 6: Commit**

```bash
git add libs/experiment/include/pychron/experiment/persist/persister.hpp libs/experiment/src/persist/persister.cpp tests/experiment/test_persist.cpp
git commit -m "feat(experiment): a persister says an identifier's last saved run and step"
```

---

### Task 2: `AliquotAllocator` asks `last_run` for a step after A

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/persist/persister.hpp` (`AliquotAllocator`: its comment, new member `std::set<std::string> asked_`)
- Modify: `libs/experiment/src/persist/persister.cpp:316-341` (`AliquotAllocator::allocate`)
- Modify: `docs/user/04-experiments.md:136` (the `aliquot`, `step` row)
- Test: `tests/experiment/test_persist.cpp`

**Interfaces:**
- Consumes: `IAnalysisPersister::last_run`, `LastRun` (Task 1).
- Produces: no new names; `allocate`'s behaviour per spec 4.

- [x] **Step 1: Write the failing tests** in `test_persist.cpp`. A fake `SavedRunPersister : IAnalysisPersister` with `std::optional<LastRun> last` (default `LastRun{3, 1}`), `bool down = false` (then `last_run` returns `fail(ErrorKind::Io, "database down")`), `int asked = 0` (counts `last_run` calls), `next_aliquot` returning `next++` from `int next = 4`; the rest succeed as in `FlakyPersister`.

```cpp
// Each row a fresh allocator over a persister whose last saved run is 3B.
TEST_F(PersistTest, AliquotAllocatorJoinsTheLastSavedHeating) {
  struct Row { std::string step; int expected; };
  for (const Row& r : std::vector<Row>{{"C", 3}, {"D", 3}, {"B", 4}, {"A", 4}, {"", 4}}) {
    SavedRunPersister db;
    AliquotAllocator alloc(db);
    auto a = alloc.allocate(RunIdentity{"12345", std::nullopt, r.step});
    ASSERT_TRUE(a) << r.step;
    EXPECT_EQ(a->aliquot, r.expected) << "step '" << r.step << "'";
  }
}
TEST_F(PersistTest, AliquotAllocatorDoesNotJoinWhenTheNewestAliquotHasNoStep) {
  SavedRunPersister db;
  db.last = LastRun{3, -1};
  AliquotAllocator alloc(db);
  EXPECT_EQ(alloc.allocate(RunIdentity{"12345", std::nullopt, "B"})->aliquot, 4);
}
TEST_F(PersistTest, AliquotAllocatorWithNothingSavedTakesTheNextAliquot) {
  SavedRunPersister db;
  db.last.reset();
  AliquotAllocator alloc(db);
  EXPECT_EQ(alloc.allocate(RunIdentity{"12345", std::nullopt, "C"})->aliquot, 4);
}
TEST_F(PersistTest, AliquotAllocatorFailsWhenLastRunFails) {
  SavedRunPersister db;
  db.down = true;
  AliquotAllocator alloc(db);
  auto a = alloc.allocate(RunIdentity{"12345", std::nullopt, "C"});
  ASSERT_FALSE(a);
  EXPECT_EQ(a.error().what, "database down");
  EXPECT_EQ(db.next, 4);  // no aliquot was issued
}
TEST_F(PersistTest, AliquotAllocatorAsksOncePerIdentifierAndNeverForAOrNoStep) {
  SavedRunPersister db;
  AliquotAllocator alloc(db);
  ASSERT_TRUE(alloc.allocate(RunIdentity{"12345", std::nullopt, "A"}));  // 4A: not asked
  ASSERT_TRUE(alloc.allocate(RunIdentity{"12345", std::nullopt, ""}));   // not asked
  EXPECT_EQ(db.asked, 0);
  SavedRunPersister db2;
  AliquotAllocator alloc2(db2);
  EXPECT_EQ(alloc2.allocate(RunIdentity{"12345", std::nullopt, "C"})->aliquot, 3);
  EXPECT_EQ(alloc2.allocate(RunIdentity{"12345", std::nullopt, "D"})->aliquot, 3);  // memory answers
  EXPECT_EQ(alloc2.allocate(RunIdentity{"12345", std::nullopt, "B"})->aliquot, 4);  // not after D: T090
  EXPECT_EQ(alloc2.allocate(RunIdentity{"12345", std::nullopt, "C"})->aliquot, 4);
  EXPECT_EQ(db2.asked, 1);
}
TEST_F(PersistTest, AnAllocatorOverTheSameFilesContinuesAHeatingAfterARestart) {
  FilePersister files(dir_ / "records");
  {
    AliquotAllocator before(files);
    ASSERT_EQ(before.allocate(RunIdentity{"12345", std::nullopt, "A"})->aliquot, 1);
  }
  touch("12345-1A.json");  // what the first session saved
  FilePersister restarted(dir_ / "records");
  AliquotAllocator after(restarted);
  EXPECT_EQ(after.allocate(RunIdentity{"12345", std::nullopt, "B"})->aliquot, 1);
  EXPECT_EQ(after.allocate(RunIdentity{"12345", std::nullopt, "C"})->aliquot, 1);
  EXPECT_EQ(after.allocate(RunIdentity{"12345", std::nullopt, "A"})->aliquot, 2);
}
```

The existing `AliquotAllocatorKeepsTheStepsOfAHeatingTogether` stays as it is and must still pass (T090's rule unchanged: its `FilePersister` has no files, so `last_run` answers none).

- [x] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev --target pychron_experiment_tests && build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='PersistTest.*Allocator*'`
Expected: `AliquotAllocatorJoinsTheLastSavedHeating` row C gives 4, not 3; `...FailsWhenLastRunFails` gets a value; `...AfterARestart` B gives 2.

- [x] **Step 3: Implement `AliquotAllocator::allocate`**

After the in-session lookup (unchanged), when `step > 0`, no entry in `last_` for the identifier and the identifier not in `asked_`: insert it into `asked_` (under `mutex_`), release the lock, call `persister_.last_run(id.identifier)`. Error: `return fail(err)`. A value with `0 <= last->step && last->step < step`: `remember(last->aliquot)` and return `Allocation{last->aliquot, id.step}`. Otherwise fall through to `next_aliquot()` as now. Fixed aliquots, step A and no step never reach the call. Rewrite the class comment's last paragraph: what is remembered lives as long as the allocator; the first time a step after A comes for an identifier nothing is remembered for, the persister's `last_run` stands in for that memory, so a heating resumed after a restart, in another queue or on another day keeps its aliquot; an error from it fails the allocation.

- [x] **Step 4: Run them to see them pass**

Run: `build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='PersistTest.*'`
Expected: all PASS.

- [x] **Step 5: Update the user doc** `docs/user/04-experiments.md:136`: replace "a queue resumed after the program was restarted gives its remaining steps a new aliquot, so fix the aliquot of a step heat you may have to resume" with: a step heat resumed after a restart, or continued in a later queue, joins the aliquot of the identifier's last saved run when its step comes after that run's step (B, C ... after a saved A); otherwise the run takes the next free aliquot.

- [x] **Step 6: Checks**

Run: `python3 tools/quality_check.py && python3 tools/compiler_sweep.py --changed && ctest --preset dev-quick -R 'Persist|Executor|LabSession'`
Expected: exit 0; all selected tests pass.

- [x] **Step 7: Commit**

```bash
git add libs/experiment/include/pychron/experiment/persist/persister.hpp libs/experiment/src/persist/persister.cpp tests/experiment/test_persist.cpp docs/user/04-experiments.md
git commit -m "fix(experiment): a step heat resumed after a restart keeps its aliquot"
```

---

### Task 3: `StorePersister::last_run` and the `Acquisition` proxy

**Files:**
- Modify: `libs/experiment/adapters/store/include/pychron/experiment/store/store_persister.hpp:35-59` (declare `last_run`; `read_` becomes `std::map<std::string, persist::LastRun> read_` with `aliquot 0` meaning "store had none")
- Modify: `libs/experiment/adapters/store/src/store_persister.cpp:49-75` (`next_aliquot` reads its highest from `read_[...].aliquot`; new `last_run`), `:401-431` (`Marshalled::last_run` forwards through `impl_.call`)
- Test: `tests/experiment/test_store_persister.cpp` (target `pychron_experiment_store_tests`)

**Interfaces:**
- Consumes: `persist::LastRun`, `IAnalysisPersister::last_run` (Task 1); `ps::IStore::last_analysis(identifier) -> Result<std::optional<ps::LastAnalysis>>` (`aliquot`, `increment`, -1 for none; ordered `aliquot DESC, increment DESC`); `persist::Spool::pending()`, `get()`; `step_index`.
- Produces: `Result<std::optional<persist::LastRun>> StorePersister::last_run(const std::string& identifier) override;`

- [x] **Step 1: Write the failing tests.** A local helper `stepped(identifier, aliquot, step)` = `numbered(identifier, aliquot)` with `identity.step = step` and `identity.uuid = Uuid::v5(Uuid{}, identifier + "/" + aliquot + step)` (`numbered`'s uuid does not include the step: two steps of one aliquot would otherwise be one analysis).

```cpp
TEST_P(StorePersisterTest, LastRunFollowsTheStore) {
  ASSERT_TRUE(persister_->save_analysis(stepped("66573", 2, "")));
  ASSERT_TRUE(persister_->save_analysis(stepped("66573", 3, "A")));
  ASSERT_TRUE(persister_->save_analysis(stepped("66573", 3, "B")));
  EXPECT_EQ(persister_->last_run("66573").value(), (persist::LastRun{3, 1}));
  EXPECT_FALSE(persister_->last_run("66574").value().has_value());
}
TEST_P(StorePersisterTest, LastRunCountsARecordWaitingInTheSpool) {
  ASSERT_TRUE(persister_->save_analysis(stepped("66573", 3, "A")));
  persist::Spool spool(data_ / "spool");
  ASSERT_TRUE(spool.put(stepped("66573", 3, "B")));
  ASSERT_TRUE(spool.put(stepped("66574", 9, "C")));  // another identifier's
  EXPECT_EQ(persister_->last_run("66573").value(), (persist::LastRun{3, 1}));
}
TEST_P(StorePersisterTest, LastRunSurvivesAnUnreadableStore) {
  ASSERT_TRUE(persister_->save_analysis(stepped("66573", 3, "A")));
  ASSERT_EQ(persister_->last_run("66573").value(), (persist::LastRun{3, 0}));
  failing_->fail_reads(true);
  EXPECT_EQ(persister_->last_run("66573").value(), (persist::LastRun{3, 0}));  // the cached answer
  const fs::path other = scratch_dir();
  store::StorePersister fresh(*failing_, lab_.acquisition_client, other);
  EXPECT_FALSE(fresh.last_run("66573"));  // nothing cached: the error
  std::error_code ignored;
  fs::remove_all(other, ignored);
}
TEST_P(StorePersisterTest, LastRunIssuesNothing) {
  ASSERT_TRUE(persister_->save_analysis(stepped("66573", 3, "B")));
  (void)persister_->last_run("66573");
  EXPECT_EQ(persister_->next_aliquot("66573").value(), 4);
}
// Spec 5: the store and the files answer the same for the same runs.
TEST_P(StorePersisterTest, LastRunIsTheSameFromTheStoreAndFromFiles) {
  const fs::path files_dir = scratch_dir();
  persist::FilePersister files(files_dir);
  for (const auto& r : {stepped("66573", 2, ""), stepped("66573", 3, "A"), stepped("66573", 3, "C"),
                        stepped("66573", 1, "D")}) {
    ASSERT_TRUE(files.begin_run(RunIdentity{r.identity.identifier, r.identity.aliquot, r.identity.step}, {}));
    ASSERT_TRUE(files.save_analysis(r));
    ASSERT_TRUE(persister_->save_analysis(r));
  }
  EXPECT_EQ(files.last_run("66573").value(), persister_->last_run("66573").value());
  EXPECT_EQ(files.last_run("66574").value(), persister_->last_run("66574").value());
  std::error_code ignored;
  fs::remove_all(files_dir, ignored);
}
TEST_P(AcquisitionTest, ForwardsLastRun) {
  auto acquisition = open();
  ASSERT_TRUE(acquisition);
  auto& persister = acquisition->persister();
  ASSERT_TRUE(persister.save_analysis(stepped("66573", 1, "A")));
  EXPECT_EQ(persister.last_run("66573").value(), (persist::LastRun{1, 0}));
}
```

(`RunIdentity`'s aggregate order is `{identifier, aliquot, step}` as `test_persist.cpp` uses it. If `AcquisitionTest`'s `open()` needs a catalog row for `66573`, follow `ServesCallsFromOtherThreads`, which saves `numbered("66573", 1)` the same way.)

- [x] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev --target pychron_experiment_store_tests && build/dev/tests/experiment/pychron_experiment_store_tests --gtest_filter='*LastRun*'`
Expected: `StorePersisterTest.*` answers `nullopt` (the default); `ForwardsLastRun` answers `nullopt`.

- [x] **Step 3: Implement `StorePersister::last_run`**

Same shape as `next_aliquot`: `store_.last_analysis(identifier)` outside the lock; under `mutex_`, on success map it (`increment` → `step`; none → `LastRun{0, -1}`) and store it in `read_[identifier]`; on failure use `read_[identifier]` when present, else return the error. Outside the lock, read the spool as `next_aliquot` does (a spool that cannot be listed: its error; a record that cannot be read: skipped) and take the maximum by `(aliquot, step_index(step))` over the identifier's records. Answer `nullopt` when the result's aliquot is 0. Do not touch `issued_`. `next_aliquot` keeps its results: it reads `read_[identifier].aliquot` where it read the `int` before. Add `Marshalled::last_run` beside `next_aliquot` in the proxy.

- [x] **Step 4: Run them to see them pass**

Run: `build/dev/tests/experiment/pychron_experiment_store_tests` (all of it: the existing `Aliquots*` tests guard the `read_` change). With `PYCHRON_TEST_PG_URL` set (see `docs/dev_setup.md`) the PostgreSQL engine runs too; without it those cases skip.
Expected: all PASS.

- [x] **Step 5: Checks**

Run: `python3 tools/quality_check.py && python3 tools/compiler_sweep.py --changed`
Expected: exit 0 from both.

- [x] **Step 6: Commit**

```bash
git add libs/experiment/adapters/store/include/pychron/experiment/store/store_persister.hpp libs/experiment/adapters/store/src/store_persister.cpp tests/experiment/test_store_persister.cpp
git commit -m "feat(experiment): the store persister says an identifier's last saved run"
```

---

### Task 4: Resume tests through the executor and `elctl --resume`

Tests only: Tasks 1 to 3 make them pass. Before Task 2 they would see B and C at aliquot 2.

**Files:**
- Test: `tests/experiment/test_executor.cpp` (beside `ResumeStartsAfterTheLastStartedRun`, line 567)
- Test: `apps/elctl/tests/test_exp_commands.cpp` (fixture `ElctlExpTest`)

**Interfaces:**
- Consumes: `Executor::resume_row`, `ExecutorTest::context()` (sets `s.aliquots = &lab_.aliquots`, line 81), `lab_.files`; `elctl exp run ... --resume`, which starts at `next_row` of `<data>/executor_state.json`.

- [ ] **Step 1: Write the executor test**

`ExecutorTest.AResumedStepHeatKeepsItsAliquot`: `RunSpec` `a`, `b`, `c` = `unknown_run("12345")` with `id.step` `"A"`, `"B"`, `"C"`. First session: `Executor ex(context(), options())` runs `queue({a})` to `Completed` (the program stopped after A; a deterministic stand-in for a crash between runs). Assert `Executor::resume_row(lab_.dir() / "executor_state.json")` is 1. Second session: `persist::AliquotAllocator fresh(lab_.files); auto ctx = context(); ctx.aliquots = &fresh;` a new `Executor`, `execute(queue({a, b, c}), 1)`. Assert: `Completed`, `states(r) == {"12345:success", "12345:success"}`; files `12345/12345-1B.json` and `12345/12345-1C.json` exist under `lab_.files`' root; `12345-2B.json` does not.

- [ ] **Step 2: Run it**

Run: `cmake --build --preset dev --target pychron_experiment_tests && build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='ExecutorTest.AResumedStepHeatKeepsItsAliquot'`
Expected: PASS.

- [ ] **Step 3: Write the `elctl` test**

`ElctlExpTest.AResumedStepHeatKeepsItsAliquot` (skips without scripting, as its neighbours): write `<lab>/steps.toml` with the `[queue]` table of `configs/examples/experiment.toml` and one `[[runs]]` row of `66001`, `step = "A"`, the example's extraction, measurement and post-measurement. Run it (`exp({"run", lab("steps.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"), "--data", data, "--sim-speed", "max"}, true)`), code 0. Rewrite `steps.toml` with rows A, B, C; run again with `--resume` (a new process: a new allocator; `--resume` starts at `next_row` 1 of the first run's state file). Assert code 0, `"2/2 run(s)"` in the output, `records/66001/66001-1B.json` and `66001-1C.json` exist, `66001-2B.json` does not.

- [ ] **Step 4: Run it**

Run: `cmake --build --preset dev --target pychron_elctl_tests && build/dev/apps/elctl/tests/pychron_elctl_tests --gtest_filter='ElctlExpTest.AResumedStepHeatKeepsItsAliquot'`
Expected: PASS.

- [ ] **Step 5: Checks**

Run: `python3 tools/quality_check.py && python3 tools/compiler_sweep.py --changed`
Expected: exit 0 from both.

- [ ] **Step 6: Commit**

```bash
git add tests/experiment/test_executor.cpp apps/elctl/tests/test_exp_commands.cpp
git commit -m "test(experiment): a step heat resumed by the executor and by elctl keeps its aliquot"
```

---

### Task 5: Spec status and the full run

**Files:**
- Modify: `docs/superpowers/specs/2026-10-13-resume-aliquot-design.md` (status line: built, with the branch; any departure made in Tasks 1 to 4 written into sections 3 to 6)

- [ ] **Step 1:** Update the spec's status line; check sections 3 to 6 against the code (`read_` change, `Marshalled` forwarding, `.extraction.json` counted) and write in what the code settled that the spec did not say. Compressed, as `AGENTS.md` says for specs.
- [ ] **Step 2:** Run `cmake --build --preset dev` (timeout 3600000 ms). Expected: builds.
- [ ] **Step 3:** Run `ctest --preset dev` (timeout 3600000 ms). Expected: 100% pass.
- [ ] **Step 4:** Run `python3 tools/compiler_sweep.py` (whole tree) and `python3 tools/quality_check.py`. Expected: exit 0, no warnings.
- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/specs/2026-10-13-resume-aliquot-design.md
git commit -m "docs(experiment): the resume aliquot spec says what was built"
```

## Open questions

None.
