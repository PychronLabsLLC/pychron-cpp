# File menu: New, Open, Save, Save As in one place: design

Date: 2026-10-09. Status: approved 2026-10-09; implemented on this branch.

## 1. Problem

- Same four commands, three places, three names:
  - Experiment window: Open, Save, Save As in **Queue** menu
    (`experiment_window.cpp` `build_actions`).
  - Script editor: New, Save in **Scripts** menu
    (`script_editor_window.cpp`).
  - Conditionals editor: Save Conditionals in **Scripts** menu; New only a
    toolbar `+` (`conditionals_editor_window.cpp`).
- File menu holds only Installations, Preferences, Quit.
- Shortcut already follows active window (hub gates window-scoped actions);
  menu placement does not. Operator looks under File, finds nothing.
- Holes: no New queue; no Open command in script or conditionals editor
  (list click only, no keyboard path, not in command palette).

## 2. Goal

File > New, Open…, Save, Save As… act on what the window in front edits:

| Window in front | New | Open… | Save | Save As… |
|---|---|---|---|---|
| Experiment | empty queue | queue file dialog | queue | queue file dialog |
| Script editor | kind + name prompt | pick lab script | current tab | greyed |
| Conditionals editor | name prompt | pick lab file | current file | greyed |
| any other | greyed | greyed | greyed | greyed |

Success: Cmd+S in script editor saves script, in experiment window saves
queue; File menu shows one Save, labelled for what it will save; Queue and
Scripts menus no longer carry copies.

## 3. Hub

`MenuHub` (`apps/pychron-ui/src/menu_hub.hpp`) owns four actions, created in
constructor before any bar, shared by every bar (as `minimize_` is):

```cpp
enum class FileRole { New, Open, Save, SaveAs };
static constexpr std::size_t kFileRoles = 4;

// `action` is what File > <role> does while `owner`'s window is in front,
// for as long as both live. `noun` names what it acts on ("Queue").
void set_file_action(QWidget* owner, FileRole role, QAction* action, const QString& noun);
// The hub's own File item for `role` (the same in every bar).
QAction* file_action(FileRole role) const;
```

- Placement: first group of File, order New, Open…, Save, Save As…, then
  separator, then contributed groups (Installations/Preferences, Quit).
  File is therefore never hidden.
- Hub item is proxy. Target = action registered for the role by a window
  whose `window()` is `active_window()`. At most one target per role per
  window; registering again for same owner and role replaces.
- State, recomputed on `focusWindowChanged`, on target's `QAction::changed`,
  on registration, on owner or target destroyed:
  - enabled = target exists and `target->isEnabled()`;
  - text = role text with noun when target exists, plain otherwise:

    | Role | With noun | Plain |
    |---|---|---|
    | New | `&New <noun>…` | `&New…` |
    | Open | `&Open <noun>…` | `&Open…` |
    | Save | `&Save <noun>` | `&Save` |
    | SaveAs | `Save <noun> &As…` | `Save &As…` |

- Trigger: target is resolved again when the hub item is triggered, from
  the window then in front, and `target->trigger()` is called; nothing
  happens without one. (Palette or a menu left open can outlive the window
  the item was enabled for; same rule as Reset Layout.)
- Shortcuts on hub items: `Shortcut::FileNew`, `FileOpen`, `FileSave` =
  `QKeySequence::New`, `Open`, `Save`. Save As has no key, as before:
  `QKeySequence::SaveAs` is Ctrl+Shift+S, which is View > Spectrometer.
- A registered window action carries no shortcut: hub item has it, two
  would be ambiguous.
- Hub item with no target is disabled, and a disabled action's shortcut is
  inactive. This matters: `Shortcut::RecallNext` is Ctrl+N in data browser,
  same key as File > New. Data browser registers no New, so its Ctrl+N
  keeps working. A window must not both register a role and bind that
  role's key to something else.
- `commands()` (command palette) lists the four hub items under
  `Menu::File`, first. Registered targets are not contributed to any menu,
  so each command appears once.
- No change to `contribute`, gates, `Bars` modes, or other menus.

## 4. Windows

### 4.1 Experiment window

- `open_`, `save_`, `save_as_` no longer added to `Menu::Queue`, lose their
  shortcuts, and are registered: Open, Save, SaveAs, noun `Queue`. Toolbar
  keeps `open_`, `save_`.
- New action `new_`, registered as New, calls new method:

  ```cpp
  // Empties the queue and forgets its file. Refused (false, with `error`)
  // while a queue runs. Asks about unsaved edits first.
  bool new_queue(QString* error = nullptr);
  ```

  - running: false, `error` = "a queue is running"; action shows the same
    information box Open shows.
  - `resolve_unsaved()` false: false, `error` = "cancelled", queue untouched,
    no box.
  - else `model_.set_queue({})`, `path_.reset()`, `set_modified(false)`.
    `experiment_window/last_queue` setting left as is (it seeds Open's
    dialog).
