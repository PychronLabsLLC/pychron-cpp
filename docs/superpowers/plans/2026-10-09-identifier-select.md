# Identifier Select Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The run factory panel's Identifier field offers the store's identifiers in a dropdown, narrowed by optional Package and Level selects, while typing keeps working.

**Architecture:** `RunFactoryPanel` talks to an abstract, asynchronous `IdentifierSource` (a `QObject` with two calls and a `changed` signal) that it holds by `QPointer` and never owns. The store-backed implementation, `StoreIdentifierSource`, runs existing `IStore` reads on the `EntryBridge` worker and is built only with `PYCHRON_UI_HAS_STORE`. Level narrowing is done in the panel on what one `contents()` call returned.

**Tech Stack:** C++20, Qt 6 Widgets, Qt Test (`tests/ui`, one `QTEST_MAIN` executable per `test_*.cpp`), `pychron::Result<T>`.

**Spec:** `docs/superpowers/specs/2026-10-09-identifier-select-design.md`

## Global Constraints

- Branch `feat/identifier-select`, cut from `develop`. Commits follow Conventional Commits with scope `ui`.
- `identifier_source.hpp` and `run_factory_panel.*` include no persistence, entry or store header: they build with `-DPYCHRON_PERSISTENCE=OFF`. No `#ifdef PYCHRON_UI_HAS_STORE` in the panel.
- No new store query, index or migration. The source and the panel write nothing to the store.
- The GUI thread never waits on the store: no `run_sync` outside tests.
- The identifier line edit keeps object name `identifier`; the existing tests in `tests/ui/test_run_factory_panel.cpp` pass unchanged.
- Object names: Package combo `package`, Level combo `level`, Identifier combo `identifier_select`.
- First items, exact text: Package `(none)`, Level `(all)`.
- Item text: `<identifier>  <sample>  (<level> <position>)` with two spaces between parts; with an empty sample `<identifier>  (<level> <position>)`. Item data (`Qt::UserRole`) is the identifier alone.
- Nothing modal on a failed read: the error text goes in a tooltip.
- Errors are values (`pychron::Result<T>`); no exceptions.
- After changing C++ and before the tests: `python3 tools/quality_check.py` (exit 0). Build directory: `build/dev-ui`.
- No thread is added, so no TSan run is owed.

## Review Focus

1. Two packages with the same name: the panel must key the chosen package by `PackageChoice::id`, so choosing the second lists the second's contents. (Task 1)
2. A package with no levels, or levels with no identifiers: Level shows only `(all)`, the dropdown is empty, no tooltip error. (Task 1)
3. `changed()` arriving while a `contents()` answer is still pending: the pending answer is stale and dropped; the reload's answer is the one shown. (Task 2)
4. An answer arriving while the panel is locked: the lists fill but the three widgets stay disabled; unlocking enables them (Level only when a package is chosen). (Task 2)
5. `set_identifier_source(nullptr)` after a source was set, and a second non-null source: rows hidden and lists cleared in the first case; in the second the old source's late answers and `changed()` are ignored. (Task 2)

---

### Task 1: The source interface and the selects

**Files:**
- Create: `apps/pychron-ui/src/identifier_source.hpp`
- Modify: `apps/pychron-ui/src/run_factory_panel.hpp`, `apps/pychron-ui/src/run_factory_panel.cpp` (`build_run()` at about line 102, `set_form()`, `set_locked()`), `apps/pychron-ui/CMakeLists.txt` (add `src/identifier_source.hpp` beside `src/run_factory_panel.hpp` in the unconditional header list, so AUTOMOC sees its `Q_OBJECT`)
- Test: `tests/ui/test_run_factory_panel.cpp`

**Interfaces:**
- Produces, in `identifier_source.hpp`, exactly the declarations of spec section 3: `PackageChoice{id, name}`, `IdentifierChoice{identifier, sample, level, position}`, `PackageContents{levels, choices}`, and `class IdentifierSource : public QObject` with pure virtual `packages(QObject* receiver, std::function<void(Result<std::vector<PackageChoice>>)> done)`, `contents(QObject* receiver, const std::string& package, std::function<void(Result<PackageContents>)> done)` and signal `changed()`.
- Produces, on `RunFactoryPanel`:

