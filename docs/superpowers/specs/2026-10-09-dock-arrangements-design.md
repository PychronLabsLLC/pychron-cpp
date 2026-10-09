# Dock arrangements and layout reset: design

Date: 2026-10-09. Status: draft, awaiting review.

## 1. Problem

- Extraction line window (`MainWindow`): canvas + docks Log, Alarms, Cryo
  (line has `[cryo]`), Heaters (line has `[[heaters]]`). All closable,
  movable, floatable.
- No `toggleViewAction`, no menu entry per dock: closed dock gone until
  restart.
- `MainWindow` never saves or restores layout. Restart = only reset.
- `ExperimentWindow`, `SpectrometerWindow` persist layout (QSettings) but
  have no reset: bad saved layout (dock floated onto absent monitor, all
  closed) stays bad across restarts.
- No way to keep more than one layout.

## 2. Goal

For every docked window (Extraction Line, Experiment, Spectrometer), each on
its own:

1. Reshow a closed panel without restart.
2. One command back to factory layout, any time.
3. Save current layout under a name, apply it later, delete it.
4. Last layout survives restart (new for Extraction Line).

Success: operator closes every panel, floats some, picks
Window > Reset Layout, window looks as on first launch. Operator saves
"bakeout" and "running", switches between them from menu.

## 3. Terms

- Layout: a window's dock state (`QMainWindow::saveState()` bytes): which
  docks shown, area, tabbing, floating, sizes.
- Factory layout: layout the window's own code builds. Never stored.
- Arrangement: named, user-saved layout + window geometry
  (`saveGeometry()` bytes). Belongs to one window kind.
- Last layout: layout + geometry written when window closes.

## 4. Components

### 4.1 `DockLayouts` (new)

Files: `apps/pychron-ui/src/dock_layouts.hpp`, `dock_layouts.cpp`.
`QObject`, child of the `QMainWindow` it serves. Hub finds it with
`window->findChild<DockLayouts*>(QString(), Qt::FindDirectChildrenOnly)`.

```cpp
class DockLayouts : public QObject {
 public:
  struct Keys {            // names of the last-layout keys inside `group`
    QString state = QStringLiteral("state");
    QString geometry = QStringLiteral("geometry");
  };
  // `factory` puts every dock where the window's code wants it. `settings`
  // may be null: nothing is then read or written, and save_as() fails.
  DockLayouts(QMainWindow* window, std::function<void()> factory, QSettings* settings, QString group, Keys keys = {});

  void reset();                                  // factory layout
  void restore_last();                           // no-op without settings or saved bytes
  void save_last();
  [[nodiscard]] QStringList names() const;       // sorted, case-insensitive
  [[nodiscard]] Result<void> save_as(const QString& name);  // overwrites
  [[nodiscard]] Result<void> apply(const QString& name);
  void remove(const QString& name);
  [[nodiscard]] bool contains(const QString& name) const;   // ignoring case
  static DockLayouts* of(const QWidget* window); // its helper, or nullptr
  [[nodiscard]] bool can_save() const;           // has settings
  [[nodiscard]] QList<QAction*> panel_actions() const;      // closable docks, in creation order
  static Result<QString> valid_name(const QString& raw);    // trimmed name or why not
};
```

- `settings` not owned: it is the window's `settings_` member. The helper
  never touches it in its destructor, because a child `QObject` is destroyed
  after the window's members are.
- `panel_actions()`: `toggleViewAction()` of each direct-child `QDockWidget`
  with `DockWidgetClosable`. Text = dock's window title.

### 4.2 Factory layout: a function, not captured bytes

Each window moves its dock placement into one private member
`void default_layout()`; constructor calls it once, `DockLayouts::reset()`
calls it again. Function must be re-runnable: for each dock
`setFloating(false)`, `addDockWidget(area, dock)` (moves a dock already
added), `show()`; then `tabifyDockWidget` / `resizeDocks` as today.

Reason not to capture `saveState()` in constructor: window not shown yet,
dock sizes in those bytes not reliable; function is what already defines
the layout, so nothing to keep in sync.

Reset does not change window geometry (size, position, maximized).

### 4.3 Windows

| Window | group | last keys | settings source |
|---|---|---|---|
| `MainWindow` | `main_window/<line system name>` | `state`, `geometry` | new ctor arg |
| `ExperimentWindow` | `experiment_window` | `state`, `geometry` (existing) | existing `settings_` |
| `SpectrometerWindow` | `spectrometer_window/<bridge name>` | `dock_state`, `geometry` (existing) | existing `settings_` |

