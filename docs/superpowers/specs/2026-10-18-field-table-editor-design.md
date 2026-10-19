# Field table editor (T095)

Row T095: "need a way to edit mftable via a UI". Field table (pychron's
mftable) = per-detector magnet value for each isotope, fit over mass. Today
pychron-cpp has model, versioned store and peak-center update, no way for
operator to see or change table but text editor.

## 1. What exists

### 1.1 Legacy pychron (`~/Programming/pychron`)

- File: `setupfiles/spectrometer/mftables/<name>.csv`; active name in hidden
  file `mftable_name` or spectrometer `.cfg` `[Magnet] mftable`. Optional first
  line = fit name (`linear|parabolic|cubic`), else discrete; header
  `iso,<det>,...`; one axial DAC per detector, no mass column (mass from
  `molweights`), blank cell = no value. `ic_mftable.csv` swapped in by
  `MFTableCTX` for peak hops.
- Fit: per detector `leastsq` of `polyval`, DAC against mass, degree 1-3;
  inverse by `brentq` on [0, 200].
- History: pref `use_local_mftable_archive` → every write copied into git repo
  `spectrometer_dir/<name>_mftable_archive` and committed. Backup copy before
  "Populate MFTable".
- Spectrometer menu: Populate MFTable (peak center every detector,
  `update_others=False`), Reload MFTable, Edit MF Table…, Local MFTable
  History…, Define Peak Center… (type det, isotope, DAC → `update_field_table`).
- Edit MF Table… (`mftable_view.py`, `spectrometer_actions.py:241`): beta
  warning; `TableEditor`, isotope column read-only, one `%0.5f` column per
  detector; no add/remove row, no plot, no validation. OK writes
  `<path>.temp` and archives it: live file never replaced. Edit did nothing
  operator could use.
- History view (`local_mftable_history_view.py`): commit list; two selected →
  text panes plus per-cell numeric difference table (only when headers
  match); one selected → checkout over file ("revert").
- Peak center: `delta = new - old` at reference isotope on reference
  detector, added to every row of every detector (or reference detector only
  without `update_others`), refit, write, archive.
- Reload: md5 of file per lookup (`_make_hash` returns hash object, so always
  "changed": reloaded every lookup).
- Table never stored with analysis; per-analysis `peakcenter` JSON only.

Lessons kept: grid by isotope × detector, blank cell allowed, history with
numeric diff and restore, manual "set this isotope here" with propagate,
Populate-by-peak-center idea (out of scope, §9). Lessons dropped: temp-file
edit, git per lab, per-lookup reload.

### 1.2 pychron-cpp

- Model `pychron::spectrometer::FieldTable`
  (`libs/systems/include/pychron/systems/spectrometer/field_table.hpp`): points
  `ControlPoint{isotope, mass, values}`, `FitKind`
  `Discrete|Linear|Quadratic|Cubic`, `TableAxis Dac|Field|Mass`, per-detector
  fit overrides, `value_for`, `mass_for`, `update(det, iso, value,
  propagate)`. Fit refuses too few points, repeated masses. TOML via
  `parse_field_table` / `to_toml`.
- Store `FieldTableStore` (`field_table_store.hpp`):
  `<data_root>/tables/<name>/<version>.toml` immutable, `current` pointer,
  `save(table, when)` → new version `YYYYMMDDTHHMMSSZ[-N]`, `versions()`,
  `current_version()`, `load()`, `restore(version)` (moves pointer only).
  Atomic write (tmp + rename).
- Assembler path: `cfg::load_table`, `resolve_current_table`
  (`data_dir.cpp`), `check_field_tables` (`config_validate.cpp`: table found,
  axis = positioner `native_axis`, column for every active detector).
- `Spectrometer` (`spectrometer.hpp`): `tables_` map, `active_table()`,
  `field_table()` copy, `update_table(...)` in memory, `save_table(when)`,
  `with_table(name)` RAII scope, lazy load of named table from store.
