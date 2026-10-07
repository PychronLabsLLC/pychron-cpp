# Flux window

Date: 2026-10-07
Status: Draft for owner review
Owner: Jake Ross
Depends on: `2026-10-06-flux-fitting-design.md` (the math of section 5, the
orchestration of section 6, `elctl flux` of section 7, the rulings of
section 12; this window is the one its section 11 reserves),
`2026-10-02-data-browsing-visualization-design.md` (`Scene` and `SceneView`,
options schemas, `OptionsEditor`, `PresetBar`, `PresetStore`),
`2026-10-04-sample-irradiation-entry-design.md` (the store worker thread of
`EntryBridge`, the Packages window).
Scope: a window in `apps/pychron-ui` that fits and saves the J of one
irradiation level: a level tree, a J plot with the individual monitor
analyses, a monitor table and an unknown table, an options dock with
presets, Save. A new `Fit` menu. Three Qt-free additions to
`libs/processing` that the window draws from.
Out of scope: a 2-D map of J over the tray, vertical flux (J against level
height), a history pane with restore, an editor for monitor sets, Monte
Carlo and position error, moving the other fit windows under the `Fit`
menu. See section 10.

Legacy citations are `file:line` relative to the `pychron/` package of the
legacy Python repo (read-only), at commit `26e77ad17`.

## 1. Goal

The review step of a flux fit is done by eye: which analysis of a monitor is
off, which monitor disagrees with the rest, whether the model follows the
tray. `elctl flux fit` makes that a loop of flags and reruns. The window
makes it one screen: every toggle refits at once, the plot shows what
changed, and Save writes what `elctl flux fit --save` would.

Success: with a store holding level `NM-300 A` (FC-2 monitors in eight
holes), `Fit ▸ Flux…` opens the window, selecting `NM-300 ▸ A` fills both
tables and the plot within a second or two; clicking an outlying analysis in
the plot hollows its point, unticks it in the analyses table, and moves that
monitor's mean and every predicted J; unticking a monitor's `Fit` box drops
the fit's degrees of freedom by one; Save reports `saved 12 positions`, and
the Packages window's J column shows the new values without being reopened.

## 2. What legacy does (summary of the survey)

`FluxResultsEditor` (`pipeline/editors/flux_results_editor.py`) on
`BaseFluxVisualizationEditor` (`pipeline/editors/flux_visualization_editor.py`),
reached by `Fit ▸ Flux` (`pipeline/engine.py:1052`,
`pipeline/tasks/actions.py:226-228`).

| Area | Legacy behaviour | Kept, changed or dropped |
|---|---|---|
| Entry | a pipeline template: pick irradiation and level in a dialog, then an options dialog, then the editor, then "resume" to save (`pipeline/pipeline_defaults.py:347-357`) | changed: one window; the level is chosen in a tree and the options live in a dock |
| Monitors table | Use, Save, Save Pred., hole, identifier, sample, N, saved / mean / predicted J with errors and percent, MSWD, dev, position error (`flux_results_editor.py:86-121`) | kept, less "Save Pred." and position error (flux spec F5, F8) |
| Unknowns table | Save, hole, identifier, sample, saved model, saved and predicted J, dev (`:428-453`) | kept, less the saved model |
| Plot, 1D | J against hole angle `arctan2(x, y)`, the fit round a circle of the tray radius with an error envelope, individual analyses spread about each hole, means as diamonds (`flux_visualization_editor.py:709-851`); for 1-D models J against X or Y (`:621-707`) | kept |
| Point omission | select analyses on the scatter to toggle a temporary omit; means and fit recompute (`flux_results_editor.py:133-164`, `:325-350`) | kept; the selection indexes the wrong analyses once a monitor is unticked (`:231-233` against `flux_visualization_editor.py:615-619`), fixed by construction: points carry the analysis uuid |
| Plot, 2D and Grid | a contour of the model over the tray with slices; one small plot per monitor (`flux_visualization_editor.py:419-613`, `:878-1007`) | dropped from this spec (section 10) |
| Recalculate | a Calculate button; toggling Use refits at once (`flux_visualization_editor.py:212-217`) | changed: everything refits at once; no button |
| Summary | min J, max J, delta J % (`:323-328`) | kept, in the status bar |
| Options | a modal dialog before the editor (`options/views/flux_views.py`) | changed: a dock, schema-driven, with presets |
| Export | "Monitor Fluxes CSV" (`flux_results_editor.py:363-383`) | kept: the CSV of `elctl flux fit --csv` |
| Bracket columns | editable Bracket A / B that do nothing (`:510-535`) | dropped |