- Existing keys kept: nobody's saved layout lost on upgrade.
- `MainWindow` keyed by line name: its dock set depends on line.
- `MainWindow` constructor gains
  `std::unique_ptr<QSettings> settings = nullptr` (after `line`, before
  `parent`). Null means the window persists nothing; `main.cpp` passes
  `std::make_unique<QSettings>()`. Tests that do not care pass nothing and
  touch no settings file. This differs from the other two windows (null =
  application's QSettings) on purpose: `MainWindow` is built by dozens of
  tests.
- `MainWindow::closeEvent`: `save_last()` only once close is certain (after
  experiment window agreed to close).
- `ExperimentWindow`, `SpectrometerWindow`: their `restoreState` /
  `saveState` / geometry lines replaced by `restore_last()` / `save_last()`.
  Other settings in those groups (scan width, scale, …) untouched.
- `DataMainWindow` and windows in section 9: no helper.

### 4.4 Menu (`MenuHub`)

Hub-owned actions, like Minimize / Zoom: one set, shared by every bar, act
on `current_window()`'s `DockLayouts`. Not contributed per window (three
windows would put three "Reset Layout" in the one macOS bar).

Window menu becomes:

```
Minimize
Zoom
---
Bring All to Front
---
Panels            ▸  ☑ Log  ☑ Alarms  ☐ Cryostat  ☑ Heaters
Arrangements      ▸  bakeout
                     running
                     ---
                     Save Arrangement As…
                     Delete            ▸  bakeout / running
Reset Layout
---
<open windows>
```

- `Panels`, `Arrangements` submenus filled in `aboutToShow` from front
  window's helper. One `QMenu` each, owned by hub.
- Front window has no `DockLayouts`: `Panels`, `Arrangements`,
  `Reset Layout` disabled (updated where Minimize / Zoom are, in
  `refresh_windows()`).
- `Save Arrangement As…` disabled when `!can_save()`.
- `Delete` submenu hidden when no arrangements.
- Accessors for tests: `panels_action()`, `arrangements_action()`,
  `reset_layout_action()`, `save_arrangement_action()`.
- `MenuHub::commands()` also returns `Reset Layout` and
  `Save Arrangement As…` (menu `Window`), so command palette has them.
- No shortcuts.

### 4.5 Save dialog

`QInputDialog::getText`, window modal over front window, title
"Save Arrangement", label "Name:", prefilled with nothing.
- Invalid name: `QMessageBox::warning` with reason, dialog asked again.
- Name exists (case-insensitive): `QMessageBox::question`
  "Replace arrangement “<name>”?"; No returns to name dialog.
- Hub takes injectable ask functions (name, confirm) so tests do not open
  modal dialogs; pattern as `ExperimentWindow`'s `ask_stop_`.

Delete: no confirmation (one click in a submenu named Delete; arrangement
is cheap to remake).

## 5. Storage

QSettings, per user, application's (`PychronLabs` / `pychron-ui`).

```
<group>/<keys.state>                      QByteArray   last layout
<group>/<keys.geometry>                   QByteArray   last geometry
<group>/arrangements/<name>/state         QByteArray
<group>/arrangements/<name>/geometry      QByteArray
```

- `saveState()` version argument stays 0 (existing saved bytes are 0).
- `<name>` stored as typed (after trim). Lookup case-insensitive: saving
  "Running" over "running" removes the old key first.

## 6. Rules

