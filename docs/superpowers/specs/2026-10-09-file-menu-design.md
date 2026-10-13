# File menu: New, Open, Save, Save As in one place: design

Date: 2026-10-09. Status: approved 2026-10-09; implemented. Revised same day:
New and Open are submenus that work from any window (section 3.2), not
single items following the window in front.

## 1. Problem

- Same four commands were in three places under three names: Open, Save,
  Save As in **Queue**; New, Save in **Scripts** (script editor); Save
  Conditionals in **Scripts** (conditionals editor), its New a toolbar `+`
  only.
- File held only Installations, Preferences, Quit.
- No New queue; no Open command in script or conditionals editor.
- Opening a script meant: open Experiment window, open script editor from
  its menu, then pick. No way to say "open a script" from where you are.

## 2. Goal

File menu, every window, both `Bars` modes:

    New  ▸  New Queue / New Script… / New Conditionals…
    Open ▸  Open Queue… / Open Script… / Open Conditionals…
    Save
    Save As…
    ─────
    (contributed groups: Installations, Preferences; Quit)

- New and Open entries work from any window and decide which window comes
  to the front:

  | Entry | Brings up | Then |
  |---|---|---|
  | New Queue | Experiment window | empty queue, no file |
  | Open Queue… | Experiment window | queue file dialog |
  | New Script… | script editor | kind + name prompt |
  | Open Script… | script editor | pick one of lab's scripts |
  | New Conditionals… | conditionals editor | name prompt |
  | Open Conditionals… | conditionals editor | pick one of lab's files |

  Script and conditionals entries create the Experiment window if needed
  (the editors are its children) but do not show it.
- Save and Save As act on what the window in front edits and say so:
  "Save Queue", "Save Script", "Save Conditionals", "Save Queue As…". Greyed
  and plain ("Save", "Save As…") in a window that edits no file. Save As:
  Experiment window only.
- Keys: Ctrl+S is File > Save. Ctrl+N and Ctrl+O act on the window in
  front: New Queue / Open Queue in Experiment window, New Script / Open
  Script in script editor, New / Open Conditionals in conditionals editor,
  nothing elsewhere. Save As has no key.

## 3. Hub (`apps/pychron-ui/src/menu_hub.hpp`)

### 3.1 Save, Save As: proxies for the window in front

```cpp
enum class FileRole { Save, SaveAs };
void set_file_action(QWidget* owner, FileRole role, QAction* action, const QString& noun);
QAction* file_action(FileRole role) const;
```

- Hub owns one item per role, shared by every bar. Target = action
  registered for the role by a window whose `window()` is
  `active_window()`. Registering again for same owner and role replaces.
- Recomputed on `focusWindowChanged`, target's `QAction::changed`,
  registration, owner or target destroyed: enabled = target exists and is
  enabled; text = `&Save <noun>` / `Save <noun> &As…` with target, `&Save` /
  `Save &As…` without.
- Target is resolved again when the hub item is triggered, from the window
  then in front; nothing happens without one. (Palette or a menu left open
  can outlive the window the item was enabled for.)
- Registered window action carries no shortcut and sits in no menu.
- `Shortcut::FileSave` (`QKeySequence::Save`) on hub's Save. No key on Save
  As: `QKeySequence::SaveAs` is Ctrl+Shift+S, which is View > Spectrometer.

### 3.2 New, Open: submenus of contributed entries

```cpp
enum class FileList { New, Open };
struct FileEntry {
  QAction* action = nullptr;
  std::function<bool(const QWidget* window)> home;  // is `window` where the entry acts?
};
void contribute_file(QWidget* owner, FileList list, const QList<FileEntry>& entries);
QAction* file_list_action(FileList list) const;   // File > New, File > Open
QList<QAction*> file_entries(FileList list) const;
```

- Hub owns File > New and File > Open: one action each carrying one hub-owned
  `QMenu`, shared by every bar. Greyed while list has no entry.
- Entries: contribution order, for as long as `owner` lives. Not gated by
  active window; only entry's own enabled state greys it.
- Key: on each focus change, the first entry of a list whose `home(active)`
  is true gets the list's key (`Shortcut::FileNew`, `FileOpen`); every other
  entry of the list gets none. A contributed action sets no shortcut itself.
- A key that is on no entry is free for the window in front. This matters:
  `Shortcut::RecallNext` is Ctrl+N in data browser, where no New is at home.
- `commands()` (palette) lists, under `Menu::File`: New entries, Open
  entries, Save, Save As, then contributed File commands. The two submenu
  actions are not commands.

## 4. Windows

### 4.1 Main window (`main_window.cpp` `build_file_entries`)

- Contributes the six entries (it alone can create the Experiment window).
  `file_entries()` returns them in menu order.
- Enabled iff there is an experiment session (`set_experiment(bridge)`),
  same rule as View > Experiment.
- `ensure_experiment_window()` creates it on first use (loading the queue
  named at launch); `show_experiment_window()` also shows and raises. Queue
  entries use the second; script and conditionals entries use the first,
  then `open_script_editor()` / `open_conditionals_editor()`.