## 3. Decisions

Owner decisions of 2026-10-07 are marked (owner).

| # | Decision |
|---|---|
| W1 | (owner) **A fit window only.** One level at a time; two tables, a J plot, options, Save. |
| W2 | (owner) **No 2-D J map** in this spec. |
| W3 | (owner) **Options are schema-driven with presets**: a flux options schema, the existing `OptionsEditor` and `PresetBar`, preset kind `flux`. |
| W4 | (owner) **`Fit ▸ Flux…`**, a new top-level `Fit` menu, and a `Fit flux…` action in the Packages window that opens the flux window on the level being edited. |
| W5 | **Live refit.** Every edit and every option change refits (debounced 150 ms). `fit_level` is pure and takes well under a millisecond for a level; the slow part, loading and reducing the monitor analyses, happens once per level. There is no Fit button. |
| W6 | **Where the options of a level come from.** A level with a saved fit opens on the saved options and the preset bar shows `(saved fit)`. A level without one opens on the preset in use (`Default` to begin with). Choosing a preset applies it over the current options. |
| W7 | **The monitor selection is not in a preset.** The monitor set, the sample override and "all positions" belong to the level, not to a style of fitting; they sit in their own group above the options and come from the level's saved fit (flux spec R18, R20). |
| W8 | **The window computes nothing.** The scene, the options schema and the level status are Qt-free functions in `libs/processing`, tested there. The window holds `LevelInputs`, `FluxOptions`, `Edits` and the `LevelFit`, and draws. |
| W9 | **One edit, three places.** A plot click, an analysis check box and a rubber band are the same change to `Edits`; a monitor's `Fit` box is `Edits::exclude_positions`. Each view is redrawn from the refit, never from the gesture. |
| W10 | **The store is never called from the GUI thread.** Load, save and the monitor sets run as `EntryBridge` jobs. The fit itself runs on the GUI thread (W5). |
| W11 | **Saving is `save_level`**, unchanged: one changeset, every position or none, compare-and-swap, the refusal of a J that is not finite and positive. The window adds nothing to its rules. |
| W12 | **The fit result says why an analysis is out.** `FittedPosition::analyses` gains each analysis's J and the reason it takes no part. The window needs it for the plot and the analyses table; `elctl flux fit` prints it too. |

## 4. Core additions (`libs/processing`, Qt-free)

### 4.1 Per-analysis J and reason (`flux_fit.hpp`)

```cpp
enum class AnalysisState { Used, OmittedByTag, OmittedBySavedFit, OmittedByEdit, NotReduced, NoJ };

struct FittedPosition::UsedAnalysis {
  std::string uuid, record_id, tag;
  bool omitted = false;                 // as today: omitted by rule (tag, saved fit, edit)
  AnalysisState state = AnalysisState::Used;
  std::optional<double> j, j_err;       // absent for NotReduced and NoJ
};
```

`fit_level` already computes each J to form the mean; it now keeps it. An
analysis omitted by more than one rule reports the first of: edit, saved
fit, tag. `omitted` keeps its meaning and is what a save writes
(`is_omitted`, flux spec R15). `elctl flux fit` gains one warning line per
monitor with analyses out, `hole 3: 66003-02 omitted (tag outlier)`.

### 4.2 Options schema (`flux_view.hpp`)

```cpp
SchemaPtr flux_options_schema();                         // kind "flux", version 1
Options to_options(const FluxOptions& options);
Result<FluxOptions> flux_options_from(const Options& options);
```

| Key | Section | Type, choices | Default | Enabled when |
|---|---|---|---|---|
| `model.kind` | Model | enum: `plane`, `bowl`, `weighted-mean`, `matching`, `nearest`, `bracketing`, `ls1d`, `mean1d`, `bracketing1d` | `plane` | |
| `model.weighted` | Model | bool | false | `model.kind in plane\|bowl\|ls1d` |
| `model.neighbors` | Model | int, 1..64 | 2 | `model.kind == nearest` |
| `model.interpolation` | Model | enum: `weighted`, `average`, `linear` | `weighted` | `model.kind == bracketing` |
| `model.axis` | Model | enum: `x`, `y` | `x` | `model.kind in ls1d\|mean1d\|bracketing1d` |
| `model.degree` | Model | int, 1..4 | 1 | `model.kind == ls1d` |
| `mean.kind` | Errors | enum: `arithmetic`, `weighted` | `arithmetic` | |
| `mean.error` | Errors | enum: `sem`, `msem`, `sd` | `msem` | |
| `fit.error` | Errors | enum: `sem`, `msem`, `sd` | `msem` | `model.kind in plane\|bowl\|weighted-mean\|ls1d\|mean1d` |