- Peak center (`libs/systems/src/jobs/peak_center.cpp:438`): `update_table`
  (default true) calls `Spectrometer::update_table`.
- UI: QWidgets. `SpectrometerBridge` copies active table once at
  construction (isotope lists, mass on reference). `SpectrometerWindow` has no
  table view. Model editor: `ConditionalsEditorWindow` (file list, table
  model, diagnostics block save, canonical TOML, unsaved-change prompt,
  unparseable file shown read-only).
- `read_mftable_csv` (`mftable_csv.hpp`) exists, no caller but tests.
- Spectrometer-control spec (`2026-09-29-spectrometer-control-design.md`)
  §9.3 plans `elctl spec table [show|import|restore]`, §9.4 "table selector"
  on magnet panel.

### 1.3 Defects found (fixed here, §6)

- D1 pointer format. Shipped `current` files hold file name
  (`2026-09-30T000000.toml`); `resolve_current_table` expects that.
  `FieldTableStore::current_version` rejects `.` (`valid_version`) and
  `set_current` writes bare stem. So `Spectrometer`'s lazy `store.load()`
  fails on every shipped table, and after any `FieldTableStore::save`,
  next start's `resolve_current_table` reports "points at missing version".
- D2 peak center never persists. Nothing calls `Spectrometer::save_table`;
  `docs/user/03-spectrometer.md` ("`update_table = true` writes a new
  version") is false. Update lives until process exits.
- D3 `SpectrometerBridge` table copy goes stale after peak center update.
- D4 run record `field_table_version` holds table name
  (`SpectrometerState::field_table`), not version. Noted, out of scope (§9).

## 2. Goal

Operator opens field table of this line, sees it as grid and plot, edits
values, rows, fits; saves new version that magnet uses from next move;
browses versions, compares two, restores one. Same operations from `elctl`
for scripted use and for line without UI.

## 3. Window: `FieldTableEditorWindow` (`apps/pychron-ui`)

Opened from `Spectrometer > Field Table…`: one item, main window's,
app-wide, so in every window's bar (spectrometer window's too); new
`MenuHub::Menu::Spectrometer`, hidden while empty. One window per app (second open raises
it). Needs spectrometer data root; without one (no spectrometer configured)
menu item disabled with tooltip giving reason.

Layout:

- Left: table names = directories under `<data_root>/tables` with a
  `current` file. Active table marked "(active)", config `hv_table` marked
  "(HV)". Under it, version list of selected table, newest first, current
  marked; columns: version, points, detectors.
- Centre top: grid of open version. Rows = points, sorted by mass. Columns:
  `Isotope`, `Mass`, one per detector. Detector columns = config detectors in
  config order, then any detector only in table (shown, header marked "not in
  config"). Empty cell = no value for that detector.
- Centre bottom: plot of selected detector column: points (value vs mass),
  fit curve over mass range ±`FieldTable::kInverseMargin`, residual
  (value − fit) per point in tooltip and in grid cell tooltip. Discrete
  fit: points only. Colours from `plot_style()` / `plot_series_color(slot)`,
  restyles on `PlotStyleHub::changed()` (apps/pychron-ui AGENTS.md).
- Right: table default fit (combo), per-detector fit override (combo per
  detector, "default" = none), axis (read-only label), diagnostics list
  (click → cell).
- Toolbar: Add Row, Remove Row, Set Value… , Save, Revert, Compare…,
  Make Current, Import CSV….

### 3.1 Editing rules

- Opening a version loads it from store. Opening table that spectrometer
  holds in memory with content different from store's current version
  (unsaved in-memory change; after D2 fix only on save failure) loads the
  in-memory one and shows banner "in-memory table differs from saved
  version <v>": saving makes it a version. In-memory table loaded as one
  undoable edit over saved version (Undo shows saved one). Conflict Reload
  and reload after `fieldTableChanged` open store's version as saved, no
  in-memory check.
- Value cells: finite double, shown `%0.5f`, typed text kept to full
  precision. Clearing cell removes detector value from that point.
- Isotope cell: text; on commit, mass filled from `MolecularWeights` when
  known and mass cell empty or equal to old isotope's weight. Mass cell
  editable (finite, > 0).
- Add Row: isotope picker from `MolecularWeights` names plus free text;
  mass from weights; values empty.
- Remove Row: selected rows, no prompt (undo by Revert).
- Set Value…: detector, isotope, value, `Propagate` checkbox (default
  `[magnet] propagate`). Runs `FieldTable::update` on edit copy: same
  semantics as peak center. Legacy "Define Peak Center…".
- Axis not editable. Changing axis = different table; out of scope.
- No detector column add/remove: column set fixed by config ∪ table. A
  detector absent from config keeps its values unless cleared by hand.
- Undo stack per open version (`QUndoStack`), Ctrl+Z / Ctrl+Shift+Z.

### 3.2 Diagnostics

Recomputed on each edit, 300 ms debounce. Errors block Save; warnings do not.

Errors:
- isotope empty or duplicated;
- mass not finite or ≤ 0;
- value not finite;
- per detector: fit fails (too few points for degree, repeated masses):
  message from `FieldTable` fit;
- active detector in config with no value in any point (same rule as
  `check_field_tables`) for table named by `[magnet] field_table` or
  `hv_table`.

Warnings:
- fit not monotonic over detector's mass range (then `mass_for` may pick
  wrong root): checked at 200 evenly spaced masses;
