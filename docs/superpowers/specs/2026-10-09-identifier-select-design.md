# Identifier select in run factory

Date: 2026-10-09
Status: Approved 2026-10-09; plan `../plans/2026-10-09-identifier-select.md`
Owner: Jake Ross
Depends on: `2026-10-02-experiment-window-design.md` (section 5.6, run factory
panel), `2026-10-04-sample-irradiation-entry-design.md` (sections 5.1 entry
reads, 9 `EntryBridge`).
Scope: run factory panel's Identifier field also offers store identifiers in
dropdown, narrowed by optional Package and Level selects.
Out of scope: type-ahead or search over whole store; filter by project,
sample or principal investigator; remembering Package/Level across sessions
or in queue file; showing J or analysis counts; any new store query, index or
migration; identifier pickers elsewhere (conditionals editor, data browser).

## 1. Goal

Operator building queue today types identifier from memory or from level
sheet. Wanted: pick it from the package and level being run. Typing stays:
specials (`bu`, `a`), identifiers not in store, install with no store.

Success: with `--db`, operator picks package `NM-293`, level `A`, opens
Identifier dropdown, sees that level's identifiers with sample names, picks
one; form and preview behave exactly as if it had been typed. Without `--db`
panel looks and works as before.

## 2. Behaviour

Run group rows, in order: Type, Package, Level, Identifier, Aliquot, Step.

- Package: `QComboBox`, object name `package`. First item `(none)`, then
  packages in the order `IStore::irradiations()` returns them (newest
  first), both kinds (`irradiation`, `package`). Item text: package name.
- Level: `QComboBox`, object name `level`. First item `(all)`, then levels of
  chosen package by name. Disabled while Package is `(none)`. Choosing
  another package resets it to `(all)`.
- Identifier: editable `QComboBox`, `InsertPolicy::NoInsert`, no completer.
  Its line edit keeps object name `identifier`, placeholder text, and every
  reaction it has today. Combo object name: `identifier_select`.
- Dropdown content:
  - Package `(none)`: empty.
  - Package chosen, Level `(all)`: every position of every level of package
    that holds an identifier, ordered by level name, then position.
  - Package and Level chosen: that level's, by position.
  - Position without identifier: skipped.
- Item text: `<identifier>  <sample>  (<level> <position>)`, e.g.
  `66001  FC-2  (A 3)`. Sample empty: `<identifier>  (<level> <position>)`.
  Item data (`Qt::UserRole`): identifier alone.
- Picking item puts item data, not item text, into line edit, then runs the
  path a keystroke runs (`on_identifier_changed()`): type reclassified, lab
  defaults applied on type change, preview refreshed.
- Typed text never changes Package or Level. Text matching no item is
  accepted as today.
- `set_form()` (Defaults, load from selection, auto-increment after Add) sets
  line edit text only. Package, Level and dropdown content stay.
- Package, Level and dropdown are disabled while panel is locked (queue
  running), with the rest of Run group.
- Package and Level are view state of panel: not in `FactoryForm`, not in
  queue, not in `QSettings`.

No source (section 4), or source's package list fails on first load: Package
and Level rows hidden, Identifier dropdown empty. Panel then equals today's.

## 3. Interface

New header `apps/pychron-ui/src/identifier_source.hpp`. No store or
persistence header included; builds with `PYCHRON_PERSISTENCE=OFF`.

```cpp
namespace pychron::ui {

struct PackageChoice {
  std::string id;    // opaque to the panel (the store's uuid as text)
  std::string name;
  friend bool operator==(const PackageChoice&, const PackageChoice&) = default;
};

struct IdentifierChoice {
  std::string identifier;
  std::string sample;  // empty: position has no sample
  std::string level;
  int position = 0;
  friend bool operator==(const IdentifierChoice&, const IdentifierChoice&) = default;
};

struct PackageContents {
  std::vector<std::string> levels;         // every level of the package, by name
  std::vector<IdentifierChoice> choices;   // by level name, then position
  friend bool operator==(const PackageContents&, const PackageContents&) = default;
};

class IdentifierSource : public QObject {
  Q_OBJECT
 public:
  using QObject::QObject;
  // Both answer on the GUI thread, later, and not at all once `receiver` is gone.
  virtual void packages(QObject* receiver, std::function<void(Result<std::vector<PackageChoice>>)> done) = 0;
  virtual void contents(QObject* receiver, const std::string& package,
                        std::function<void(Result<PackageContents>)> done) = 0;
 Q_SIGNALS:
  void changed();  // the catalog changed: what was listed may be stale
};

}
```

Level narrowing is done by panel on `PackageContents::choices`
(`choice.level == chosen`); choosing level asks source nothing.

`RunFactoryPanel`:

```cpp
// Null: no selects (the default). The panel does not own it and holds it by
// QPointer; a source destroyed first hides the selects.
void set_identifier_source(IdentifierSource* source);
// For tests.
QStringList package_choices() const;
QStringList level_choices() const;
QStringList identifier_choices() const;  // item texts
```

Constructor unchanged. `ExperimentWindow::factory()->set_identifier_source()`
is the way in. `MainWindow::set_identifier_source(IdentifierSource*)` keeps
pointer (`QPointer`) and hands it to factory panel when experiment window is
made, or at once when it exists already.