- `home`: queue entries, the Experiment window; script entries, its script
  editor; conditionals entries, its conditionals editor.
- Data-only main window (`data_main_window.cpp`) contributes none: New and
  Open greyed there.

### 4.2 Experiment window

- `save_`, `save_as_`: registered Save, SaveAs, noun `Queue`; not in
  `Menu::Queue`; no shortcut. `open_` kept as plain action for the toolbar.
- Public: `bool new_queue(QString* error = nullptr)`, `void new_dialog()`,
  `void open_dialog()`.
- `new_queue`: running → false, "a queue is running"; `resolve_unsaved()`
  false → false, "cancelled", queue untouched; else empty queue, no path,
  not modified. `new_dialog` shows the "A queue is running." box, as
  `open_dialog` does.
- Queue submenu keeps Revalidate.

### 4.3 Script editor

- Save: registered, noun `Script`; enabled only with a tab open. Close Tab,
  Check Now, Go to Gosub stay in Scripts with their keys.
- Public: `void new_dialog()` (kind + name prompt, then `new_script`),
  `bool open_picked()`, `set_pick_script(PickScript)`.
- `open_picked`: no scripts → false, says so in status line, asks nothing;
  pick cancelled → false; else opens or switches to the tab. Names are the
  tree's (`script_names()`: `kind/name`, `lib/name`). Default pick:
  `QInputDialog::getItem`, not editable. Not a file dialog: a script
  outside the lab's script directories is not a `ScriptFile`.

### 4.4 Conditionals editor

- Save: registered, noun `Conditionals`.
- Public: `void new_dialog()` (name prompt, then `new_file`; toolbar `+`
  calls it), `bool open_picked()`, `set_pick_file(PickFile)`.
- `open_picked`: no files → false, asks nothing; else existing `open(name)`,
  so the unsaved question applies.

### 4.5 Everything else

Registers nothing. Store-backed windows (Samples, Packages, Flux, Recall,
Isotope Evolution, Reference Fit, Pattern Maker) keep their own Save
buttons.

## 5. Shortcut catalog

- Removed: `OpenQueue`, `SaveQueue`, `NewScript`, `SaveScript`,
  `SaveConditionals`; `ShortcutContext::ConditionalsEditor`.
- Added: `FileNew` "New…", `FileOpen` "Open…", `FileSave` "Save", in
  `ShortcutContext::FileMenu` ("Experiment window and editors"). For the
  clash test (`overlap()`), `FileMenu` overlaps `ExperimentWindow`,
  `ScriptEditor`, `Everywhere` and itself; not `DataBrowser`.
- Catalog is static (no user rebinding), so nothing stored is orphaned.

## 6. Rules that must hold

1. File shows exactly one New, Open, Save, Save As, in every bar, in both
   `Bars` modes, whatever windows exist.
2. Save and Save As are enabled only if the window in front registered the
   role and its action is enabled, and never act on a window other than the
   one in front at the moment they are triggered.
3. A New or Open entry works from every window in which it is enabled.
4. At most one entry per list has the list's key at a time, and only while
   its home window is in front.
5. Destroying a window or its action removes what it gave; hub holds
   `QPointer`s only.

## 7. Tests

- `tests/ui/test_menu_hub.cpp`: `file_has_the_four_commands_once`,
  `new_and_open_list_entries_that_work_from_any_window` (both bar modes),
  `file_commands_follow_the_window_in_front` (both bar modes),
  `a_file_command_is_as_enabled_as_its_target`,
  `a_closed_windows_file_commands_go`,
  `a_file_key_with_no_entry_at_home_is_the_windows`,
  `the_command_palette_has_the_file_commands`.
- `tests/ui/test_experiment_window.cpp`: `aNewQueueIsEmptyAndHasNoFile`,
  `aQueueCannotBeReplacedByANewOneWhileItRuns`,
  `fileNewAndOpenBringUpTheWindowTheyActIn` (the six entries through the
  main window, and the keys by window in front).
- `tests/ui/test_script_editor.cpp`: `fileOpenPicksOneOfTheLabsScripts`.
- `tests/ui/test_conditionals_editor.cpp`:
  `windowFileOpenPicksOneOfTheLabsFiles`, empty lab in `windowEmptyLab`.
- `tests/ui/test_shortcuts.cpp`: catalog, `overlap()`, dialog groups.
- Not covered by a test: Open Queue…'s file dialog through the entry (the
  dialog has no hook); keys on the macOS shared bar (a parentless bar's
  keys fire only as the platform's global bar).

## 8. Docs

`docs/user/01-getting-started.md` (section 6 File, shortcut tables),
`04-experiments.md` (3.1, 7.6), `09-scripting.md` (section 6),
`docs/dev_setup.md`.

## 9. Out of scope

- Store-backed windows answering File > Save.
- Save As for scripts and conditionals.
- Recent files, File > Close, File > Revert.
- User-rebindable shortcuts.
- Open Queue… or New entries in the data-only application.
