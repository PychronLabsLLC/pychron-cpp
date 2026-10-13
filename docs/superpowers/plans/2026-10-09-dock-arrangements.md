# Dock Arrangements and Layout Reset Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the operator reshow a closed panel, reset a docked window to its factory layout, and save, apply and delete named layouts, in the Extraction Line, Experiment and Spectrometer windows.

**Architecture:** One helper, `DockLayouts`, is a child `QObject` of each docked `QMainWindow`. It is given a function that rebuilds the window's factory layout and the QSettings group the window already uses. `MenuHub` owns one set of Window-menu actions (Panels, Arrangements, Reset Layout) that act on the helper of the window in front.

**Tech Stack:** C++20, Qt 6 Widgets (`QMainWindow::saveState` / `restoreState`, `QSettings`), QtTest.

**Spec:** `docs/superpowers/specs/2026-10-09-dock-arrangements-design.md`. Read it first; section numbers below refer to it.

## Global Constraints

- Errors are values: `pychron::Result<T>` (`libs/core` `error.hpp`). `ErrorKind` has no "invalid argument"; this feature uses `ErrorKind::Config` with `Error::code` one of `"bad_name"`, `"no_settings"`, `"bad_layout"`, `"unknown_name"`, and `ErrorKind::Io` with code `"not_saved"`.
- Arrangement name: after trim, length 1..64, no `/`, no `\`, no control character.
- `saveState()` / `restoreState()` version argument stays `0`.
- Existing settings keys are kept: `experiment_window/state`, `experiment_window/geometry`, `spectrometer_window/<bridge name>/dock_state`, `spectrometer_window/<bridge name>/geometry`.
- New keys: `main_window/<line system name>/state|geometry`, and under each window's group `arrangements/<name>/state` and `arrangements/<name>/geometry`.
- Reset changes dock layout only: never window geometry, never stored arrangements, never another window.
- Qt code: a parent owns its child (`new` with a parent is allowed). `const` by default, `[[nodiscard]]` on results.
- No threads are added; nothing here waits on a clock.
- Commits: Conventional Commits with component scope (`feat(ui): ...`). Plans, code, comments and commit messages in normal prose.
- Before each commit that changes C++: `python3 tools/quality_check.py` (exit 0), then the tests named in the task.
- Build and run (UI tests are picked up by glob; re-run configure after adding a `test_*.cpp`):

  ```bash
  cmake --preset dev-ui
  cmake --build --preset dev-ui
  ctest --preset dev-ui -R '^ui\.'
  ```

## File Structure

| File | Responsibility |
|---|---|
| `apps/pychron-ui/src/dock_layouts.hpp`, `.cpp` (new) | The helper: factory reset, last layout, named arrangements, panel actions, name validation. Knows nothing of menus. |
| `apps/pychron-ui/src/menu_hub.hpp`, `.cpp` | Window-menu actions and the save flow; finds the front window's helper. |
| `apps/pychron-ui/src/main_window.hpp`, `.cpp` | `default_layout()`, optional settings, helper, save on close. |
| `apps/pychron-ui/src/experiment_window.hpp`, `.cpp` | `default_layout()`, helper in place of its own restore/save lines. |
| `apps/pychron-ui/src/spectrometer_window.hpp`, `.cpp` | Same. |
| `apps/pychron-ui/src/main.cpp` | Passes `std::make_unique<QSettings>()` to `MainWindow`. |
| `apps/pychron-ui/CMakeLists.txt` | Adds `src/dock_layouts.cpp`. |
| `tests/ui/test_dock_layouts.cpp` (new) | Helper tests on a bare `QMainWindow`. |
| `tests/ui/test_menu_hub.cpp`, `test_docks.cpp`, `test_experiment_window.cpp`, `test_spectrometer_window.cpp` | Hub and per-window cases. |
| `docs/user/02-extraction-line.md`, `03-spectrometer.md`, `04-experiments.md`, `10-troubleshooting.md` | User docs. |

## Review Focus

Conditions the spec implies but does not list as tests. Each has a test in the task named.

1. **A name the ini format mangles** (`Ünï 100% [a]=b`, a name of dots): saved and applied under exactly the name typed, and listed once. Task 2.
2. **Settings that cannot be written** (read-only file, full disk): `save_as` reports failure instead of showing an arrangement that is gone at next start. Task 2.
3. **Reset while docks float together or are hidden and floating at once:** every dock ends docked and shown, none left as an empty floating frame. Task 1.
4. **Apply to a maximized window:** the arrangement's panels appear; the window does not end half off screen or with a stale normal size. Task 2.
5. **A layout command with no usable front window** (front window is a dialog, the command palette popup, or nothing): the command acts on the window under the popup or does nothing; it never crashes or acts on a hidden window. Task 5.

---

### Task 1: `DockLayouts`: factory reset, last layout, panel actions

**Files:**
- Create: `apps/pychron-ui/src/dock_layouts.hpp`, `apps/pychron-ui/src/dock_layouts.cpp`
- Modify: `apps/pychron-ui/CMakeLists.txt` (add `src/dock_layouts.cpp` beside `src/log_dock.cpp`)
- Test: `tests/ui/test_dock_layouts.cpp` (new; `QTEST_MAIN(TestDockLayouts)`, camelCase slots as in `test_docks.cpp`)

**Interfaces:**
- Consumes: `pychron::Result`, `pychron::Error` from `pychron/core/error.hpp`.
- Produces (namespace `pychron::ui`):

  ```cpp
  class DockLayouts : public QObject {
    Q_OBJECT
   public:
    struct Keys {
      QString state = QStringLiteral("state");
      QString geometry = QStringLiteral("geometry");
    };
    DockLayouts(QMainWindow* window, std::function<void()> factory, QSettings* settings, QString group, Keys keys = {});
    // The helper of `window`, or nullptr.
    static DockLayouts* of(const QWidget* window);

    void reset();
    void restore_last();
    void save_last();
    [[nodiscard]] bool can_save() const;
    [[nodiscard]] QList<QAction*> panel_actions() const;
    QMainWindow* window() const;
  };
  ```

  `of()` is `window->findChild<DockLayouts*>(QString(), Qt::FindDirectChildrenOnly)`, null-safe. The constructor does not call `factory` (the window already has); it `Q_ASSERT`s that every direct-child `QDockWidget` has a non-empty `objectName` (rule 9).

Test fixture, shared by Tasks 1 and 2: a function `make_window(QSettings* settings)` returning a struct with a `QMainWindow`, three docks named `A` (left), `B` (right), `C` (right, tabified with `B`, not closable), a `default_layout` lambda that does `setFloating(false)`, `addDockWidget`, `show()`, `tabifyDockWidget(B, C)`, and the `DockLayouts` on group `"win"`. Settings: `QSettings(dir.filePath("s.ini"), QSettings::IniFormat)` in a `QTemporaryDir`. Show the window and `QVERIFY(QTest::qWaitForWindowExposed(&w))` before asserting visibility.

- [ ] **Step 1: Write the failing tests**

  ```cpp
  void resetRestoresFactoryAfterEverythingMoved();
  //  A->close(); B->setFloating(true); w.addDockWidget(Qt::BottomDockWidgetArea, C); layouts.reset();
  //  for each dock: QVERIFY(d->isVisible()); QVERIFY(!d->isFloating());
  //  QCOMPARE(w.dockWidgetArea(A), Qt::LeftDockWidgetArea);
  //  QCOMPARE(w.dockWidgetArea(B), Qt::RightDockWidgetArea);
  //  QVERIFY(w.tabifiedDockWidgets(B).contains(C));

  void resetDocksPanelsThatFloatTogetherOrAreHiddenAndFloating();   // Review Focus 3
  //  A->setFloating(true); B->setFloating(true); B->hide(); layouts.reset();
  //  same assertions as above; QCOMPARE(w.findChildren<QDockWidget*>().size(), 3);

  void lastLayoutSurvivesASecondWindow();
  //  window 1: A->close(); B->setFloating(true); layouts.save_last();
  //  window 2 on the same file: layouts.restore_last();
  //  QVERIFY(!A2->isVisible()); QVERIFY(B2->isFloating());

  void garbageLastLayoutGivesFactoryWithoutError();
  //  settings.setValue("win/state", QByteArray("not a layout")); restore_last();
  //  factory assertions hold.

  void customKeysAreUsed();
  //  Keys{"dock_state", "geometry"}; save_last(); QVERIFY(settings.contains("win/dock_state"));
  //  QVERIFY(!settings.contains("win/state"));

  void withoutSettingsNothingIsWritten();
  //  DockLayouts with settings == nullptr: QVERIFY(!can_save()); save_last(); restore_last(); reset() works;
  //  QVERIFY(QDir(dir.path()).isEmpty());

  void panelActionsAreTheClosableDocks();
  //  QCOMPARE(texts(panel_actions()), QStringList({"A", "B"}));   // C is not closable
  //  panel_actions()[0]->trigger(); QVERIFY(!A->isVisible()); trigger again; QVERIFY(A->isVisible());

  void ofFindsTheHelperOrNull();
  //  QCOMPARE(DockLayouts::of(&w), &layouts); QMainWindow bare; QVERIFY(DockLayouts::of(&bare) == nullptr);
  //  QVERIFY(DockLayouts::of(nullptr) == nullptr);
  ```

- [ ] **Step 2: Run to verify they fail**

  Run: `cmake --preset dev-ui && cmake --build --preset dev-ui --target pychron_ui_test_dock_layouts`
  Expected: compile error, `dock_layouts.hpp` not found.

- [ ] **Step 3: Implement the helper**

  - `reset()`: call `factory`. Nothing else.
  - `restore_last()`: no settings, or no state bytes: return. Otherwise `restoreGeometry` (when geometry bytes are present), then `restoreState(bytes, 0)`; if that returns false call `reset()`.
  - `save_last()`: no settings: return. Write `saveGeometry()` and `saveState(0)` under `<group>/<keys.geometry>` and `<group>/<keys.state>`.
  - Use full key paths (`group + '/' + key`), not `beginGroup`: the windows' own code opens and closes groups on the same `QSettings`.
  - `panel_actions()`: direct-child `QDockWidget`s (`findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly)`, which is creation order) whose `features()` include `DockWidgetClosable`; return each one's `toggleViewAction()`.

- [ ] **Step 4: Run to verify they pass**

  Run: `cmake --build --preset dev-ui --target pychron_ui_test_dock_layouts && ctest --preset dev-ui -R ui.test_dock_layouts`
  Expected: all pass.

- [ ] **Step 5: Static analysis, commit**

  ```bash
  python3 tools/quality_check.py
  git add apps/pychron-ui/src/dock_layouts.hpp apps/pychron-ui/src/dock_layouts.cpp apps/pychron-ui/CMakeLists.txt tests/ui/test_dock_layouts.cpp
  git commit -m "feat(ui): a helper that resets a window's docks and keeps its last layout"
  ```

---

### Task 2: `DockLayouts`: named arrangements

**Files:**
- Modify: `apps/pychron-ui/src/dock_layouts.hpp`, `apps/pychron-ui/src/dock_layouts.cpp`
- Test: `tests/ui/test_dock_layouts.cpp`

**Interfaces:**
- Consumes: Task 1's class and fixture.
- Produces, added to `DockLayouts`:

  ```cpp
  [[nodiscard]] QStringList names() const;                    // sorted, case-insensitive
  [[nodiscard]] Result<void> save_as(const QString& name);    // overwrites a name equal ignoring case
  [[nodiscard]] Result<void> apply(const QString& name);      // name matched ignoring case
  void remove(const QString& name);
  [[nodiscard]] bool contains(const QString& name) const;     // ignoring case
  [[nodiscard]] static Result<QString> valid_name(const QString& raw);
  signals:
    // "arrangement “<name>” not applied: <why>"
    void applyFailed(const QString& message);
  ```

- [ ] **Step 1: Write the failing tests**

  ```cpp
  void validNameTable_data(); void validNameTable();
  //  rows (raw, ok, trimmed): ("", false), ("   ", false), (QString(65, 'x'), false), (QString(64, 'x'), true),
  //  ("a/b", false), ("a\\b", false), ("a\tb", false), (" ok ", true, "ok")
  //  failures: error().kind == ErrorKind::Config, error().code == "bad_name", !error().what.empty()

  void arrangementRoundTrip();
  //  A->close(); B->setFloating(true); QVERIFY(save_as("bakeout")); reset();
  //  QCOMPARE(names(), QStringList{"bakeout"}); QVERIFY(apply("bakeout"));
  //  QVERIFY(!A->isVisible()); QVERIFY(B->isFloating());
  //  remove("bakeout"); QVERIFY(names().isEmpty()); QVERIFY(!settings.contains("win/arrangements/bakeout/state"));

  void namesAreSortedIgnoringCase();          // save "running", "Bakeout", "air" -> {"air", "Bakeout", "running"}

  void savingOverANameThatDiffersInCaseReplacesIt();
  //  save_as("running"); save_as("Running"); QCOMPARE(names(), QStringList{"Running"});
  //  QVERIFY(contains("RUNNING")); QVERIFY(apply("running"));

  void namesTheIniFormatManglesRoundTrip();   // Review Focus 1
  //  for name in {"Ünï 100% [a]=b", "...", "a.b", "General"}: save_as, QVERIFY(names().contains(name)),
  //  a second QSettings on the same file lists it too, apply succeeds.

  void garbageArrangementGivesFactoryAndAnError();
  //  settings.setValue("win/arrangements/bad/state", QByteArray("zz")); QSignalSpy spy(&layouts, &DockLayouts::applyFailed);
  //  const auto r = apply("bad"); QVERIFY(!r); QCOMPARE(r.error().code, std::string("bad_layout"));
  //  factory assertions hold; QCOMPARE(spy.count(), 1);
  //  QVERIFY(spy[0][0].toString().startsWith("arrangement “bad” not applied: "));

  void unknownArrangementLeavesTheLayoutAlone();
  //  A->close(); const auto r = apply("nope"); QCOMPARE(r.error().code, std::string("unknown_name"));
  //  QVERIFY(!A->isVisible());

  void aMissingGeometryStillAppliesTheState();   // rule 3: remove the geometry key, apply succeeds

  void aDockAddedSinceTheArrangementWasSavedStaysShown();   // rule 4
  //  save_as("old") in a window with A, B, C; second window on the same file has A, B, C, D (D in the factory);
  //  QVERIFY(apply("old")); QVERIFY(D->isVisible());

  void withoutSettingsSaveAsFails();           // code "no_settings"; apply("x") -> "unknown_name"

  void anUnwritableSettingsFileIsReported();   // Review Focus 2
  //  QSettings on a path whose parent is a regular file (dir.filePath("f") written first, then "f/s.ini");
  //  const auto r = save_as("x"); QVERIFY(!r); QCOMPARE(r.error().kind, ErrorKind::Io);
  //  QCOMPARE(r.error().code, std::string("not_saved")); QVERIFY(names().isEmpty());

  void applyingToAMaximizedWindowShowsThePanels();   // Review Focus 4
  //  save_as("small") with the window normal at 800x600 and A closed; reset(); w.showMaximized(); apply("small");
  //  QVERIFY(!A->isVisible()); QVERIFY(B->isVisible());
  //  QVERIFY(QGuiApplication::primaryScreen()->availableGeometry().intersects(w.frameGeometry()));
  ```

- [ ] **Step 2: Run to verify they fail**

  Run: `cmake --build --preset dev-ui --target pychron_ui_test_dock_layouts`
  Expected: compile error, `save_as` is not a member.

- [ ] **Step 3: Implement**

  - `valid_name`: trim; reject per Global Constraints; `what` says which rule failed, in words the dialog can show (for example `a name cannot contain “/”`).
  - Names are read with `beginGroup(group + "/arrangements")`, `childGroups()`, `endGroup()` inside one function, so the group is never left open. QSettings escapes key text itself: do not hand-escape.
  - `save_as`: validate; no settings: `no_settings`; remove an existing name equal ignoring case; write state and geometry; `settings->sync()`; if `settings->status() != QSettings::NoError` remove what was written and return `ErrorKind::Io`, code `not_saved`.
  - `apply`: find the name ignoring case, else `unknown_name` (no signal, layout untouched). Call `restoreGeometry` first when geometry bytes exist, ignoring its result; then `restoreState(bytes, 0)`. On false or empty bytes: `reset()`, emit `applyFailed`, return `bad_layout`.

- [ ] **Step 4: Run to verify they pass**

  Run: `cmake --build --preset dev-ui --target pychron_ui_test_dock_layouts && ctest --preset dev-ui -R ui.test_dock_layouts`
  Expected: all pass.

- [ ] **Step 5: Static analysis, commit**

  ```bash
  python3 tools/quality_check.py
  git add apps/pychron-ui/src/dock_layouts.hpp apps/pychron-ui/src/dock_layouts.cpp tests/ui/test_dock_layouts.cpp
  git commit -m "feat(ui): a window's dock layout can be saved under a name and applied"
  ```

---

### Task 3: The Extraction Line window keeps and resets its layout

**Files:**
- Modify: `apps/pychron-ui/src/main_window.hpp`, `apps/pychron-ui/src/main_window.cpp:151-183,492-503`, `apps/pychron-ui/src/main.cpp:474`
- Test: `tests/ui/test_docks.cpp`

**Interfaces:**
- Consumes: `DockLayouts` (Tasks 1, 2).
- Produces:

  ```cpp
  explicit MainWindow(systems::ExtractionLine& line, std::unique_ptr<QSettings> settings = nullptr, QWidget* parent = nullptr);
  DockLayouts* dock_layouts() const noexcept;
  ```

  Every existing call `MainWindow(line)` compiles unchanged. A call that passes a parent as second argument must be updated: `grep -rn "MainWindow [a-z_]*(.*," apps tests` and fix what it finds.

- [ ] **Step 1: Write the failing tests** (in `test_docks.cpp`, using its existing line fixture; settings on a `QTemporaryDir` ini file)

  ```cpp
  void mainWindowResetShowsClosedPanelsAgain();
  //  window.log_dock()->close(); window.alarm_dock()->setFloating(true);
  //  if (window.cryo_dock()) window.cryo_dock()->close(); if (window.heater_dock()) window.heater_dock()->close();
  //  window.dock_layouts()->reset();
  //  every non-null dock: isVisible(), !isFloating();
  //  QCOMPARE(window.dockWidgetArea(window.log_dock()), Qt::BottomDockWidgetArea);
  //  QCOMPARE(window.dockWidgetArea(window.alarm_dock()), Qt::RightDockWidgetArea);

  void mainWindowKeepsItsLayoutAcrossInstances();
  //  window 1 with settings: log_dock()->close(); window.close();
  //  QVERIFY(settings.contains(QStringLiteral("main_window/%1/state").arg(system name of the fixture line)));
  //  window 2 with settings on the same file: QVERIFY(!window2.log_dock()->isVisible());

  void mainWindowWithoutSettingsWritesNothing();
  //  MainWindow window(line); QVERIFY(!window.dock_layouts()->can_save()); window.close();
  //  QVERIFY(QDir(dir.path()).isEmpty());

  void mainWindowLogsAnArrangementThatCannotBeApplied();
  //  settings.setValue("main_window/<name>/arrangements/bad/state", QByteArray("zz"));
  //  (void)window.dock_layouts()->apply("bad");
  //  the log model's last row text contains "arrangement “bad” not applied"  and its level is WARN
  //  (read it the way logDockAppendsFormattedLines does)

  void mainWindowPanelsListItsClosableDocks();
  //  texts of dock_layouts()->panel_actions() == window titles of log, alarms, then cryo and heaters when present
  ```

- [ ] **Step 2: Run to verify they fail**

  Run: `cmake --build --preset dev-ui --target pychron_ui_test_docks`
  Expected: compile error, `dock_layouts` is not a member of `MainWindow`.

- [ ] **Step 3: Implement**

  - Members: `std::unique_ptr<QSettings> settings_;` declared before the docks; `DockLayouts* layouts_`.
  - Move the four `addDockWidget` calls into private `void default_layout()`. For each non-null dock: `setFloating(false)`, `addDockWidget(area, dock)` (Log bottom; Alarms, Cryo, Heaters right, in that order), `show()`. Creating `cryo_` / `heaters_` and their `connect`s stays in the constructor, before the call.
  - After `default_layout()`: `layouts_ = new DockLayouts(this, [this] { default_layout(); }, settings_.get(), QStringLiteral("main_window/%1").arg(QString::fromStdString(line.config().system.name)));` then `layouts_->restore_last();`.
  - `connect(layouts_, &DockLayouts::applyFailed, log_, [this](const QString& m) { log_->append_line(QStringLiteral("WARN [ui] ") + m); });`
  - `closeEvent`: `layouts_->save_last();` immediately before `QMainWindow::closeEvent(event)`, after the experiment window has agreed to close.
  - `main.cpp:474`: `pychron::ui::MainWindow window(**line, std::make_unique<QSettings>());`
  - Update the header comment of `main_window.hpp` (docks listed; layout kept per line).

- [ ] **Step 4: Run to verify they pass, and nothing else broke**

  Run: `cmake --build --preset dev-ui && ctest --preset dev-ui -R '^ui\.'`
  Expected: all pass.

- [ ] **Step 5: Static analysis, commit**

  ```bash
  python3 tools/quality_check.py
  git add apps/pychron-ui/src/main_window.hpp apps/pychron-ui/src/main_window.cpp apps/pychron-ui/src/main.cpp tests/ui/test_docks.cpp
  git commit -m "feat(ui): the extraction line window keeps its panel layout and can reset it"
  ```

---

### Task 4: The Experiment and Spectrometer windows use the helper

**Files:**
- Modify: `apps/pychron-ui/src/experiment_window.hpp`, `apps/pychron-ui/src/experiment_window.cpp:112-131,178-181,201,565-568`
- Modify: `apps/pychron-ui/src/spectrometer_window.hpp`, `apps/pychron-ui/src/spectrometer_window.cpp:102-118,296-304,349-353`
- Test: `tests/ui/test_experiment_window.cpp`, `tests/ui/test_spectrometer_window.cpp`

**Interfaces:**
- Consumes: `DockLayouts`, `DockLayouts::Keys`.
- Produces: `DockLayouts* dock_layouts() const noexcept;` on both windows.

- [ ] **Step 1: Write the failing tests**

  `test_experiment_window.cpp`:

  ```cpp
  void resetPutsTheDocksAndTheToolbarBack();
  //  docks by objectName: ExperimentExecutorDock, ExperimentEvolutionsDock, ExperimentFactoryDock, ExperimentMeasurementDock
  //  evolutions->close(); measurement->setFloating(true); toolbar (ExperimentToolBar)->hide();
  //  window.addToolBar(Qt::BottomToolBarArea, toolbar); window.dock_layouts()->reset();
  //  all four docks visible and not floating; areas: executor bottom, evolutions right, factory left;
  //  QVERIFY(window.tabifiedDockWidgets(factory).contains(measurement));
  //  QVERIFY(toolbar->isVisible()); QCOMPARE(window.toolBarArea(toolbar), Qt::TopToolBarArea);

  void panelsLeaveOutTheExecutor();
  //  texts of panel_actions() == {"Evolutions", "Run Factory", "Measurement"}

  void theLayoutIsStillKeptUnderItsOldKeys();
  //  close the window; QVERIFY(settings.contains("experiment_window/state"));
  //  QVERIFY(settings.contains("experiment_window/geometry"));
  ```

  `test_spectrometer_window.cpp`:

  ```cpp
  void resetPutsTheDocksBack();
  //  intensities (SpectrometerIntensitiesDock)->close(); controls (SpectrometerControlsDock)->setFloating(true);
  //  reset(); both visible, not floating; controls left, intensities right.

  void panelsLeaveOutTheControls();            // texts == {"Intensities"}

  void anArrangementIsKeptUnderTheSpectrometersName();
  //  QVERIFY(window.dock_layouts()->save_as("scan"));
  //  QVERIFY(settings.contains(QStringLiteral("spectrometer_window/%1/arrangements/scan/state").arg(bridge.name())));
  ```

  `closingStopsScanAndSavesSettings` and `staleOrCorruptSettingsIgnored` must pass unchanged: they pin `dock_state` and the behaviour on bad bytes.

- [ ] **Step 2: Run to verify they fail**

  Run: `cmake --build --preset dev-ui --target pychron_ui_test_experiment_window pychron_ui_test_spectrometer_window`
  Expected: compile error, `dock_layouts` is not a member.

- [ ] **Step 3: Implement `ExperimentWindow`**

  - Keep the four docks and the toolbar as members (they are locals today).
  - Private `void default_layout()`: each dock `setFloating(false)`; `addDockWidget` executor bottom, evolutions right, factory left; `tabifyDockWidget(factory, measurement)`; all `show()`; `factory->raise()`; the two `resizeDocks` calls with today's numbers (`{500, 380}` horizontal for evolutions and factory, `{300}` vertical for executor); `addToolBar(Qt::TopToolBarArea, toolbar)` and `toolbar->show()`.
  - The toolbar is created at line 201, after the docks: call `default_layout()` once, after the toolbar exists, and create the helper there: group `experiment_window`, default `Keys`.
  - Replace lines 178-181 with `layouts_->restore_last()` at that same later point, and lines 565-568 with `layouts_->save_last()`.

- [ ] **Step 4: Implement `SpectrometerWindow`**

  - Docks as members; `default_layout()`: controls left, intensities right, both docked and shown.
  - Helper: group `QStringLiteral("spectrometer_window/%1").arg(bridge_.name())`, `Keys{QStringLiteral("dock_state"), QStringLiteral("geometry")}`.
  - `load_settings()`: drop its geometry and `dock_state` lines; call `layouts_->restore_last()` before `s.beginGroup(...)`. `save_settings()`: drop those two `setValue` lines; call `layouts_->save_last()` before `s.beginGroup(...)`. The helper uses full key paths, so it must not run while the window has a group open.

- [ ] **Step 5: Run to verify they pass**

  Run: `cmake --build --preset dev-ui && ctest --preset dev-ui -R '^ui\.'`
  Expected: all pass, the two pre-existing spectrometer settings tests included.

- [ ] **Step 6: Static analysis, commit**

  ```bash
  python3 tools/quality_check.py
  git add apps/pychron-ui/src/experiment_window.hpp apps/pychron-ui/src/experiment_window.cpp apps/pychron-ui/src/spectrometer_window.hpp apps/pychron-ui/src/spectrometer_window.cpp tests/ui/test_experiment_window.cpp tests/ui/test_spectrometer_window.cpp
  git commit -m "feat(ui): the experiment and spectrometer windows can reset their panel layout"
  ```

---

### Task 5: Window menu: Panels, Arrangements, Reset Layout

**Files:**
- Modify: `apps/pychron-ui/src/menu_hub.hpp`, `apps/pychron-ui/src/menu_hub.cpp` (constructor near line 71, `commands()` line 189, `refresh_windows()` line 276, `window_menu()` line 298)
- Test: `tests/ui/test_menu_hub.cpp` (snake_case slots, as there)

**Interfaces:**
- Consumes: `DockLayouts::of`, `panel_actions`, `names`, `contains`, `save_as`, `apply`, `remove`, `reset`, `can_save`, `valid_name`.
- Produces, on `MenuHub`:

  ```cpp
  QAction* panels_action() const;            // "Panels", has a submenu
  QAction* arrangements_action() const;      // "Arrangements", has a submenu
  QAction* reset_layout_action() const;      // "Reset Layout"
  QAction* save_arrangement_action() const;  // "Save Arrangement As…"

  struct ArrangementAsks {
    std::function<std::optional<QString>(QWidget* over)> name;                    // nullopt: cancelled
    std::function<bool(QWidget* over, const QString& name)> replace;
    std::function<void(QWidget* over, const QString& why)> refuse;                // an invalid name
  };
  void set_arrangement_asks(ArrangementAsks asks);   // tests; the defaults are the dialogs of spec 4.5
  ```

  Defaults: `QInputDialog::getText(over, tr("Save Arrangement"), tr("Name:"))`; `QMessageBox::question(over, tr("Save Arrangement"), tr("Replace arrangement “%1”?").arg(name))`; `QMessageBox::warning(over, tr("Save Arrangement"), why)`.

- [ ] **Step 1: Write the failing tests**

  Helper for the tests: a `QMainWindow` with docks `A`, `B` and a `DockLayouts` on a temp ini file (same shape as Task 1's fixture; copy the few lines, the fixture is not shared between test executables).

  ```cpp
  void the_window_menu_has_the_layout_commands();
  //  for bars in {PerWindow, Shared}: the Window menu's actions, separators as nullptr, begin
  //  {minimize, zoom, nullptr, bring_all, nullptr, panels, arrangements, reset_layout, nullptr, <open windows...>}

  void layout_commands_follow_the_window_in_front();
  //  plain QMainWindow in front: panels, arrangements, reset_layout all !isEnabled().
  //  a window with a helper in front: all enabled; emit panels_action()->menu()->aboutToShow();
  //  texts of its actions == {"A", "B"}.
  //  two helper windows w1 (docks A, B) and w2 (docks X): activate w2 -> Panels lists {"X"};
  //  close A in w1 and X in w2, reset_layout_action()->trigger(): X visible again, A still closed.

  void save_arrangement_asks_again_after_a_bad_name();
  //  asks.name returns "a/b" then "bakeout"; asks.refuse counts calls.
  //  save_arrangement_action()->trigger(); refuse called once with a non-empty reason;
  //  layouts.names() == {"bakeout"}.

  void save_arrangement_asks_before_replacing();
  //  existing "bakeout"; name returns "Bakeout" then nullopt; replace returns false:
  //  names() == {"bakeout"} and name was asked twice.  Then name returns "Bakeout", replace returns true:
  //  names() == {"Bakeout"}.

  void save_arrangement_is_disabled_without_settings();   // helper built with nullptr settings

  void the_arrangements_menu_applies_and_deletes();
  //  save "bakeout" (A closed) and "running"; reset; aboutToShow on arrangements_action()->menu();
  //  its actions: "bakeout", "running", separator, save_arrangement_action(), an action "Delete" with a submenu
  //  holding "bakeout", "running".  Trigger "bakeout": A hidden.  Trigger Delete > "running": names() == {"bakeout"}.
  //  With no arrangements the "Delete" action is not visible.

  void a_failed_apply_is_shown_in_the_status_bar();
  //  garbage bytes under "bad"; trigger it from the menu;
  //  QVERIFY(w.statusBar()->currentMessage().startsWith("arrangement “bad” not applied: "));

  void the_command_palette_has_reset_and_save();
  //  commands() contains {reset_layout_action(), Menu::Window} and {save_arrangement_action(), Menu::Window}.

  void layout_commands_without_a_front_window_do_nothing();   // Review Focus 5
  //  (a) a QDialog child of the helper window shown and active: reset_layout_action() is disabled;
  //      trigger() anyway: no crash, layout unchanged.
  //  (b) a Qt::Popup child shown over the helper window (as the command palette is): reset_layout_action()
  //      is enabled and trigger() resets that window (active_window() is the window under the popup).
  //  (c) the helper window hidden, nothing active: disabled; trigger(): no crash.
  ```

- [ ] **Step 2: Run to verify they fail**

  Run: `cmake --build --preset dev-ui --target pychron_ui_test_menu_hub`
  Expected: compile error, `panels_action` is not a member.

- [ ] **Step 3: Implement**

  - Create the four actions and the two submenus (`QMenu` with no parent widget, deleted in `~MenuHub` when not `closingDown`, like `shared_`) in the constructor beside `minimize_`, before any bar is made.
  - One private `DockLayouts* front_layouts() const { return DockLayouts::of(current_window()); }`. Every triggered slot starts by reading it and returns on null: an action can be triggered from the palette while disabled state is stale.
  - `window_menu()`: after `bring_all_`, append `nullptr, panels_, arrangements_, reset_layout_`.
  - `refresh_windows()`: enable the three from `front_layouts() != nullptr`; `save_arrangement_` from `can_save()`.
  - `Panels` `aboutToShow`: `clear()`, then `addActions(panel_actions())`. The toggle actions belong to the docks; `clear()` does not delete actions the menu does not own.
  - `Arrangements` `aboutToShow`: `clear()`; one action per name (created with the menu as parent, so `clear()` deletes them); separator; `save_arrangement_`; a "Delete" submenu of the names, its action hidden when there are none.
  - Apply slot: on error, when the window is a `QMainWindow`, `statusBar()->showMessage(QStringLiteral("arrangement “%1” not applied: %2").arg(name, why), 5000)`.
  - Save flow, as a loop: ask `name`; `nullopt` ends it; `valid_name` fails: `refuse`, ask again; `contains` and `replace` says no: ask again; `save_as`; on error `refuse` with its `what` and end.
  - `commands()`: after the contributed groups of `Menu::Window`, append `reset_layout_` and `save_arrangement_`.
  - `MenuHub::reset()` gives a fresh hub with default asks; `test_menu_hub.cpp`'s `cleanup()` already resets, so an injected ask does not leak into the next test only if the test called `reset` or restores the defaults: set the asks back in `cleanup()`.
  - Update the header comment of `menu_hub.hpp` (the Window menu paragraph).

- [ ] **Step 4: Run to verify they pass**

  Run: `cmake --build --preset dev-ui && ctest --preset dev-ui -R '^ui\.'`
  Expected: all pass. `test_command_palette` and `test_shortcuts` included: if either pins the full command list or shortcut table, add the two commands there (no shortcut for either).

- [ ] **Step 5: Static analysis, commit**

  ```bash
  python3 tools/quality_check.py
  git add apps/pychron-ui/src/menu_hub.hpp apps/pychron-ui/src/menu_hub.cpp tests/ui/test_menu_hub.cpp
  git commit -m "feat(ui): the Window menu shows panels, resets the layout and keeps arrangements"
  ```

---

### Task 6: User docs and the run in the real application

**Files:**
- Modify: `docs/user/02-extraction-line.md`, `docs/user/03-spectrometer.md`, `docs/user/04-experiments.md`, `docs/user/10-troubleshooting.md`
- Modify (only if the code departed from it): `docs/superpowers/specs/2026-10-09-dock-arrangements-design.md`

- [ ] **Step 1: Write the docs**

  - `02-extraction-line.md`: a section "Panels and arrangements": the panels (Log, Alarms, Cryostat, Heaters) can be closed, moved and floated; Window > Panels shows or hides one; Window > Reset Layout puts them back as installed without changing the window's size; Window > Arrangements > Save Arrangement As… keeps the current layout under a name (1 to 64 characters, no `/` or `\`), choosing a name applies it, Delete removes it. The layout is remembered between sessions, per line and per user. Each window (Extraction Line, Experiment, Spectrometer) has its own arrangements; the commands act on the window in front.
  - `03-spectrometer.md`, `04-experiments.md`: one sentence each pointing to that section, naming the panel that cannot be closed (Controls; Executor).
  - `10-troubleshooting.md`: entry "A panel has disappeared" → Window > Panels, or Window > Reset Layout.

- [ ] **Step 2: Whole suite and static analysis on the rebased branch**

  ```bash
  git fetch origin && git rebase origin/develop
  python3 tools/quality_check.py
  cmake --build --preset dev-ui && ctest --preset dev-ui
  ```

  Expected: `quality_check.py` exits 0; every test passes.

- [ ] **Step 3: See it in the application**

  Run `build/dev-ui/apps/pychron-ui/pychron-ui --sim` on the example line (path of the binary: `find build/dev-ui -name pychron-ui -type f`). Check by eye, in each of the three windows: close every closable panel, float one, Window > Reset Layout restores it; save two arrangements and switch between them; quit and start again, the last layout is back; on macOS the Window menu has one set of the commands and they grey out when a Data window is in front.

- [ ] **Step 4: Commit**

  ```bash
  git add docs/user docs/superpowers/specs/2026-10-09-dock-arrangements-design.md
  git commit -m "docs: panels, layout reset and arrangements in the user guide"
  ```