1. Name valid if, after trim: length 1..64, no `/`, no `\`, no control
   character. Otherwise `save_as` returns `ErrorKind::Config`, `code`
   `"bad_name"`, with reason; nothing written. Without settings:
   `ErrorKind::Config`, `code` `"no_settings"`.
2. Saved bytes are untrusted. `restoreState` returns false, or bytes empty:
   `reset()` is run, and `apply` returns `ErrorKind::Config`, `code`
   `"bad_layout"` (`"unknown_name"` when no such arrangement; layout then
   left as it is), naming the arrangement.
   `restore_last` in same case: factory, silently.
3. `apply` restores geometry before state. Geometry bytes missing or
   refused: state still applied.
4. Dock present in window but absent from saved bytes (line gained
   heaters since arrangement saved): Qt leaves it where it is; it stays
   shown at its factory place because the window ran `default_layout()`
   before any restore.
5. Dock named in saved bytes but absent from window: ignored by Qt.
6. `reset()` leaves named arrangements and last-layout keys alone; changes
   one window only.
7. After `reset()`: every dock of that window `isVisible()`,
   `!isFloating()`, in its factory area; tab groups as factory.
8. Non-closable docks (`ExperimentExecutorDock`,
   `SpectrometerControlsDock`) stay non-closable and are not in `Panels`.
9. Every dock a helper manages must have a non-empty `objectName`; helper
   constructor asserts it (`Q_ASSERT`), since Qt silently drops unnamed
   docks from `saveState()`.
10. Helper emits `applyFailed(QString message)`, message
    `arrangement “<name>” not applied: <why>`, on `"bad_layout"` only.
    `apply` failure is reported in the window's status bar
    (`statusBar()->showMessage`, 5 s); Extraction Line also writes
    `WARN [ui] arrangement “<name>” not applied: <why>` to its log dock.
11. `save_as` syncs settings. If `QSettings::status()` is then not
    `NoError`, what it wrote is removed and it returns `ErrorKind::Io`,
    `code` `"not_saved"`: an arrangement never listed now and gone at next
    start.
12. Layout command with no usable front window: front is a dialog or
    nothing → commands disabled, triggering does nothing; front is a popup
    (command palette) → acts on window under it (`MenuHub::active_window()`).

## 7. Tests

New `tests/ui/test_dock_layouts.cpp` (QtTest, registered in
`tests/ui/CMakeLists.txt`; `dock_layouts.cpp` added to
`apps/pychron-ui/CMakeLists.txt`). QSettings on `QTemporaryDir` ini file.

Helper, on a bare `QMainWindow` with three named docks:
- close all + float one + move one, `reset()`: rule 7 holds.
- `save_as` / `names` / `apply` / `remove` round trip; `apply` brings back
  visibility, floating and area.
- second window instance on same settings: `restore_last` gives what first
  one's `save_last` wrote.
- garbage bytes under an arrangement: `apply` errors, layout is factory.
- garbage last layout: factory, no error.
- `valid_name` table: empty, spaces only, 65 chars, `a/b`, `a\b`, tab,
  " ok " → "ok".
- "Running" over "running": one arrangement left, named "Running".
- null settings: `can_save()` false, `save_as` errors, `reset` works,
  no file written.
- dock added after arrangement saved (rule 4): shown after `apply`.
- `panel_actions`: closable docks only; toggling action hides / shows.

Hub (`tests/ui/test_menu_hub.cpp`):
- Window menu order as 4.4, in both `Bars` modes.
- three actions disabled for a window without helper, enabled with.
- two windows with helpers: `Panels` lists front window's docks; Reset
  changes front window only.
- Save flow with injected asks: invalid then valid name; replace refused
  then accepted.
- `commands()` contains the two actions.

Windows:
- `tests/ui/test_docks.cpp`: `MainWindow` with settings saves on close and
  restores in a second instance; without settings writes nothing; reset
  shows closed Log / Alarms / Cryo / Heaters again.
- `test_experiment_window.cpp`, `test_spectrometer_window.cpp`: existing
  persistence tests pass unchanged (old keys); one reset case each
  (factory tabbing of Factory / Measurement docks restored).

## 8. Docs

- `docs/user/02-extraction-line.md`: panels, reset, arrangements.
- `docs/user/03-spectrometer.md`, `04-experiments.md`: one line each
  pointing there.
- `docs/user/10-troubleshooting.md`: "a panel disappeared" →
  Window > Reset Layout.
- `menu_hub.hpp` header comment: Window menu description updated.

## 9. Out of scope

- Whole-workspace arrangements (which top-level windows are open, where).
- Arrangements shared between users, shipped in line config, exported or
  imported.
- Flux, Figure, Isotope Evolution, Reference Fit, Script Editor, Laser,
  Data windows: docks there are fixed two-panel layouts with no
  persistence. Adding one later = `default_layout()` + one helper.
- Rename of an arrangement (save under new name, delete old).
- Shortcuts for arrangements.
- Canvas zoom / pan, table column widths, splitter positions: not dock
  state.
- Toolbars: `saveState()` carries them where a window has one
  (`ExperimentToolBar`); reset puts them back only if `default_layout()`
  places them. `ExperimentWindow::default_layout()` does
  (`addToolBar(Qt::TopToolBarArea, bar)`, `show()`).