- residual |value − fit| > 1 % of detector's value span;
- detector column not in config;
- isotope not in `MolecularWeights`, or mass differs from weight by > 0.01 amu.

Diagnostics function lives in `libs/systems` (pure, testable), not UI:
`check_field_table(const FieldTable&, const cfg::SpectrometerConfig&, const MolecularWeights&, std::string_view name) -> TableDiagnostics` (`field_table_check.hpp`; `has_error`, `first_error`, `to_string(TableDiagnostic)` → `"error: row 3/H1: <message>"`, row 1-based, location left out when none).

### 3.3 Save

- Save writes new version through `Spectrometer::replace_table(name, table,
  base_version, when)` (§5) when spectrometer present, else
  `FieldTableStore::save`. `when` = `clock.wall_now()` (`CoreBridge::wall_now()`).
- Conflict: window remembers version it opened (`base`). Save refused when
  store's current version ≠ `base` (peak center or `elctl` saved meanwhile):
  dialog "table changed since you opened it (now <v>)" with Reload (discard
  edits, open current), Save Anyway (new version from edits, becomes
  current), Cancel.
- Save refused while executor runs a queue or a scan (peak center, sweep) is
  running on this spectrometer: Save disabled, tooltip gives reason. Table
  must not change between hops of one run. UI state is convenience only:
  refusal itself is in core (`replace_table` returns error while a
  `TableHold` is out, §5), so a race between enable check and Save, or a
  caller other than this window, still refused. Error shown in dialog.
- Saved TOML is canonical `to_toml`: comments of hand-written version lost.
  First save over version whose file text ≠ `to_toml(parse(text))` asks once
  ("comments in <v> are not kept"). Old version file untouched (immutable).
- Save disabled ("no changes") while grid has no edit.
- Unsaved edits: closing window, switching version or table asks Save /
  Discard / Cancel (as `ConditionalsEditorWindow::Unsaved`).
- Version that does not parse: listed, shown with parse error, read-only;
  can still be compared as text.

### 3.4 Versions: compare, make current

- Compare…: two versions of same table (selected in list, or open vs
  current). Grid of differences by (isotope, detector): old, new, Δ; rows
  only in one side marked added / removed; fit and override changes listed
  above. Matched by isotope name, not row index (legacy matched by header
  and row; failed when detectors differed).
- Make Current: `FieldTableStore::restore(version)` through
  `Spectrometer::restore_table_version(name, version)` (§5). Same refusals as
  Save (running queue or scan, conflict). Also refused when chosen version
  has a §3.2 error (diagnostics run against current config; version that does
  not parse counts as error): Make Current disabled, tooltip lists first
  error; core refuses too (§5). Confirm dialog naming old and new version.