The choices are the spellings of `elctl flux fit`. Each field's `help` is
the sentence of `docs/flux.md` for that option. `flux_options_from` returns
the error of the math layer when `fit.error` is `sd` with a fitted surface
("sd is not an error kind of a fitted surface"); the editor shows it on the
field. Factory presets: `Default` (the defaults above) and `Weighted plane`
(`model.weighted = true`).

`FieldSpec::enabled_when` today reads `<key> == <value>`, `<key> != <value>`
or `<key>`. It gains one form, `<key> in <v1>|<v2>|...`, evaluated where the
others are; `OptionsEditor` needs no other change.

### 4.3 The scene (`flux_view.hpp`)

```cpp
enum class FluxAbscissa { Angle, X, Y };
FluxAbscissa flux_abscissa(const FluxOptions& options);   // X or Y for the three 1-D kinds, else Angle

struct FluxSceneOptions { std::optional<int> highlight_hole; };
Scene flux_scene(const LevelInputs& inputs, const LevelFit& fit, const FluxSceneOptions& options = {});
Scene flux_scene(const LevelInputs& inputs, const FluxOptions& options);   // no fit: analyses only
```

One graph, one panel. The abscissa of a hole is its angle in degrees,
`atan2(x, y)` about the origin of the holder as legacy has it
(`flux_visualization_editor.py:729-741`), or its x or y.

Layers, in drawing order:

1. **The fit band and line.** The model evaluated by `reduction::fit_flux`
   on the used monitors at 181 points: round a circle whose radius is the
   mean radius of the used monitors (Angle), or along the axis from the
   least to the greatest coordinate of any position (X, Y). A `BandLayer` at
   +/- 1 sigma and a `LineLayer`. Present for Plane, Bowl, LeastSquares1D and
   the mean kinds; absent for Matching, NearestNeighbors, Bracketing and
   Bracketing1D, which have no curve.
2. **Analyses.** One `PointLayer`, a small marker per monitor analysis with a
   J, `refs[i].analysis` its uuid, spread within its hole over +/- 2 degrees
   (or 2 % of the axis range) in record-id order so they do not sit on each
   other, `y_err` its error, `excluded[i]` when it takes no part. Tooltip:
   the record id, `J +/- err`, and for one that is out, why.
3. **Monitor means.** A `PointLayer` of diamonds with error bars, one per
   monitor with a mean; a monitor left out of the fit is drawn excluded.
   Tooltip: hole, identifier, n, mean J, MSWD.
4. **Predicted J of the unknowns.** A `PointLayer` of a third marker, with
   error bars. Tooltip: hole, identifier, predicted J, dev %.
5. **The highlighted hole**, when given: its analyses and mean drawn again
   in the accent of the scene style.

The y axis is J; the title of the x axis is `Hole angle (degrees)`, `X` or
`Y`. Without a fit (the fit failed) layers 2 and 3 are still drawn, so the
data stays in view under the error.

### 4.4 Level status (`flux_view.hpp`)

```cpp
enum class LevelFluxStatus { NoMonitors, NotFitted, Fitted };
LevelFluxStatus level_flux_status(const persistence::LevelSheet& sheet, std::string_view monitor_sample);
```

From the level sheet alone: `NoMonitors` when no position carries the
monitor sample; `Fitted` when every monitor position has a J; else
`NotFitted`. The sample is that of the monitor document's default set.
This lives in the store adapter's header (`flux_store.hpp`), since it names
a persistence type.

## 5. UI design (`apps/pychron-ui`)

All of it in the store-gated block of `apps/pychron-ui/CMakeLists.txt`, under
`PYCHRON_UI_HAS_STORE`.

### 5.1 `FitActions`

The owner, as `EntryActions` is for the entry windows. Constructed with the
store URL and an `EntryBridge&` (the one `EntryActions` owns, so that a
flux save reaches the Packages window through `notify_changed()`), and an
optional callback to open a recall window. It opens its own `StoreSource`
on the URL for `load_level`, declared before the window so it outlives it.
A store that cannot be opened is a message box, as in `EntryActions`.