```cpp
void set_identifier_source(IdentifierSource* source);  // null: no selects
// For tests: what the operator's pick does.
void choose_package(int index);     // 0 is "(none)"
void choose_level(int index);       // 0 is "(all)"
void choose_identifier(int index);  // a row of the dropdown
QStringList package_choices() const;
QStringList level_choices() const;
QStringList identifier_choices() const;  // item texts
bool selects_visible() const;            // the Package and Level rows
QString identifier_tooltip() const;
QString package_tooltip() const;
```

- [ ] **Step 1: Write the fake and the failing tests**

In `tests/ui/test_run_factory_panel.cpp`, above the test class, a `FakeIdentifierSource : public pychron::ui::IdentifierSource` without `Q_OBJECT` (it adds no signal or slot). It records each call and answers nothing until told:

```cpp
struct FakeIdentifierSource : pychron::ui::IdentifierSource {
  std::vector<std::function<void(pychron::Result<std::vector<pychron::ui::PackageChoice>>)>> package_calls;
  std::vector<std::pair<std::string, std::function<void(pychron::Result<pychron::ui::PackageContents>)>>> content_calls;
  void packages(QObject*, std::function<void(pychron::Result<std::vector<pychron::ui::PackageChoice>>)> done) override;
  void contents(QObject*, const std::string& package,
                std::function<void(pychron::Result<pychron::ui::PackageContents>)> done) override;
  void notify() { Q_EMIT changed(); }
};
```

Shared data for the tests: packages `{"p2","NM-294"}`, `{"p1","NM-293"}`; contents of `p1`: levels `A`, `B`, `C`; choices `{"66001","FC-2","A",1}`, `{"66002","","A",3}`, `{"66010","bt-1","B",2}` (level `C` has none).

Tests (new slots; each calls `panel_->set_identifier_source(&fake)` itself unless it says otherwise):

- `noSourceLeavesThePanelAsItWas`: `selects_visible()` false; `identifier_choices()` empty; `findChild<QLineEdit*>("identifier")` non-null.
- `packagesAreListedAfterNone`: before the answer `selects_visible()` false; after `package_calls[0](packages)`: `selects_visible()` true, `package_choices() == {"(none)","NM-294","NM-293"}`, `identifier_choices()` empty, `findChild<QComboBox*>("level")->isEnabled()` false, `content_calls` empty.
- `aPackageListsEveryLevelsIdentifiers`: `choose_package(2)`; `content_calls.size() == 1` and `.first == "p1"`; answer it; `level_choices() == {"(all)","A","B","C"}`; `identifier_choices() == {"66001  FC-2  (A 1)", "66002  (A 3)", "66010  bt-1  (B 2)"}`; Level enabled.
- `aLevelNarrowsWithoutAskingAgain`: after the above `choose_level(1)`: choices are the two of `A`; `choose_level(3)` (`C`): empty; `choose_level(0)`: all three; `content_calls.size()` still 1.
- `pickingAnItemIsTypingItsIdentifier`: `choose_identifier(2)`; `form().identifier == "66010"`; the line edit's text is `66010` (not the item text); `preview_text()` no longer contains `identifier`.
- `aTypedSpecialStillSwitchesType`: with `p1` and level `A` chosen, `QTest::keyClicks` of `a` into the cleared line edit: `form().identifier == "a"`, the type combo reads the lab's type for `a` (as the existing test at about line 147 asserts), Package index still 2 and Level index still 1.
- `setFormLeavesTheSelects`: with `p1`/`A` chosen, `set_form(with_identifier("20001","1"))`: line edit `20001`, `package_choices()`, Package and Level indexes and `identifier_choices()` unchanged.
- `choosingAnotherPackageResetsLevel`: `p1`/`B` chosen, then `choose_package(1)`: `content_calls.back().first == "p2"`, Level index 0 and `level_choices() == {"(all)"}`, dropdown empty until answered. `choose_package(0)`: Level disabled, dropdown empty, no new call.
- `packagesOfOneNameAreToldApartById` (Review Focus 1): packages `{"x","Dup"}`, `{"y","Dup"}`; `choose_package(2)` asks for `"y"`.
- `anEmptyPackageIsNotAnError` (Review Focus 2): answer `PackageContents{}`: `level_choices() == {"(all)"}`, dropdown empty, `identifier_tooltip()` empty.
- `lockedDisablesTheSelects`: `set_locked(true)`: the three combos disabled; `set_locked(false)` with no package: Package and Identifier enabled, Level disabled.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build/dev-ui --target pychron_ui_test_run_factory_panel`
Expected: compile error, `identifier_source.hpp` not found.

- [ ] **Step 3: Write `identifier_source.hpp`**

The declarations of spec section 3, `#pragma once`, includes `<functional>`, `<string>`, `<vector>`, `<QObject>`, `"pychron/core/error.hpp"`. Add it to `apps/pychron-ui/CMakeLists.txt`.