- Revert: drop edits, reload open version.
- No delete of versions (immutable history; a lab prunes by hand).

### 3.5 Import CSV

Import CSV…: pick legacy `mftable.csv`, `read_mftable_csv(text, weights,
table axis)`; result opens as unsaved edit of selected table (base = its
current), saved by Save. Reader must accept legacy's first line holding one
fit name (`linear|parabolic|cubic`, other → discrete) before `iso,` header;
today it expects `#,<fit>,...` row after header. Checked against a real lab
file (Drive setupfiles survey) in plan.

## 4. `elctl spec table`

Spectrometer-control spec §9.3, now specified:

```
elctl spec table list                          # tables, current version, active marker
elctl spec table show [<name>] [--version V]   # grid as text; default active table, current
elctl spec table versions <name>
elctl spec table diff <name> <v1> [<v2>]       # v2 default current; same matching as §3.4
elctl spec table set <name> <det> <isotope> <value> [--propagate|--no-propagate]
elctl spec table restore <name> <version>
elctl spec table import <name> <mftable.csv>
elctl spec table check [<name>]                # §3.2 diagnostics; exit 1 on error
```

`set` and `import` write new version (same diagnostics; errors refuse with
exit 1). `restore` runs §3.2 diagnostics on target version first (unparseable
= error); any error refuses with exit 1, pointer unchanged, errors printed. Run against data root of config, no device opened, through
`FieldTableStore` only. No cross-process lock exists between `elctl` and a
running `pychron-ui` (none found in `apps/elctl` or `libs/experiment`), and
none is added: a running process keeps the table it loaded until it reloads
(restart, or editor's Reload). Overlap is caught where it matters: editor's
Save sees current ≠ `base` (§3.3), and editor rereads version list when its
window is activated. `elctl spec table` output says "takes effect when a
process next loads the table".

## 5. Core changes (`libs/systems`)

- `Spectrometer::replace_table(const std::string& name, FieldTable table,
  std::string_view base_version, WallTime when) -> Result<std::string>`:
  under `mutex_`: refuse while any `TableHold` out (error "table is held by
  <holder>"); check axis = positioner axis; check `check_field_table` has no
  error; check store current = `base_version` unless `base_version` empty
  (Save Anyway passes empty); `FieldTableStore::save`; then swap
  `tables_[name]` if loaded. Returns new version. Move in progress holds
  `mutex_`, so move sees old or new table, never half.
- `Spectrometer::restore_table_version(name, version) -> Result<void>`:
  under `mutex_`: same hold refusal; load version from store, refuse on parse
  failure or any `check_field_table` error (error lists them); then
  `FieldTableStore::restore`, then reload `tables_[name]` if loaded.
- `Spectrometer::hold_table(std::string holder) -> TableHold`: RAII, move-only;
  holds (id, holder name), in order taken, under own plain `std::mutex
  holds_mutex_`, not `mutex_`: few fields, never held across clock time, so
  `table_held()` from UI thread never waits behind settling move;
  `replace_table` and `restore_table_version` take `mutex_`, then
  `holds_mutex_`, never other order; `table_held() ->
  std::optional<std::string>` (first holder) for UI enable state. Taken by
  `Executor` for whole queue run (`libs/experiment` already links
  `pychron::systems`) and by peak center and sweep jobs for their run. Hold
  blocks only `replace_table` and `restore_table_version`; `update_table` and
  `save_table` (peak center's D2 path) do not check it, so peak center inside
  a held queue still updates and saves.
- `Spectrometer::table_version(name) -> Result<std::string>`: store current.
- Event `FieldTableChanged{name, version}` on `SignalBus` after either, and
  after peak center save (D2). `SpectrometerBridge` relays it, re-reads its
  table copy (D3), emits `fieldTableChanged`; editor window open on that
  table and unmodified reloads, modified shows conflict banner.
- `check_field_table` (§3.2), `diff_field_tables(a, b) -> TableDiff`
  (§3.4), both pure, in `field_table.hpp` or new `field_table_check.hpp`.

## 6. Defect fixes

- D1: one pointer convention: `current` holds file name `<version>.toml`
  (what shipped files and `resolve_current_table` use). `set_current`
  writes `<version>.toml`; `current_version` strips one trailing `.toml`
  then applies `valid_version`; bare stem still read (files written by
  current code). Shipped profiles unchanged, so
  `EveryInstalledPlanPutsItsIsotopesOnItsDetectors` and install e2e
  unaffected (libs/setup AGENTS.md).
- D2: peak center with `update_table` true, after successful
  `Spectrometer::update_table`, calls `save_table(clock.wall_now())`. Save
  failure: peak center result still success, warning logged and in result's
  notes, in-memory table kept (§3.1 banner then shows it). Doc sentence
  becomes true.
- D3: §5 event.

## 7. Rules that must hold

- No version file ever overwritten or deleted by pychron.
- Magnet never positioned from table that failed §3.2 errors: Save,
  `set`, `import` refuse; `restore` of version with errors refused too.
- In a running process, table changes only between moves (`mutex_`), and
  editor never changes it while queue or scan runs: enforced in core by
  `TableHold` refusal in `replace_table` and `restore_table_version`, not by
  UI. Peak center's own D2 save is exempt: it runs as part of the queue,
  between runs, as today.
  `elctl` writes only files; it never changes a running process's table.
- Time stamps from `Clock::wall_now()`, never `system_clock::now()`.
- UI talks to core only through `SpectrometerBridge` / `CoreBridge`; save
  and restore run on bridge's executor thread, result posted back.
- Editor writes no file outside `<data_root>/tables/<name>/`. Not a user
  file: no `mark_as_user_file`.

## 8. Tests

- `tests/systems` (or where `FieldTableStore` tests are): D1 round-trip
  (save → `resolve_current_table` finds it; shipped-style pointer → `load()`
  works); `replace_table` conflict, axis refusal, diagnostics-error refusal,
  in-memory swap; `restore_table_version` reload; `replace_table` and
  `restore_table_version` each refused while a `TableHold` out (store
  current and in-memory table unchanged), accepted after hold released;
  `save_table` not refused under hold; `restore_table_version` refused for
  version with §3.2 error and for unparseable version, pointer unchanged;
  executor holds table for queue run (`tests/integration`
  `LabSessionTest`: executor tests under `tests/experiment` have no real
  `Spectrometer`; `table_held()` `"queue"` at each `RunStarted`, clear
  after); `check_field_table` each error and
  warning; `diff_field_tables` by isotope with added/removed rows.
- Peak center: with `update_table`, new version exists after job, stamped
  from `VirtualClock`; save failure (read-only dir) leaves job success with
  warning.
- `read_mftable_csv`: legacy first-line fit name form.
- `elctl`: each `spec table` subcommand on temp data root; `set` / `import`
  refused on diagnostics error, exit 1; `restore` to version with
  diagnostics error refused, exit 1, `current` file unchanged.
- `tests/ui`: editor opens shipped qtegra table, edit cell, Save → new
  version current and `SpectrometerBridge` sees it; duplicate isotope
  disables Save; conflict dialog path via injected chooser (as
  `ConditionalsEditorWindow::set_pick_file`); unsaved prompt; Save and Make
  Current disabled while `table_held()`; Make Current disabled on version
  with §3.2 error.

## 9. Out of scope

- Populate-all-detectors by peak center (legacy Populate MFTable) and
  auto-generated tables (`AutoMFTable`, `ICMFTableGenerator`): own row.
- Run record storing table version, not name (D4): touches record format
  (`libs/experiment` AGENTS.md); own row.
- Editing axis, renaming or creating tables from scratch, deleting
  versions.
- Magnet panel "table selector" (§9.4 of spectrometer-control spec):
  choosing active table stays config's `[magnet] field_table`.
- Git or database archive of tables.
- Python importer `tools/pychron_spectrometer_import.py`.

## Open questions

None.