It contributes `Flux…` to `MenuHub` under `Menu::Fit`, `Scope::App`. The
window is made on first use, kept in a `QPointer`, and shown with `show();
raise(); activateWindow()`. `open_flux(irradiation, level)` shows it on that
level.

`main.cpp` creates `FitActions` beside `EntryActions` in both start-up paths
and connects the Packages window's request to `open_flux`.

### 5.2 `MenuHub`

A new `Menu::Fit`, appended to the enum so existing slots keep their values
(as `Entry` was), `kMenus` 10, ordered between `Entry` and `Window`. It
exists only when something has contributed to it, which happens only with a
store. The command palette lists `Fit › Flux` and the Window menu lists the
open flux window with no further work.

### 5.3 `FluxWindow`

A `QMainWindow`, 1400 x 820, titled `Flux` and, with a level loaded,
`Flux — NM-300 A`. Constructor: `(EntryBridge& bridge, processing::IAnalysisSource&
source, processing::PresetStore& presets, QWidget* parent)`; all three must
outlive the window.

**Left: the level tree.** Irradiations, each with its levels, newest
irradiation first. A level shows its status in a second column in the muted
tone: `no monitors`, `not fitted`, `fitted`. Selecting a level loads it. With
edits pending it asks first (5.6).

**Centre, above: the plot.** A `SceneView` showing `flux_scene`. A click on
an analysis, or a shift-drag over several, toggles them (W9). The context
menu is the view's own: Include / exclude, Recall (when a recall callback
was given), Reset view, Copy image, Save as PNG…, Save as PDF…. The view
keeps its zoom across a refit.

**Centre, below: the tables**, in a horizontal splitter.

- *Monitors* (`FluxMonitorModel`). Columns: `Fit` (check), `Save` (check),
  `Hole`, `Identifier`, `Sample`, `N`, `Saved J`, `±`, `Mean J`, `±`, `%`,
  `MSWD`, `Pred. J`, `±`, `%`, `Dev %`. `Fit` unticked puts the hole in
  `Edits::exclude_positions`; a monitor with no usable analysis shows `Fit`
  unticked and disabled, with the reason as its tooltip. A row is tinted
  with the theme's warning tint when its mean MSWD is outside its limits,
  and with the muted tint when it is out of the fit. Selecting a row
  highlights its hole in the plot and fills the analyses table.
- *Analyses of the selected monitor* (`FluxAnalysisModel`), under the
  monitors table. Columns: `Use` (check), `Record`, `Tag`, `J`, `±`, `State`.
  `State` is `used`, `omitted (tag outlier)`, `omitted (saved fit)`,
  `omitted (here)`, `not reduced`, `no J`. `Use` is disabled for the last
  two, with the reduction's error as the tooltip.
- *Unknowns* (`FluxUnknownModel`). Columns: `Save` (check), `Hole`,
  `Identifier`, `Sample`, `Saved J`, `±`, `Pred. J`, `±`, `%`, `Dev %`.

J is printed `%.4e`, percentages and MSWD `%.2f`, an absent value blank.
`Save` unticked puts the hole in the save's `skip_positions`. `Dev %` is
(saved - predicted) / predicted x 100.

**Right dock, "Fit".**