## 4. Store-backed source

`apps/pychron-ui/src/store_identifier_source.{hpp,cpp}`, in the
`PYCHRON_UI_HAS_STORE` source list of `apps/pychron-ui/CMakeLists.txt`.

```cpp
class StoreIdentifierSource : public IdentifierSource {
 public:
  // Child of `entry`; uses the bridge `entry` owns.
  explicit StoreIdentifierSource(EntryActions& entry);
};
```

- Bridge opened on first `packages()` call, not at construction: an operator
  who never opens experiment window opens no second store connection.
- `EntryActions` gains `Result<void> open_bridge()`: opens bridge on first
  use, shows nothing, returns the error. `ensure_bridge()` keeps its
  signature: calls `open_bridge()` and shows the message box on failure.
  Source uses `open_bridge()` and reports the error through `done`, queued,
  never before the call returns. A later Entry menu use tries again and
  shows the box.
- `packages()`: `EntryBridge::run` of `IStore::irradiations()`, mapped to
  `PackageChoice{to_string(uuid), name}`.
- `contents(package)`: one worker job: `IStore::levels(uuid)`, then
  `IStore::level_sheet(level.uuid)` for each level in the order returned.
  `PositionRow` with `identifier` set becomes `IdentifierChoice{*identifier,
  sample_name, level.name, position}`. First failing read fails the whole
  job. `package` that does not parse as uuid: `Result` error, no store call.
- `changed()` re-emitted from `EntryBridge::changed`, connected when bridge
  is opened.
- Wired in `main.cpp` beside `EntryActions` (only when `cli->db`):
  `window.set_identifier_source(new StoreIdentifierSource(*entry))`.

Rule: source and panel write nothing to store.

## 5. Loading and staleness

- `set_identifier_source(non-null)`: panel asks `packages()` once, shows
  Package and Level rows when answer arrives without error.
- Package chosen: panel clears dropdown and Level, asks `contents()`.
- Each request carries a serial number held by panel. An answer whose serial
  is not the latest issued for its kind (packages, contents) is dropped. So a
  slow answer for package A arriving after B was chosen changes nothing.
- `changed()`: panel asks `packages()` again; chosen package is kept when its
  `id` is still listed (else `(none)`), and its `contents()` asked again;
  chosen level kept when its name is still listed (else `(all)`). Line edit
  text untouched.
- Failed `packages()` after a successful first load: list kept as it was,
  error text in Package combo tooltip. Failed `contents()`: dropdown and
  Level empty, error text in Identifier combo tooltip. Tooltip cleared by
  next success. Nothing modal.
- GUI thread never waits on store.

## 6. Cost

Reads used: `kIrradiations` once per load of package list, then per chosen
package `levels` (1 statement) and `level_sheet` per level (position rows
plus references, a handful each). Package of 20 levels: order of 100
statements, once per choice, on worker thread.

`kIrradiations` counts per row in subqueries (AGENTS.md, "Queries": not
measured on large store). It is not rewritten here. Measured 2026-10-09 with
`sqlite3` on a migrated copy of a lab's imported store (102 packages, 963
levels, 18998 positions): package list 8 ms; position rows of largest package
(NM-335, 22 levels, 868 positions) 0.15 s. Same position read on that store
before migration 0005 (no index on `ref_object.position_uuid`): 1.4 s.

## 7. Tests

`tests/ui/test_run_factory_panel.cpp`, with `FakeIdentifierSource` (answers
held until test releases them, so order of answers is test's choice):

- no source: Package and Level rows hidden; existing tests unchanged (they
  find line edit by name `identifier`).
- source set: packages listed after `(none)`, in source order; dropdown
  empty.
- package chosen: levels listed after `(all)`; dropdown holds all levels'
  identifiers in level, position order; item text format, with and without
  sample.
- level chosen: dropdown narrowed; no new request made to source.
- pick item: `form().identifier` is identifier alone; picking special-typed
  or monitor identifier reclassifies type as typing does; preview updated.
- typed special (`a`) still switches type with package chosen; Package and
  Level unchanged.
- `set_form()` leaves Package, Level, dropdown as they were.
- stale answer: choose A, choose B, release B's answer, then A's: dropdown is
  B's.
- `changed()`: lists reloaded; chosen package and level kept when still
  there, reset when gone; line edit text kept.
- `packages()` fails first time: rows stay hidden. `contents()` fails:
  dropdown empty, tooltip holds error.
- locked: three widgets disabled.
- source deleted: rows hidden, no crash on later `changed`-driven paths.

`tests/ui/test_entry_windows.cpp` (store builds only, SQLite fixture):
`StoreIdentifierSource` lists fixture's packages; `contents()` gives levels
by name and identifiers by level then position, skips position without
identifier; bad package id is an error; `open_bridge` on unopenable url shows
no dialog and source reports error.

No thread added (worker is `EntryBridge`'s): no TSan run required by this
change.

## 8. Docs

- `docs/user/04-experiments.md`: run factory section, Package / Level /
  Identifier select, that it needs `--db`.
- `2026-10-02-experiment-window-design.md` section 5.6: Run line names the
  selects and points here.