- [ ] **Step 4: Implement the selects in `RunFactoryPanel`**

- `identifier_` stays a `QLineEdit*`: it is now `identifier_select_->lineEdit()`, given object name `identifier` and the placeholder after `setEditable(true)`. New members `QComboBox* package_`, `level_`, `identifier_select_`, `QPointer<IdentifierSource> source_`, `PackageContents contents_`, `std::vector<PackageChoice> packages_`. Form rows in the order Type, Package, Level, Identifier, Aliquot, Step; hide and show the two rows with `QFormLayout::setRowVisible`.
- `identifier_select_`: `setInsertPolicy(QComboBox::NoInsert)`, `setCompleter(nullptr)`. On `QComboBox::activated(int)`: set the line edit's text to the item's `Qt::UserRole` data, then `on_identifier_changed()`. `choose_identifier(i)` does the same after `setCurrentIndex(i)`.
- Rebuilding the dropdown (new contents, level chosen) must leave the line edit's text as it was: block the combo's signals, clear and fill, `setCurrentIndex(-1)`, restore the text.
- The existing `connect(identifier_, &QLineEdit::textEdited, ...)` stays as it is.
- The package and contents requests each pass `this` as receiver. Staleness, `changed()` and error handling are Task 2: here an answer is applied as it arrives, and an error leaves the lists empty.
- `set_locked()` also disables the three combos; Level's enabled state is `!locked_ && a package is chosen`.

- [ ] **Step 5: Run the tests**

Run: `cmake --build build/dev-ui --target pychron_ui_test_run_factory_panel && ctest --test-dir build/dev-ui -R ui.test_run_factory_panel --output-on-failure`
Expected: PASS, the old slots included.

- [ ] **Step 6: Static analysis and commit**

Run: `python3 tools/quality_check.py` (exit 0), then

```bash
git add apps/pychron-ui/src/identifier_source.hpp apps/pychron-ui/src/run_factory_panel.hpp apps/pychron-ui/src/run_factory_panel.cpp apps/pychron-ui/CMakeLists.txt tests/ui/test_run_factory_panel.cpp
git commit -m "feat(ui): the run factory picks an identifier from a package and level"
```

---

### Task 2: Stale answers, catalog changes, errors and a source that goes away

**Files:**
- Modify: `apps/pychron-ui/src/run_factory_panel.hpp`, `apps/pychron-ui/src/run_factory_panel.cpp`
- Test: `tests/ui/test_run_factory_panel.cpp`

**Interfaces:**
- Consumes: Task 1's `FakeIdentifierSource`, `choose_*`, `*_choices()`, `selects_visible()`, `identifier_tooltip()`, `package_tooltip()`.
- Produces: no new public names.

- [ ] **Step 1: Write the failing tests**