- Queue menu keeps Revalidate and whatever else it has.

### 4.2 Script editor

- New and Save actions leave `Menu::Scripts`, lose shortcuts, registered as
  New and Save, noun `Script`. Close Tab, Check Now, Go to Gosub stay in
  Scripts with their keys.
- Save's action is enabled only with a tab open (today it is always enabled
  and does nothing without one), so File > Save greys with no tab.
- New action Open, registered as Open: asks which script, opens it.

  ```cpp
  // Which of `names` ("kind/name", as the tree lists them) to open; nullopt:
  // cancelled. A dialog by default.
  using PickScript = std::function<std::optional<QString>(const QStringList& names)>;
  void set_pick_script(PickScript pick);
  // File > Open: asks, then opens or switches to the tab. false on cancel,
  // no scripts, or a file that cannot be read.
  bool open_picked();
  ```

  Default dialog: `QInputDialog::getItem`, not editable, over
  `script_names()`. Not a file dialog: a script outside the lab's script
  directories is not a `ScriptFile`. Open's action disabled when
  `script_names()` is empty.
- No SaveAs registered.

### 4.3 Conditionals editor

- Save Conditionals leaves `Menu::Scripts`, loses shortcut, registered as
  Save, noun `Conditionals`. Enabled state as today.
- New action New, registered: same name prompt and `new_file` the toolbar
  `+` uses; `+` triggers this action.
- New action Open, registered: `set_pick_file` / `open_picked()`, same shape
  as 4.2 over `file_names()`, then existing `open(name)` (so unsaved
  question applies). Disabled when no files.
- No SaveAs registered.

### 4.4 Everything else

Registers nothing. Store-backed windows (Samples, Packages, Flux, Recall,
Isotope Evolution, Reference Fit, Pattern Maker) keep their own Save
buttons.

## 5. Shortcut catalog

`shortcuts.hpp` / `shortcuts.cpp`:

- Removed: `OpenQueue`, `SaveQueue`, `NewScript`, `SaveScript`,
  `SaveConditionals`.
- Added: `FileNew` "New…", `FileOpen` "Open…", `FileSave` "Save", in new `ShortcutContext::FileMenu` ("Experiment
  window and editors"). Not `Everywhere`: the four are live only where a
  window registered them, and `RecallNext` (data browser) shares Ctrl+N.
  For the clash test `FileMenu` overlaps `ExperimentWindow`, `ScriptEditor`,
  `Everywhere` and itself; not `DataBrowser`.
- `ShortcutContext::ConditionalsEditor` has no entries left: removed.
- Catalog is static (no user rebinding), so nothing stored is orphaned.
- Header comment example `key(Shortcut::SaveQueue)` updated.

## 6. Rules that must hold

1. File shows exactly one item per role, in every bar, in both `Bars`
   modes, whatever windows exist.
2. A File item is enabled only if the window in front registered that role
   and its action is enabled.
3. A File item never acts on a window other than the one in front at the
   moment it is triggered.
4. Destroying a window or its action removes its registration; hub holds
   `QPointer`s only.
5. No two enabled actions share a File key in one window.

## 7. Tests

- `tests/ui/test_menu_hub.cpp`, both `Bars::Shared` and `PerWindow` where
  the mode matters:
  - File starts with the four items, once, with no window registered; all
    disabled, plain text.
  - Two windows register Save with different nouns: item text, enabled
    state and trigger follow the active window; triggering reaches only the
    active window's action.
  - Target disabled → item disabled; re-enabled → enabled (via `changed`).
  - Window in front with no registration → disabled, plain text.
  - Owner destroyed → role gone, item disabled.
  - Disabled item's key reaches another action bound to it in the window in
    front (the Ctrl+N case).
  - `commands()` lists the four once, under File.
- `tests/ui/test_experiment_window.cpp`: `new_queue` clean; modified +
  Cancel leaves queue and path; modified + Discard empties; running
  refused. Queue menu no longer has Open/Save/Save As; registered actions
  have no shortcut.
- `tests/ui/test_script_editor.cpp`: `open_picked` opens the picked script;
  picks an open one → switches, no second tab; cancel → nothing; Save
  action disabled with no tab.
- `tests/ui/test_conditionals_editor.cpp`: `open_picked` opens; with
  unsaved changes and Cancel stays; New through the action.
- `tests/ui/test_shortcuts.cpp`: new ids and keys; removed ids gone.
- `tests/ui/test_command_palette.cpp`: adjust for the moved commands.

## 8. Docs

- `docs/dev_setup.md`: "Queue > Open" → "File > Open".
- `menu_hub.hpp` header comment: File's fixed items and proxy rule; example
  "Save queue / Save script" reworded.
- No user guide names these menus today (checked by grep); none added.

## 9. Out of scope

- Store-backed windows answering File > Save.
- Save As for scripts and conditionals.
- Recent files, File > Close, File > Revert.
- User-rebindable shortcuts.
- Any change to Rows, Executor, View, Window, Help menus.