- *Monitors* group, a small form: `Monitor set` (a combo of the document's
  sets; its tooltip the set's sample, age and lambda_k), `Sample` (a line
  edit, placeholder the set's own sample), `All positions` (check). A change
  here reloads the level (asking first when edits are pending), since it
  changes which positions are monitors.
- `PresetBar` for kind `flux`.
- `OptionsEditor` on `flux_options_schema()`, sections `Model` and `Errors`.

**Tool bar.** `Save`, `Revert`, `Reload`, `Reset omissions`, `Export CSV…`,
`Open in Packages`.

- `Revert` drops the edits and returns to the options the level was loaded
  with, without touching the store.
- `Reload` reads the level again (asking first when edits are pending).
- `Reset omissions` sets `Edits::reset_omits` and clears the edits made
  here: what the saved fit omitted and excluded is forgotten, tags still
  apply (flux spec 6.2).
- `Export CSV…` writes the CSV of `elctl flux fit --csv` for this level and
  passes it through `mark_as_user_file`.
- `Open in Packages` shows the Packages window on this level.

**Status bar.** One label, the summary of the fit:
`plane, weighted · fit MSWD 1.12 (5 dof) · J 1.0012e-03 – 1.0241e-03 (2.24 %)`,
followed by `· 3 warnings` when there are any, the warnings one per line in
the tooltip: the lines `elctl flux fit` prints (rejected and unreduced
analyses, extrapolated holes, MSWD outside its limits, monitors left out,
the saved monitor set missing from the store, a saved fit that used SD).
With edits pending it ends `· edited (not saved)`.

### 5.4 Loading and fitting

Selecting a level (or changing the monitor group) runs one bridge job:
`load_monitor_sets` and `load_level` with the selection. While it runs the
status reads `Loading NM-300 A…` and the tool bar and tables are disabled.
A newer selection supersedes it: results carry a generation number and a
stale one is dropped. A load error is shown in the status in the error tone,
with the tables and plot empty.

On arrival: the options are resolved (W6), the monitor group is set from
`LevelInputs` (the set and sample in use, `all_positions`), `Edits` is
empty, and the level is fitted.

A fit runs `fit_level` on the GUI thread 150 ms after the last change. When
it fails, the status shows the error unreworded in the error tone
(`flux: bowl needs 6 monitor positions, 5 used`), the predicted columns are
blank, the plot shows the analyses and means without a curve, and Save is
disabled with the error as its tooltip.

### 5.5 Saving

Enabled when a fit exists and no job is running; otherwise its tooltip says
why. It runs `save_level(store, actor, fit, skip_positions, "pychron-ui
<version>")` as a bridge job, status `Saving…`.

- Written: `Saved 12 positions (0 unchanged)`; the level is loaded again,
  and `bridge.notify_changed()` makes the Packages window refresh its J
  columns.
- Nothing to write: `Nothing to save: 12 positions unchanged`.
- A conflict: `Not saved: hole 7 was saved by jsmith at 2026-10-07 14:02:11
  since this level was loaded. Reload and fit again.`, in the error tone.
  Nothing was written; the edits are kept so they can be applied again after
  a reload.
- A refused J, or any other error: `Not saved: <the error>`.

The actor is the bridge's; it is never asked for.

### 5.6 Edits pending

The window has edits pending when `Edits`, the options, the monitor group or
a `Save` box differ from what the load produced. Selecting another level,
Reload, a change of the monitor group and closing the window then ask
`Save the flux of NM-300 A?` with Save, Discard and Cancel, through an
injectable function as the entry windows do. Save that fails or conflicts
cancels what was asked.

### 5.7 `PackagesWindow`

One more tool bar action, `Fit flux…`, enabled when a level is open, and a
signal `flux_requested(irradiation, level)`. Nothing else changes: its J
columns already reload on the bridge's `changed()`.

## 6. Error handling

Every message is inline, in the status label; modal boxes only for the
three-way question of 5.6 and for a file that cannot be written. The core's
error text is shown as it is. A level with no monitors, no holder, or a
position beyond its holder is a load error and names the cause. The Recall
action is absent when the application gave no recall callback.

## 7. Files

| Path | Change |
|---|---|
| `libs/processing/include/pychron/processing/flux_fit.hpp`, `src/flux_fit.cpp` | `AnalysisState`; `UsedAnalysis` gains `tag`, `state`, `j`, `j_err` (4.1) |
| `libs/processing/include/pychron/processing/flux_view.hpp`, `src/flux_view.cpp` | new: schema and conversions (4.2), `flux_abscissa`, `flux_scene` (4.3) |
| `libs/processing/include/pychron/processing/options.hpp`, `src/options.cpp` | `enabled_when` accepts `<key> in a\|b` |
| `libs/processing/adapters/store/include/pychron/processing/flux_store.hpp`, `src/flux_store.cpp` | `level_flux_status` (4.4) |
| `apps/elctl/src/flux.cpp` | one warning line per monitor with analyses out, with the reason |
| `apps/pychron-ui/src/flux_window.{hpp,cpp}` | new |
| `apps/pychron-ui/src/flux_monitor_model.{hpp,cpp}`, `flux_analysis_model.{hpp,cpp}`, `flux_unknown_model.{hpp,cpp}` | new |
| `apps/pychron-ui/src/fit_actions.{hpp,cpp}` | new |
| `apps/pychron-ui/src/menu_hub.{hpp,cpp}` | `Menu::Fit` |
| `apps/pychron-ui/src/options_editor.cpp` | only if the `in` form is evaluated there rather than in `options.cpp` |
| `apps/pychron-ui/src/packages_window.{hpp,cpp}` | `Fit flux…`, `flux_requested` |
| `apps/pychron-ui/src/entry_actions.{hpp,cpp}` | `bridge()` accessor; forwards `flux_requested` |
| `apps/pychron-ui/src/main.cpp` | `FitActions` in both start-up paths |
| `apps/pychron-ui/CMakeLists.txt` | the new sources, in the store block |
| `tests/processing/test_flux_view.cpp`, `test_flux_fit.cpp`, `test_options.cpp` | new and extended |
| `tests/ui/test_flux_window.cpp`, `tests/ui/CMakeLists.txt` | new; `tests/processing` on the include path for `flux_seed.hpp` |
| `apps/elctl/tests/test_flux_cmd.cpp` | the reason line |
| `docs/flux.md`, `AGENTS.md` | the window; one sentence in the flux bullet |

## 8. Testing

Core (GoogleTest):

- `fit_level` reports each analysis's J and state: used, omitted by tag, by
  saved fit, by edit (and the precedence), not reduced, no J; `omitted` and
  what a save writes are unchanged.
- The schema: `to_options` then `flux_options_from` is the identity for
  every model and every option; the defaults are `FluxOptions{}`; `sd` with
  a surface is the math layer's error; each `enabled_when` is true exactly
  for the models of the table in 4.2; the `in` form with one, several and no
  matching value.
- `flux_scene`: the abscissa per model; one analysis point per analysis
  with a J, its uuid and its excluded flag; means and unknowns counted; the
  curve present for the five kinds that have one and absent for the four
  that do not; the curve at a monitor's abscissa equals that monitor's
  predicted J for Plane on the ring; the highlight layer; a scene without a
  fit has analyses and means only.
- `level_flux_status` for the three cases.

Window (`tests/ui/test_flux_window.cpp`, QtTest, offscreen, a temporary
SQLite store seeded with `flux_seed.hpp`; one `QSKIP` without the store):

- Opening a level fills the tree, both tables and the plot; the status is
  the fit summary.
- Unticking a monitor's `Fit` drops the degrees of freedom by one and the
  row shows it left out.
- A click on an analysis in the plot unticks it in the analyses table,
  changes that monitor's N and mean, and the status ends `edited (not
  saved)`; the same through the check box gives the same fit.
- Choosing the `Weighted plane` preset changes the status to `plane,
  weighted`; a level with a saved fit opens on the saved options and shows
  `(saved fit)`.
- A model the monitors cannot support shows the error, blanks the predicted
  columns, and disables Save with the error as its tooltip.
- Save writes (the store is read back), says `Saved 12 positions`, and a
  second Save says nothing is to be saved.
- A head moved by another client: `Not saved: hole 7 was saved by`, nothing
  written, the edits kept.
- `Save` unticked on a position leaves its head alone.
- Selecting another level with edits pending asks once; Cancel stays, Discard
  moves, Save saves then moves.
- Changing the monitor set reloads and the header names the set's age.
- `Reset omissions` forgets a saved exclusion.
- `Export CSV…` writes the file of `elctl flux fit --csv` for the level.
- `Fit ▸ Flux…` opens the window; the Packages window's `Fit flux…` opens it
  on that level; after a save the Packages grid shows the new J.
- The theme and shortcut guards still pass: no colour or key literal in the
  new sources.

## 9. Documentation

`docs/flux.md` gains a section "The flux window" before the command line:
what each region is, that every edit refits, what Save does, and that the
commands and the window write the same thing. `AGENTS.md`: one sentence in
the flux bullet (the window computes nothing; scene and schema live in
`libs/processing` `flux_view.hpp`).

## 10. Not in this spec

- **A 2-D map of J** over the tray (holes coloured by J or by residual, or a
  contour of the surface). `HolderView` draws the holes and handles
  selection but fills by category only; a scalar fill and a colour scale are
  its own piece of work.
- **Vertical flux** (J against level height across an irradiation) and the
  legacy flux visualization editor.
- **A history pane** with restore. `elctl flux history` lists revisions;
  restoring is moving heads back in the store.
- **An editor for monitor sets**: `elctl flux monitors` remains the way.
- **The per-monitor grid of small plots** of legacy.
- **Moving the isotope evolution and reference fit windows** under `Fit`:
  they act on a selection of analyses from the data browser, this window on
  a level.
- Monte Carlo and position error (flux spec F5).