- `aLateAnswerForAnotherPackageIsDropped`: `choose_package(2)` (`p1`), `choose_package(1)` (`p2`); answer `p2`'s call with one choice `{"70001","","A",1}`, then `p1`'s call with the shared contents: `identifier_choices() == {"70001  (A 1)"}`.
- `aCatalogChangeReloadsAndKeepsTheChoice`: `p1`/`B` chosen, line edit holds `66010`; `fake.notify()`: a new packages call; answer it with the same packages: a new contents call for `p1`; answer it with level `B` now holding `{"66011","","B",4}` as well: Package index 2, Level text `B`, dropdown has `B`'s two, line edit still `66010`.
- `aCatalogChangeDropsAChoiceThatIsGone`: as above but the packages answer lacks `p1`: Package index 0, Level disabled, dropdown empty, no contents call made. Second case: `p1` kept, contents answer has levels `A` only: Level index 0.
- `aChangeWhileContentsArePendingWins` (Review Focus 3): `choose_package(2)`, do not answer; `notify()`; answer packages; answer the new contents call with one choice; then answer the first contents call with the shared contents: the dropdown holds the one choice.
- `aFirstPackagesFailureKeepsTheRowsHidden`: answer `packages` with an error: `selects_visible()` false.
- `aLaterPackagesFailureKeepsTheList`: after a good load, `notify()`, answer with error `"store gone"`: `package_choices()` unchanged, `package_tooltip()` contains `store gone`; `notify()` and a good answer clears the tooltip.
- `aContentsFailureEmptiesTheListAndSaysWhy`: answer contents with error `"no such level"`: `level_choices() == {"(all)"}`, dropdown empty, `identifier_tooltip()` contains `no such level`; choosing the package again and a good answer clears it.
- `anAnswerWhileLockedFillsButStaysDisabled` (Review Focus 4): `choose_package(2)`, `set_locked(true)`, answer: `identifier_choices()` has three, all three combos disabled; `set_locked(false)`: all three enabled.
- `aSourceThatGoesAwayHidesTheSelects`: source on the heap, a package chosen and answered; `delete` it: `selects_visible()` false, `identifier_choices()` empty, the line edit's text unchanged, `panel_->set_form(panel_->form())` does not crash.
- `aSourceCanBeReplacedOrCleared` (Review Focus 5): fake `a` set and loaded with a package chosen; `set_identifier_source(nullptr)`: rows hidden, lists cleared. Then `set_identifier_source(&b)`: a packages call on `b`; `a.notify()` makes no call on `a` or `b`; an unanswered call of `a` answered now changes nothing.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build/dev-ui --target pychron_ui_test_run_factory_panel && ctest --test-dir build/dev-ui -R ui.test_run_factory_panel --output-on-failure`
Expected: the new slots FAIL (the stale answer is shown, no reload on `notify()`), the Task 1 slots pass.

- [ ] **Step 3: Implement**

- Two counters, `std::uint64_t packages_serial_` and `contents_serial_`; each request captures the value it incremented to and its answer is dropped unless it equals the current one. `set_identifier_source()` and choosing `(none)` bump both, so anything outstanding is dropped.
- `set_identifier_source()`: disconnect the old source, clear lists and tooltips, hide the rows; for a non-null source connect `changed` and `QObject::destroyed` (the latter does what `set_identifier_source(nullptr)` does) and ask `packages()`.
- On `changed()`: ask `packages()`. On its answer: refill, re-select the package whose `id` equals the one chosen before (else index 0 and clear), and when one is still chosen ask `contents()` again, remembering the chosen level name to re-select when the answer lists it.
- Errors per spec section 5: tooltip text is `QString::fromStdString(to_string(error))`.

- [ ] **Step 4: Run the tests**

Same command as Step 2. Expected: PASS.

- [ ] **Step 5: Static analysis and commit**

Run: `python3 tools/quality_check.py` (exit 0), then

```bash
git add apps/pychron-ui/src/run_factory_panel.hpp apps/pychron-ui/src/run_factory_panel.cpp tests/ui/test_run_factory_panel.cpp
git commit -m "feat(ui): the identifier select follows the catalog and drops stale answers"
```

---

### Task 3: The store-backed source

**Files:**
- Create: `apps/pychron-ui/src/store_identifier_source.hpp`, `apps/pychron-ui/src/store_identifier_source.cpp`
- Modify: `apps/pychron-ui/src/entry_actions.hpp`, `apps/pychron-ui/src/entry_actions.cpp:64` (`ensure_bridge`), `apps/pychron-ui/CMakeLists.txt` (both new files in the `if(TARGET pychron_processing_store)` source list)
- Test: `tests/ui/test_entry_windows.cpp`

**Interfaces:**
- Consumes: `IdentifierSource` and its value types (Task 1); `EntryBridge::run<R>(receiver, fn, done)` and `EntryBridge::changed`; `IStore::irradiations()`, `levels(Uuid)`, `level_sheet(Uuid)`.
- Produces:

```cpp
// entry_actions.hpp
// Opens the bridge on first use and shows nothing; the error when the store cannot be opened.
Result<void> open_bridge();
bool ensure_bridge();  // unchanged for callers: open_bridge(), and the message box on failure

