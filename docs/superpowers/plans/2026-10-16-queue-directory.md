# Queue Directory Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Queue files live in `<lab>/queues/` and blocks in `<lab>/queues/blocks/`; `elctl` and the UI find a queue's lab and a queue by name through one library function each, and the experiment window gets File > Open Recent.

**Architecture:** `libs/experiment` `lab/lab.hpp` gains the paths (`LabPaths::queues()`, `LabPaths::blocks()`) and two pure functions (`lab_for_queue`, `find_queue`); `elctl exp` and the UI call them instead of their own path arithmetic. The switch of `load_lab` to `queues/blocks/`, the profile change and the move of the example lab land in one commit, so the tree never has a block the loader no longer reads. The UI's file dialogs get test hooks (as `set_ask_unsaved` already is) so their start directory can be asserted; recent queues are a small QSettings-backed class with its own test.

**Tech Stack:** C++20, GoogleTest (`libs/experiment`, `elctl`, setup, integration), Qt 6 Widgets + QtTest (`apps/pychron-ui`, `tests/ui`), TOML profiles.

**Spec:** `docs/superpowers/specs/2026-10-13-queue-directory-design.md`

## Global Constraints

- Layout: queues `<lab>/queues/*.toml`; blocks `<lab>/queues/blocks/*.toml`; old `<lab>/blocks/` not read, not moved, no problem line.
- Only `*.toml` directly in `queues/` are queues; no subdirectory of `queues/` is listed or walked.
- Lab of a queue: its directory; if that directory is named `queues`, its parent. No walk past one level.
- With an install (or the UI's lab), a relative queue name not found from cwd is looked up in `<lab>/queues/`, then `<lab>/`.
- Data directory stays `<lab>/data`, derived from the lab, never from the queue's directory.
- Queue file format unchanged: no `schema_version` change, no key added.
- QSettings keys: `experiment_window/last_queue` (unchanged), `experiment_window/recent_queues` (new, absolute paths, newest first, at most 10). UI state of this workstation, not the line's override file.
- Profile renders `queues/experiment.toml` and `queues/blocks/blank_pair.toml`; shipped queue stays `ba, a, a, a, ba` (`libs/setup/AGENTS.md`).
- Example lab files moved with `git mv`; every test and doc line naming them updated in the same commit.
- `.github/workflows/release.yml` smoke command `elctl --install argus exp run experiment.toml` is not edited: it must keep working through the lookup.
- Commits: Conventional Commits, component as scope; each task leaves the tree building and its tests passing.

## Review Focus

- A queue in `<lab>/queues/` run by `elctl` without `--lab` or `--data`: records go to `<lab>/data/records`, and no `<lab>/queues/data` appears (test in Task 2).
- An install made the old way (root `experiment.toml`, root `blocks/`): `--install lab exp validate experiment.toml` still passes, its root blocks are not loaded and `Lab::problems` stays empty (tests in Tasks 2 and 3).
- `last_queue` names a file whose directory has since been deleted: Open starts in `<lab>/queues/`, not in a missing directory (test in Task 4).
- The same queue reached by two spellings (relative path, `queues/../queues/x.toml`) is one Open Recent entry, not two (test in Task 5).
- A queue deleted after it was opened: left out of Open Recent when the menu is next built, and dropped from the stored list (test in Task 5).

---

### Task 1: Queue paths and lookups in `libs/experiment`

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/lab/lab.hpp` (`LabPaths`, two free functions)
- Modify: `libs/experiment/src/lab/lab.cpp`
- Modify: `docs/superpowers/specs/2026-10-13-queue-directory-design.md` section 2 (`LabPaths` gains member functions, not fields: see Step 3)
- Test: `tests/experiment/test_lab.cpp` (target `pychron_experiment_tests`)

**Interfaces:**
- Produces:
  - `std::filesystem::path LabPaths::queues() const` → `dir / "queues"`
  - `std::filesystem::path LabPaths::blocks() const` → `dir / "queues" / "blocks"`
  - `std::filesystem::path lab_for_queue(const std::filesystem::path& queue)` (namespace `pychron::experiment::lab`)
  - `std::filesystem::path find_queue(const std::filesystem::path& queue, const std::filesystem::path& lab)`

- [x] **Step 1: Write the failing tests** in `tests/experiment/test_lab.cpp`

```cpp
TEST(LabPathsTest, QueuesAndBlocksAreUnderTheLab) {
  const LabPaths p{fs::path("/l")};
  EXPECT_EQ(p.queues(), fs::path("/l/queues"));
  EXPECT_EQ(p.blocks(), fs::path("/l/queues/blocks"));
}

TEST(LabForQueueTest, AQueuesDirectoryBelongsToItsParent) {
  EXPECT_EQ(lab_for_queue("/l/queues/x.toml"), fs::path("/l"));
  EXPECT_EQ(lab_for_queue("/l/x.toml"), fs::path("/l"));
  EXPECT_EQ(lab_for_queue("x.toml"), fs::path("."));
  EXPECT_EQ(lab_for_queue("queues/x.toml"), fs::path("."));
  EXPECT_EQ(lab_for_queue("/l/queues/sub/x.toml"), fs::path("/l/queues/sub"));  // no walk past one level
}
```

`FindQueueTest` on a `unique_temp_path` directory `t` (cwd is not changed; names are unique so nothing in cwd matches):
- `QueuesComesFirst`: write `t/queues/t041-a.toml` and `t/t041-a.toml`; `find_queue("t041-a.toml", t) == t / "queues" / "t041-a.toml"`.
- `TheLabRootIsTheFallback`: only `t/t041-b.toml`; result `t / "t041-b.toml"`.
- `NotFoundIsReturnedAsGiven`: nothing written; `find_queue("t041-c.toml", t) == fs::path("t041-c.toml")`.
- `AnAbsolutePathIsLeftAlone`: `find_queue(t / "queues" / "x.toml", t)` returns its argument even when only `t/x.toml` exists.
- `NoLabNoLookup`: `find_queue("t041-a.toml", fs::path()) == fs::path("t041-a.toml")`.

- [x] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev --target pychron_experiment_tests && build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='LabPathsTest.*:LabForQueueTest.*:FindQueueTest.*'`
Expected: compile error, `queues` / `lab_for_queue` / `find_queue` not declared.

- [x] **Step 3: Implement**

`queues()` and `blocks()` are inline member functions of `LabPaths`, not fields: `LabPaths` is aggregate-initialised as `{dir, line, spectrometer}` at about 20 sites, and a derived field would have to be kept in step by each. Change the spec's section 2 bullet to "`LabPaths` gains `queues()` (`<lab>/queues`) and `blocks()` (`<lab>/queues/blocks`)" in this commit (AGENTS.md: spec and code never disagree).

`lab_for_queue`: `parent = queue.parent_path()`; empty → `"."`; `parent.filename() == "queues"` → `parent.parent_path()`, or `"."` when that is empty; else `parent`.

`find_queue`: return `queue` unchanged when it is absolute, when `lab` is empty, or when `fs::exists(queue)`; else the first of `lab / "queues" / queue`, `lab / queue` that exists; else `queue`. Use the `std::error_code` overloads, as `load_lab` does.

Add both declarations under `load_lab` with a comment naming the spec rule. Do not touch the blocks loader yet (Task 3).

- [x] **Step 4: Run the tests to see them pass**

Run: same command as Step 2. Expected: all PASS.

- [x] **Step 5: Static analysis**

Run: `python3 tools/quality_check.py` and `python3 tools/compiler_sweep.py --changed`. Expected: exit 0, no warnings.

- [x] **Step 6: Commit**

```bash
git add libs/experiment/include/pychron/experiment/lab/lab.hpp libs/experiment/src/lab/lab.cpp tests/experiment/test_lab.cpp docs/superpowers/specs/2026-10-13-queue-directory-design.md
git commit -m "feat(experiment): queue paths, lab_for_queue and find_queue"
```

---

### Task 2: `elctl exp` finds the lab and the queue through the library

**Files:**
- Modify: `apps/elctl/src/exp.cpp:568-574` (argument resolution: the install lookup at 568-571, the `--lab` default at 574). Line 557 (`a.lab = globals.lab` for an install, when no `--lab` is given) stays, so `lab_for_queue` applies only when neither `--lab` nor an install is given.
- Modify: `apps/elctl/src/exp.hpp:9-10` (`--lab` default in the comment), `apps/elctl/src/cli.cpp:68-71` (help: `--lab` default)
- Modify: `docs/user/08-elctl-reference.md` (`--lab` default)
- Test: `apps/elctl/tests/test_exp_commands.cpp` (target `pychron_elctl_tests`)

**Interfaces:**
- Consumes: `lab_for_queue`, `find_queue` (Task 1).

- [x] **Step 1: Write the failing tests** in `test_exp_commands.cpp`

- `ElctlExpTest.ALabDefaultsToTheParentOfItsQueuesDirectory`: copy the test's example lab (`dir_ / "lab"`) to `dir_ / "q"`, create `q/queues/` and move `q/experiment.toml` into it (the example lab moves only in Task 3). `exp({"validate", (q/"queues"/"experiment.toml").string(), "--spectrometer", (q/"spectrometer.sim-integrated.toml").string()})` exits 0 (plans and scripts found, so the lab is `q`, not `q/queues`).
- `ElctlExpTest.ItsDataGoesToTheLabNotTheQueuesDirectory`: same `q`; `exp run` of that queue with `--spectrometer` and no `--data`, as the existing run tests do (`--sim-speed max` where they use it); exit 0, `fs::exists(q / "data" / "records")`, `!fs::exists(q / "queues" / "data")`.
- `ElctlInstallExpTest` (the fixture with `root_`, an `init argus` install named `lab`), `AQueueByNameIsFoundInQueues`: move `root_/experiment.toml` to `root_/queues/experiment.toml`; `run_raw({"--install", "lab", "--sim", "exp", "validate", "experiment.toml"})` exits 0. (The old-way install, root `experiment.toml`, stays covered by `ElctlSetupTest.AnInstrumentInstallsChecksAndRunsByName`, line 54.) If the install fixture exists only under one persistence branch of the file, put the test where both builds compile it, using `run_raw({"init", ...})` as `WithoutPersistenceAnInstallsRunsAreFilesAndItSaysSo` does.

- [x] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev --target pychron_elctl_tests && build/dev/apps/elctl/tests/pychron_elctl_tests --gtest_filter='*ALabDefaultsToTheParentOfItsQueuesDirectory:*ItsDataGoesToTheLabNotTheQueuesDirectory:*AQueueByNameIsFoundInQueues'`
Expected: the first two FAIL (unknown plan/script: lab taken as `q/queues`), the third FAIL (queue not found).

- [x] **Step 3: Implement** in `exp.cpp`

Replace the install lookup block with `if (!globals.lab.empty()) a.queue_file = lab::find_queue(a.queue_file, globals.lab);` and the `--lab` default with `if (a.lab.empty()) a.lab = lab::lab_for_queue(a.queue_file);`. `a.data = a.lab / "data"` stays as is. Help and `exp.hpp` comment: "default: the queue file's directory, or its parent when that is `queues/`". Same words in `08-elctl-reference.md`.

- [x] **Step 4: Run the tests to see them pass**

Run: same as Step 2, then the whole binary `build/dev/apps/elctl/tests/pychron_elctl_tests`. Expected: all PASS.

- [x] **Step 5: Static analysis**

Run: `python3 tools/quality_check.py` and `python3 tools/compiler_sweep.py --changed`. Expected: exit 0.

- [x] **Step 6: Commit**

```bash
git add apps/elctl/src/exp.cpp apps/elctl/src/exp.hpp apps/elctl/src/cli.cpp apps/elctl/tests/test_exp_commands.cpp docs/user/08-elctl-reference.md
git commit -m "feat(elctl): a queue's lab and an install's queue found under queues/"
```

---

### Task 3: Blocks from `queues/blocks/`; profile and example lab move there

One commit (spec section 7, item 5): once the old place is not read, the shipped and example blocks must already be in the new one. Read `libs/setup/AGENTS.md` before touching `profiles/` or `tests/setup`.

**Files:**
- Modify: `libs/experiment/src/lab/lab.cpp:154-164` (blocks loader), `libs/experiment/include/pychron/experiment/lab/lab.hpp:15,103` (layout comment, `blocks` member comment)
- Modify: `apps/pychron-ui/src/run_factory_panel.cpp:320` (tooltip uses `lab_.paths.blocks()`)
- Modify: `profiles/instrument-common/profile.toml:67-72` (`to = "queues/blocks/blank_pair.toml"`, `to = "queues/experiment.toml"`; `template` lines unchanged), and the run command in the comment of `profiles/instrument-common/experiment.toml:2` if it names a path (it names `experiment.toml`, which the lookup still finds: leave it)
- Move (`git mv`): `configs/examples/experiment.toml`, `experiment.laser.toml`, `experiment.sim-air.toml` → `configs/examples/queues/`; `configs/examples/blocks/blank_pair.toml` → `configs/examples/queues/blocks/blank_pair.toml`. Update the run command in each moved queue's header comment to `exp run queues/<file>`.
- Modify (paths only, `experiment*.toml` → `queues/experiment*.toml` for the example lab or a rendered install): `tests/experiment/test_lab.cpp:30`, `tests/experiment/test_lab_extraction.cpp:58`, `tests/integration/test_lab_session.cpp:170,193`, `tests/integration/test_outage.cpp:98`, `tests/integration/test_lab_sim.cpp:166`, `tests/integration/test_install_e2e.cpp:241`, `tests/setup/test_profiles.cpp:81,127,217`, `tests/ui/experiment_fixture.hpp:45`, `tests/ui/test_experiment_window.cpp:683`, `tests/ui/test_run_factory_panel.cpp:1` (comment), `apps/elctl/tests/test_exp_commands.cpp` (every `lab("experiment*.toml")` and every `root / "experiment.toml"` / `root_ / "experiment.toml"`; `test_setup_commands.cpp:54` stays: it is the by-name lookup). Tests that write their own `experiment.toml` at a temp lab's root (`test_queue_file.cpp:144`) stay: a queue outside `queues/` is legal.
- Modify docs (spec section 5): `docs/user/01-getting-started.md`, `04-experiments.md` (layout at `:25`-`:32`, blocks at `:510`-`:511`, commands at `:200`, `:1034`, `:1054`-`:1070`), `07-configuration-reference.md` (`:124`, `:534`-`:553`, `:692`, setup section `:953`, `:1032`), `03-spectrometer.md:218,526` and `05-laser.md:215` where they name an example path, `11-glossary.md` (Block and Queue entries).
- Modify other docs and comments that name the example lab's queues or the old layout (spec section 4: every test and doc line in the same commit as the move): `README.md:116` (`--queue configs/examples/queues/experiment.toml`), `:139`, `:143` (run from `configs/examples`: `exp run queues/experiment.toml`, `queues/experiment.sim-air.toml`); `docs/dev_setup.md:150` (`queues/blocks/*.toml`), `:160`, `:413`-`:420`, `:514`-`:518`, `:613`; `docs/simulator.md:446`-`:641` (every `experiment*.toml` of the example lab, in commands and prose, becomes `queues/experiment*.toml`; `:638`, the install's queue, becomes `queues/experiment.toml` in the install); `configs/examples/sim.toml:105` (comment); `tests/experiment/test_plan_parameters.cpp:155` (comment); `libs/setup/AGENTS.md:8` (shipped `queues/experiment.toml`; kept compressed, as that file is). `README.md:89` and `docs/dev_setup.md:857` (`elctl --install argus ... exp run experiment.toml`) stay: the by-name lookup of Task 2 finds the queue in `queues/`.
- Test: `tests/experiment/test_lab.cpp`, `tests/setup/test_profiles.cpp`, `tests/integration/test_install_e2e.cpp`

**Interfaces:**
- Consumes: `LabPaths::blocks()` (Task 1); Task 2's `elctl` lookup (keeps `ElctlSetupTest` line 54 and the release smoke command working against the new install layout).

- [x] **Step 1: Write the failing tests**

In `tests/experiment/test_lab.cpp` (on the scratch copy of `configs/examples`, before the move, so they fail for the right reason):
- `LabTest.BlocksAreReadFromQueuesBlocks`: `load_lab(paths())` has `blocks.contains("blank_pair")` and `problems.empty()` (after the move the example's block is only in `queues/blocks/`).
- `LabTest.AnOldBlocksDirectoryIsNotRead`: write `dir_/blocks/old_pair.toml` (copy of the example block with `name = "old_pair"`); `!lab.blocks.contains("old_pair")` and `lab.problems.empty()`.
- `LabTest.NoBlocksDirectoryIsNoProblem`: `fs::remove_all(dir_ / "queues" / "blocks")`; `lab.blocks.empty()`, `lab.problems.empty()`.

In `tests/setup/test_profiles.cpp`, in the rendered-files test at `:127`: list `"queues/experiment.toml"` and `"queues/blocks/blank_pair.toml"` in place of `"experiment.toml"`, and add `EXPECT_FALSE(fs::exists(root / "experiment.toml"))` and `EXPECT_FALSE(fs::exists(root / "blocks"))`. In `tests/integration/test_install_e2e.cpp:241` load `site_.root / "queues" / "experiment.toml"`.

- [x] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev --target pychron_experiment_tests pychron_setup_tests && build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='LabTest.*'; build/dev/tests/setup/pychron_setup_tests`
Expected: `AnOldBlocksDirectoryIsNotRead` FAIL (old dir read), profile test FAIL (root `experiment.toml` rendered).

- [x] **Step 3: Implement**

Loader reads `paths.blocks()` in place of `dir / "blocks"`, otherwise unchanged (sorted, `*.toml`, a block that does not load is a problem line). Header comment line becomes `<lab>/queues/*.toml  queue files (any name; the program looks and saves here first)` and `<lab>/queues/blocks/*.toml  reusable run sequences for the run factory (optional)`; member comment `// <lab>/queues/blocks/*.toml by block name`. Tooltip: `lab_.paths.blocks().string()`. Profile `to` lines. `git mv` the example files. Update every path listed under Files, then confirm nothing is left:

Run: `git grep -nE '(examples|root_?|dir_|site_\.root|sim\.dir)[^"]*"?/?[" ]*experiment(\.laser|\.sim-air)?\.toml|"blocks"|/blocks/' -- . ':!docs/superpowers'`
and `git grep -nE 'configs/examples/(experiment|blocks)|<lab>/blocks|exp (run|validate) experiment|--queue experiment' -- . ':!docs/superpowers'`
Expected: only lines that are about a queue the test wrote itself, the profile's `template =` lines, `test_setup_commands.cpp:54`, and the `--install` by-name commands (`README.md:89`, `docs/dev_setup.md:857`).

- [x] **Step 4: Build and run what the move reaches**

Run:
```
cmake --build --preset dev --target pychron_experiment_tests pychron_elctl_tests pychron_setup_tests pychron_integration_tests pychron_install_e2e_tests pychron_outage_tests
build/dev/tests/experiment/pychron_experiment_tests
build/dev/apps/elctl/tests/pychron_elctl_tests
build/dev/tests/setup/pychron_setup_tests
build/dev/tests/integration/pychron_integration_tests --gtest_filter='*LabSession*:*LabSim*'
build/dev/tests/integration/pychron_install_e2e_tests
build/dev/tests/integration/pychron_outage_tests
cmake --build --preset dev-ui
ctest --preset dev-ui
```
(`pychron_install_e2e_tests` exists only with persistence; check the binaries' paths under `build/dev` and the suite names in the integration sources before filtering.) `tests/ui/experiment_fixture.hpp` is shared by many UI test targets and its `sim.dir / "experiment*.toml"` paths change with the move, so this commit runs the whole `dev-ui` suite (which covers everything `dev-quick` runs) rather than two UI binaries. Expected: all PASS.

- [x] **Step 5: Static analysis**

Run: `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed`, and `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui` (run factory panel and UI tests changed). Expected: exit 0.

- [x] **Step 6: Commit**

```bash
git add -A libs/experiment apps/pychron-ui/src/run_factory_panel.cpp profiles/instrument-common configs/examples tests apps/elctl/tests docs/user README.md docs/dev_setup.md docs/simulator.md libs/setup/AGENTS.md
git commit -m "feat(experiment)!: queues and blocks live under <lab>/queues" -m "Blocks are read from <lab>/queues/blocks only. The instrument profile renders its queue and block there, and the example lab's queues and block move there.

BREAKING CHANGE: a lab's old <lab>/blocks/ directory is no longer read; move its files to <lab>/queues/blocks/ by hand."
```

---

### Task 4: UI dialogs start in `queues/`; `--queue` looked up in the lab

**Files:**
- Modify: `apps/pychron-ui/src/experiment_window.hpp` (hooks), `apps/pychron-ui/src/experiment_window.cpp:648-666` (`open_dialog`, `save_as_dialog`)
- Modify: `apps/pychron-ui/src/main_window.cpp:406-411` (`--queue` through `find_queue`)
- Modify: `apps/pychron-ui/src/main.cpp:22-24` (comment: `--queue` name looked up in `<lab>/queues/`, then `<lab>/`)
- Modify: `docs/user/04-experiments.md` (Open and Save start in `queues/`)
- Test: `tests/ui/test_experiment_window.cpp` (target `pychron_ui_test_experiment_window`)

**Interfaces:**
- Consumes: `LabPaths::queues()`, `find_queue` (Task 1); `bridge_.lab().paths`.
- Produces (in `ExperimentWindow`, beside the other dialog hooks):
  - `using PickFile = std::function<QString(const QString& start)>;`
  - `void set_pick_open_file(PickFile pick);` default `QFileDialog::getOpenFileName(this, tr("Open queue"), start, tr("Experiment queues (*.toml)"))`
  - `void set_pick_save_file(PickFile pick);` default `QFileDialog::getSaveFileName(this, tr("Save queue"), start, tr("Experiment queues (*.toml)"))`
  - `void save_as_dialog();` moves from private to public (tests call it; File > Save As already reaches it).

Start directory, as settled from spec section 3: Open starts in the directory of `last_queue` when the setting is set and that directory exists, else in `<lab>/queues/`. Save As of a queue that has a file starts at that file, as now. Save As of a new queue (no file yet, also reached by Save) creates `<lab>/queues/` if missing, then starts there; the user types the name.

- [x] **Step 1: Write the failing tests** (private slots of the existing test class, each with its own `QTemporaryDir` settings file as the layout tests use)

- `openStartsInTheLabsQueuesWithNoLastQueue`: hook records `start` and returns `QString()`; `open_dialog()`; `QCOMPARE(start, QString::fromStdString((sim.dir / "queues").string()))`.
- `openStartsWhereTheLastQueueWas`: save the sim queue to `sim.dir / "elsewhere" / "q.toml"`, `load_queue` it, then `open_dialog()`: start is `sim.dir / "elsewhere"`.
- `openFallsBackWhenTheLastQueuesDirectoryIsGone`: as above, then `fs::remove_all(sim.dir / "elsewhere")`; start is `sim.dir / "queues"`.
- `savingANewQueueMakesQueues`: `fs::remove_all(sim.dir / "queues")`, `new_queue()`; save hook records `start` and `QVERIFY(fs::is_directory(sim.dir / "queues"))` inside the hook, returns `(sim.dir/"queues"/"new.toml")`; `save_as_dialog()`; start is `sim.dir / "queues"`, `window.path() == sim.dir / "queues" / "new.toml"`, the file exists.
- `aQueueNamedAtStartIsFoundInTheLab`: `save_queue_file((sim.dir/"queues"/"t041-start.toml").string(), sim.queue())`; `main.set_experiment(&bridge, true, fs::path("t041-start.toml"), …)` as the test at `:969` does; open the experiment window; its `path()` is `sim.dir / "queues" / "t041-start.toml"`.

- [x] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev-ui --target pychron_ui_test_experiment_window && build/dev-ui/tests/ui/pychron_ui_test_experiment_window openStartsInTheLabsQueuesWithNoLastQueue openStartsWhereTheLastQueueWas openFallsBackWhenTheLastQueuesDirectoryIsGone savingANewQueueMakesQueues aQueueNamedAtStartIsFoundInTheLab`
Expected: compile error (hooks missing).

- [x] **Step 3: Implement**

Hooks stored as members, defaults set in the constructor. `open_dialog` computes the start as above and calls the open hook; `save_as_dialog` creates `bridge_.lab().paths.queues()` (`fs::create_directories` with `std::error_code`; on failure the dialog still opens, at the lab) only when `path_` is empty. `MainWindow::ensure_experiment_window` loads `lab::find_queue(*experiment_queue_, experiment_->lab().paths.dir)`. `last_queue` is written as today (on load only).

- [x] **Step 4: Run the tests to see them pass**

Run: same as Step 2, then the whole binary. Expected: all PASS.

- [x] **Step 5: Static analysis**

Run: `python3 tools/quality_check.py` and `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`. Expected: exit 0.

- [x] **Step 6: Commit**

```bash
git add apps/pychron-ui/src/experiment_window.hpp apps/pychron-ui/src/experiment_window.cpp apps/pychron-ui/src/main_window.cpp apps/pychron-ui/src/main.cpp tests/ui/test_experiment_window.cpp docs/user/04-experiments.md
git commit -m "feat(ui): queue dialogs start in the lab's queues directory"
```

---

### Task 5: File > Open Recent in the experiment window

**Files:**
- Create: `apps/pychron-ui/src/recent_queues.hpp`, `apps/pychron-ui/src/recent_queues.cpp` (add to the source list in `apps/pychron-ui/CMakeLists.txt`)
- Modify: `apps/pychron-ui/src/experiment_window.hpp`, `apps/pychron-ui/src/experiment_window.cpp` (menu, recording)
- Modify: `docs/user/04-experiments.md` (File > Open Recent)
- Test: `tests/ui/test_recent_queues.cpp` (new; target `pychron_ui_test_recent_queues`, picked up by the glob), `tests/ui/test_experiment_window.cpp`

**Interfaces:**
- Consumes: `ExperimentWindow::load_queue`, `save_as` (existing); `MenuHub::contribute(owner, Menu::File, actions, Scope::Window)`.
- Produces:
  - `class RecentQueues` (namespace `pychron::ui`), `explicit RecentQueues(QSettings& settings);`
  - `static constexpr int kMax = 10;` and key `"experiment_window/recent_queues"` (a `QStringList`)
  - `void add(const std::filesystem::path& queue);` stores `fs::weakly_canonical(fs::absolute(queue))`, first; drops its older entry; keeps `kMax`
  - `QStringList entries();` the stored list without paths that no longer exist, written back when one was dropped
  - `void clear();`
  - `QMenu* ExperimentWindow::recent_menu() const noexcept;` the File > Open Recent submenu (tests read it)

- [ ] **Step 1: Write the failing tests**

`test_recent_queues.cpp` (`QTEST_MAIN`, a `QSettings` on a `QTemporaryDir` ini file, queue files touched in that directory):
- `newestFirst`: add `a`, `b`; `entries() == {b, a}` (absolute).
- `aReopenedQueueMovesToTheTop`: add `a`, `b`, `a`; `entries() == {a, b}`.
- `keepsTen`: add 12 files; `entries().size() == 10`, first is the 12th, the first two are gone.
- `oneEntryForTwoSpellings`: add `dir/queues/x.toml` and `dir/queues/../queues/x.toml`; one entry.
- `aDeletedQueueIsLeftOutAndForgotten`: add `a`, `b`; remove `a` from disk; `entries() == {b}`; the setting itself now holds only `b`.
- `clearEmptiesIt`.

`test_experiment_window.cpp`:
- `openRecentListsOpenedAndSavedQueues`: load `queue_file(sim, "a.toml")`, `save_as` to `b.toml`; `recent_menu()` after `aboutToShow` lists `b`, `a`, a separator, "Clear Menu"; the submenu's action is enabled.
- `openRecentIsDisabledWhenEmpty`: fresh settings; `recent_menu()->menuAction()->isEnabled()` is false.
- `choosingAnEntryOpensIt`: load `a`, load `b`; trigger the `a` entry; `window.path()` ends with `a.toml`.
- `clearMenuEmptiesIt`: trigger "Clear Menu"; menu disabled; setting empty.
- `anEntryAsksAboutUnsavedEdits`: modify the queue, `set_ask_unsaved` returns `Cancel`, trigger an entry; path unchanged (same rule as Open, through `load_queue`).

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset dev-ui --target pychron_ui_test_recent_queues pychron_ui_test_experiment_window`
Expected: compile error (`recent_queues.hpp` missing, `recent_menu` not declared).

- [ ] **Step 3: Implement `RecentQueues`, then the menu**

`ExperimentWindow` owns a `RecentQueues` on `*settings_`. `load_queue` and `save_as` call `add(path)` on success. A `QAction` "Open &Recent" with a `QMenu` (`recent_menu()`) is contributed to `MenuHub::Menu::File` with `Scope::Window` beside the window's other contributions (`experiment_window.cpp:388`). The menu is rebuilt from `entries()` on `aboutToShow` and after each `add`/`clear`, so the enabled state is right before it is shown: one action per entry (text: file name, tooltip and data: full path), a separator, "Clear Menu". An entry while a queue runs says so, as `open_dialog` does; otherwise `load_queue`, warning on error other than "cancelled", as `open_dialog` does.

- [ ] **Step 4: Run the tests to see them pass**

Run: `build/dev-ui/tests/ui/pychron_ui_test_recent_queues` and `build/dev-ui/tests/ui/pychron_ui_test_experiment_window`, and `build/dev-ui/tests/ui/pychron_ui_test_menu_hub` (File menu contents are pinned there: the contribution comes after the four commands, which `file_has_the_four_commands_once` checks with `mid(0, 4)`). Expected: all PASS.

- [ ] **Step 5: Static analysis**

Run: `python3 tools/quality_check.py` and `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`. Expected: exit 0.

- [ ] **Step 6: Commit**

```bash
git add apps/pychron-ui/src/recent_queues.hpp apps/pychron-ui/src/recent_queues.cpp apps/pychron-ui/CMakeLists.txt apps/pychron-ui/src/experiment_window.hpp apps/pychron-ui/src/experiment_window.cpp tests/ui/test_recent_queues.cpp tests/ui/test_experiment_window.cpp docs/user/04-experiments.md
git commit -m "feat(ui): File > Open Recent for queues"
```

---

### Task 6: Whole-tree checks

No new code. Run once, after Tasks 1 to 5 are committed.

- [ ] **Step 1: Full build and tests**

Run: `cmake --build --preset dev-ui` then `ctest --preset dev-ui` (covers everything `dev` runs, and the UI tests). Expected: 100% tests passed. A failure is fixed at its cause in the task it belongs to, never skipped.

- [ ] **Step 2: Whole-tree sweep and static analysis**

Run: `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui`. Expected: exit 0, no warnings.

- [ ] **Step 3: Nothing left at the old paths**

Run: `git grep -nE 'configs/examples/(experiment|blocks)|<lab>/blocks' -- . ':!docs/superpowers'`
Expected: no output.

- [ ] **Step 4: Commit** only if Steps 1 to 3 needed a fix, with the scope of what was fixed.

## Open questions

None.