// store_identifier_source.hpp
class StoreIdentifierSource : public IdentifierSource {
  Q_OBJECT
 public:
  explicit StoreIdentifierSource(EntryActions& entry);  // child of `entry`
  void packages(QObject* receiver, std::function<void(Result<std::vector<PackageChoice>>)> done) override;
  void contents(QObject* receiver, const std::string& package,
                std::function<void(Result<PackageContents>)> done) override;
};
```

- [ ] **Step 1: Write the failing tests**

In `EntryWindowsTest`. Give the seeded package identifiers the way `packages_assign_save_and_generate` does (assign `FC-2` to level `A` positions 1 and 3 and `bt-1` to level `B` position 2, save, generate identifiers), or write a helper next to `seed()` that does it directly on `store_`; read the identifiers back from `store_->level_sheet()` so the test does not assume their values. An `EntryActions entry(&window, url_)` on a `QMainWindow` gives the bridge.

- `identifier_source_lists_packages`: `packages()`, then `QTRY_VERIFY` on the answer: one `PackageChoice`, `name == "P-1"`, `id == to_string(seeded_.package)`.
- `identifier_source_lists_a_packages_levels_and_identifiers`: `contents(id)`: `levels == {"A","B"}`; `choices` has three entries in the order (A,1), (A,3), (B,2) with those identifiers and samples `FC-2`, `FC-2`, `bt-1`; the positions without identifier (A 0, A 2, B 0, B 1, B 3) are absent.
- `identifier_source_refuses_a_bad_package_id`: `contents("not-a-uuid")` answers an error.
- `identifier_source_passes_on_changes`: `QSignalSpy` on the source's `changed`; after a first `packages()` answer, `entry.bridge()->notify_changed()`: spy count 1.
- `identifier_source_reports_a_store_it_cannot_open`: `EntryActions` on `sqlite:/nonexistent-dir/x.db`; `packages()` answers an error; `QApplication::activeModalWidget()` is null throughout; `entry.bridge()` is null.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build/dev-ui --target pychron_ui_test_entry_windows`
Expected: compile error, `store_identifier_source.hpp` not found.

- [ ] **Step 3: Implement**

- `EntryActions::open_bridge()` holds what `ensure_bridge()` did without the box; `ensure_bridge()` calls it and shows the existing `QMessageBox::critical` text with the error.
- `StoreIdentifierSource`: each call starts with `entry_.open_bridge()`; on failure `done` is called with the error through `QMetaObject::invokeMethod(receiver, ..., Qt::QueuedConnection)` so that it is never called before the call returns. The first successful open connects `EntryBridge::changed` to `changed` (once).
- `contents()` is one `EntryBridge::run` job: parse the uuid (error without a store call when it does not parse), `levels(uuid)`, then `level_sheet(level.uuid)` per level in the order returned; a missing sheet (`nullopt`: the level went away meanwhile) is skipped, a failed read fails the job. Mapping per spec section 4.

- [ ] **Step 4: Run the tests**

Run: `cmake --build build/dev-ui --target pychron_ui_test_entry_windows && ctest --test-dir build/dev-ui -R ui.test_entry_windows --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Static analysis and commit**

Run: `python3 tools/quality_check.py` (exit 0), then

```bash
git add apps/pychron-ui/src/store_identifier_source.hpp apps/pychron-ui/src/store_identifier_source.cpp apps/pychron-ui/src/entry_actions.hpp apps/pychron-ui/src/entry_actions.cpp apps/pychron-ui/CMakeLists.txt tests/ui/test_entry_windows.cpp
git commit -m "feat(ui): identifiers of a package read from the store for the run factory"
```

---

### Task 4: Wiring, measurement and docs

**Files:**
- Modify: `apps/pychron-ui/src/main_window.hpp`, `apps/pychron-ui/src/main_window.cpp:348` (`ensure_experiment_window`), `apps/pychron-ui/src/main.cpp:498` (beside `new EntryActions`, inside `#ifdef PYCHRON_UI_HAS_STORE` and `if (cli->db)`)
- Modify: `docs/user/04-experiments.md`, `docs/superpowers/specs/2026-10-02-experiment-window-design.md` (section 5.6, the "Run:" bullet)
- Test: `tests/ui/test_experiment_window.cpp`

**Interfaces:**
- Consumes: `RunFactoryPanel::set_identifier_source`, `ExperimentWindow::factory()`, `StoreIdentifierSource(EntryActions&)`.
- Produces: `void MainWindow::set_identifier_source(IdentifierSource* source);` It keeps a `QPointer<IdentifierSource>` and gives it to `experiment_window_->factory()` at once when the window exists, and in `ensure_experiment_window()` when it is made.

- [ ] **Step 1: Write the failing test**

In `tests/ui/test_experiment_window.cpp` (or the main-window test that already reaches `ensure_experiment_window()`; use whichever fixture constructs a `MainWindow` with an experiment session), with a fake source like Task 1's that answers at once with one package:

- `theMainWindowHandsTheSourceToTheFactory`: `set_identifier_source(&fake)` before the experiment window is opened; open it; `factory()->package_choices().size() == 2`.
- `aSourceSetLaterReachesAnOpenWindow`: open the window first, then set the source: same assertion.

- [ ] **Step 2: Run it to see it fail**

Expected: compile error, `MainWindow` has no `set_identifier_source`.

- [ ] **Step 3: Implement `MainWindow::set_identifier_source` and the `main.cpp` line**

`window.set_identifier_source(new pychron::ui::StoreIdentifierSource(*entry));` Only the second `EntryActions` site (line 498, the one with a line and an experiment session) gets it; the data-only window at line 207 has no experiment window.

- [ ] **Step 4: Run the UI tests**

Run: `cmake --build build/dev-ui && ctest --test-dir build/dev-ui -R '^ui\.' --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Build without persistence**

Run: `cmake -S . -B build/nopersist --preset dev-ui -DPYCHRON_PERSISTENCE=OFF && cmake --build build/nopersist --target pychron_ui_test_run_factory_panel && ctest --test-dir build/nopersist -R ui.test_run_factory_panel --output-on-failure`
Expected: builds and passes (the panel and the interface need no store).

- [ ] **Step 6: Measure on a store of real size (spec section 6)**

On a copy of a lab's store, time `irradiations()` and `contents()` of its largest package (a throwaway `QElapsedTimer` around the two `EntryBridge::run` jobs, or `sqlite3 .timer on` with the statements of `kIrradiations`, `kLevels` and the level sheet). Put both numbers in the commit message. If the package list takes more than 1 s, stop and report it instead of committing: the fix is a separate change.

- [ ] **Step 7: Docs**

- `docs/user/04-experiments.md`, run factory section: Package and Level selects, the Identifier dropdown and its item text, that picking equals typing, that the selects appear only when `pychron-ui` was started with `--db`, and that nothing is remembered between sessions.
- Experiment window spec 5.6, "Run:" bullet: name the two selects and the dropdown and point to `2026-10-09-identifier-select-design.md`.

- [ ] **Step 8: Static analysis and commit**

Run: `python3 tools/quality_check.py` (exit 0), then

```bash
git add apps/pychron-ui/src/main_window.hpp apps/pychron-ui/src/main_window.cpp apps/pychron-ui/src/main.cpp tests/ui/test_experiment_window.cpp docs/user/04-experiments.md docs/superpowers/specs/2026-10-02-experiment-window-design.md
git commit -m "feat(ui): the experiment window's identifier select reads the store given with --db"
```

Then land as AGENTS.md says: rebase on `origin/develop`, `python3 tools/quality_check.py`, the full `ctest` on the rebased branch, merge into `develop`, push.
