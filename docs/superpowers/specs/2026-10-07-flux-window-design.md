# Flux window

Date: 2026-10-07
Status: Implemented (owner decisions in section 3; implementation notes in section 11)
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
presets, Save. A new `Fit` menu. Qt-free additions the window draws from, in
`libs/processing` and its store adapter (section 4).
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
the fit's degrees of freedom by one; Save reports `Saved 12 positions (0 unchanged)`, and
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
| W6 | **Where the options of a level come from.** A level with a saved fit opens on the saved options and the preset bar shows `(saved fit)`. A level without one opens on the preset in use (`Default` to begin with). Choosing a preset replaces the model and error options with the preset's (fields it does not set go to their defaults); it does not merge. |
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
  std::optional<double> j, j_err;       // absent for NotReduced and for a NoJ that has no J (see R3)
};
```

`fit_level` already computes each J to form the mean; it now keeps it. An
analysis omitted by more than one rule reports the first of: edit, saved
fit, tag. `omitted` keeps its meaning and is what a save writes
(`is_omitted`, flux spec R15). An analysis whose J the weighted mean refuses
(it has no error) keeps its J and has state `NoJ`; only an F that gives no J
leaves `j` absent (section 11, R3). `elctl flux fit` gains one warning line per
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
ScenePtr flux_scene(const LevelInputs& inputs, const LevelFit& fit, const FluxSceneOptions& options = {});
ScenePtr flux_scene(const LevelInputs& inputs, const FluxOptions& options, const Edits& edits,
                    const FluxSceneOptions& scene_options = {});   // no fit: analyses and means, the highlight too
```

One graph, one panel. The abscissa of a hole is its angle in degrees,
`atan2(x, y)` about the origin of the holder as legacy has it
(`flux_visualization_editor.py:729-741`), or its x or y.

Layers, in drawing order:

1. **The fit band and line.** The model evaluated by `reduction::fit_flux`
   on the used monitors at 361 points: round a circle whose radius is the
   mean radius of the used monitors (Angle), or along the axis from the
   least to the greatest coordinate of any position (X, Y). A `BandLayer` at
   +/- 1 sigma and a `LineLayer`. Present for Plane, Bowl, LeastSquares1D and
   the mean kinds; absent for Matching, NearestNeighbors, Bracketing and
   Bracketing1D, which have no curve.
2. **Analyses.** One `PointLayer`, a small marker per monitor analysis with a
   J, `refs[i].analysis` its uuid, spread within its hole over +/- 2 degrees
   (or 2 % of the axis range) in record-id order so they do not sit on each
   other, `excluded[i]` when it takes no part. No error bars (R5). Tooltip:
   the record id, `J +/- err`, and for one that is out, why (for an analysis
   whose J the weighted mean refuses: `not used: J has no error`).
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

### 4.4 Level status (`flux_store.hpp`)

```cpp
enum class LevelFluxStatus { NoMonitors, NotFitted, Fitted };
LevelFluxStatus level_flux_status(const persistence::LevelSheet& sheet, std::string_view monitor_sample);
```

From the level sheet alone: `NoMonitors` when no position carries the
monitor sample; `Fitted` when every monitor position has a J; else
`NotFitted`. The sample is that of the monitor document's default set.
It lives in the store adapter because it names a persistence type.

### 4.5 Shared text (`flux_view.hpp`)

The window shows the lines `elctl flux fit` prints and writes its CSV, and
cannot link `apps/elctl`. The functions that build them move from
`apps/elctl/src/flux.cpp` into `libs/processing` unchanged in output:
`flux_model_line`, `flux_summary`, `flux_warnings`, `flux_j_text`,
`csv_field`, `flux_csv_header`, `flux_csv_rows`. `elctl` calls them. (As
built, one part of the output did change: the warnings name each analysis that
is out on a line of its own with the reason, `hole 3: 66003-02 omitted (tag
outlier)`, where `elctl` printed `rejected <id>` and `analysis not reduced`.)

### 4.6 Ticking a monitor back in (`flux_fit.hpp`)

`Edits` gains `std::set<int> include_positions`: a monitor in it is fitted
even when the saved fit excluded it, as `Edits::include` does for an
omitted analysis. Without it the window could untick a monitor's `Fit` box
but never tick back one the saved fit had left out, short of forgetting
every saved omission.

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
level; when the window is on that level already, or is reading it, it is
raised as it is, with its edits (R18).

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
menu is the view's own: Include / exclude and Recall when the right-click is
on an analysis point (Recall is listed there with or without a recall
callback, and does nothing without one), then Reset view, Copy image, Save as
PNG…, Save as PDF…. The view keeps its zoom across a refit.

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
  `omitted (here)`, `not reduced`, `no J`. `Use` is not checkable for the last
  two; the tooltip of `not reduced` is the reduction's error (or `Not
  reduced`), of `no J` it is `No J`.
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
- `PresetBar` for kind `flux`. A level with a saved fit has `(saved fit)` as
  the first entry, for as long as it is on show: choosing a preset leaves it
  in the list, and choosing it applies the saved options again (R20).
- `OptionsEditor` on `flux_options_schema()`, sections `Model` and `Errors`.

**Tool bar.** `Save`, `Revert`, `Reload`, `Reset omissions`, `Export CSV…`,
`Open in Packages`.

- `Revert` drops the edits and returns to the options the level was loaded
  with, without touching the store.
- `Reload` reads the level again and puts back what is pending (R19): the
  `Edits`, the options when they were changed, the unticked `Save` boxes and
  the chosen monitor group. It asks nothing. The edits are validated against
  the level as read before it is fitted: one that names an analysis record
  that is no longer a monitor's, or a hole that is no longer a monitor, is
  dropped, and the status says `2 edits no longer apply` until the next
  change. What `edited()` compares against is the level as read now, so the
  window still reads edited. While a save runs, Reload does nothing.
- `Reset omissions` sets `Edits::reset_omits` and clears the edits made
  here: what the saved fit omitted and excluded is forgotten, tags still
  apply (flux spec 6.2).
- `Export CSV…` writes the CSV of `elctl flux fit --csv` for this level and
  passes it through `mark_as_user_file`.
- `Open in Packages` shows the Packages window on this level.

**Status bar.** One label, `flux_status_line` of the fit (R1), which starts
with the model as `plane, unweighted` or `plane, weighted` (R6):
`plane, unweighted · fit MSWD 1.12 (5 dof) · J 1.0012e-03 – 1.0241e-03 (2.24 %)`,
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
empty (but after a Reload, which puts back what was pending: R19), and the
level is fitted at once (R7).

The window answers the bridge's `changed()` only while it is on screen (R17):
it reads the tree, and the level too when nothing is edited (with edits, or
while a save runs, the status ends `· level changed elsewhere, Reload to see
it`). Hidden, it only notes that something changed, and does the same once
when it is next shown. `FitActions` keeps the window after it is closed, so
without this every save of the session would queue a tree read and a level's
reduction on the shared worker.

A fit after an edit or an option change runs `fit_level` on the GUI thread
150 ms after the last change; the fit on arrival does not wait (R7). A
level with no monitors loads like any other and shows `fit_level`'s error
with the level on screen (R6). When a fit fails, the status shows the error unreworded in the error tone
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
  UTC since this level was loaded. Reload and fit again.`, in the error tone
  (R12). The author and time come from the conflicting head
  (`flux_head_info`, R2).
  Nothing was written and the edits are kept. `Reload` is the way on: it
  reads the level as it is now and puts the edits back on it (R19), and Save
  then compares against the new heads.
- A refused J, or any other error: `Not saved: <the error>`.

Save boxes unticked stay unticked across the reload that follows a save of
the same level, and are no edit (R11); they stay so over every later read of
that level (a change elsewhere, also one that supersedes the reload after the
save; a Reload), until another level is picked or `Revert` (R20). The actor is
the bridge's; it is never asked for.

### 5.6 Edits pending

The window has edits pending when `Edits`, the options, the monitor group or
a `Save` box differ from what the load produced. A monitor-group choice that
differs from the level's as-saved selection is an edit, also after its reload;
`Revert` then reloads with no selection (R9). Selecting another level, a
change of the monitor group and closing the window then ask
`Save the flux of NM-300 A?` with Save, Discard and Cancel, through an
injectable function as the entry windows do. Reload does not ask: it leaves
nothing behind (R19). Nor does opening the level already on show (R18). Save
that fails or conflicts cancels what was asked. Discard on closing drops the
edits (R10). A change of the monitor group is not blocked by a message (R13):
the Save answer of the question saves the fit on show, then loads the new
group, which is then an edit against the selection that was saved, and no
edit once it is chosen back. Another level selected while a save runs is not
asked about either, since the edits are the ones being saved: the move
follows the save, and is dropped when the save fails or conflicts.

### 5.7 `PackagesWindow`

One more tool bar action, `Fit flux…`, enabled when a level is open, and a
signal `flux_requested(irradiation, level)`. One more change (R14): it did
not, as first written here, already reload its open level on the bridge's
`changed()`; it now re-reads it on another window's change, only when nothing
in it is unsaved, and keeps its selection.

## 6. Error handling

Every message is inline, in the status label; modal boxes only for the
three-way question of 5.6 and for a file that cannot be written. The core's
error text is shown as it is. No holder, a holder without geometry, or a
position beyond its holder is a load error and names the cause (the tables
and plot are empty). A level with no monitors loads, and shows `fit_level`'s
error with the level on screen (R6). The Recall item is in the plot's menu
when the right-click is on an analysis point, and does nothing when the
application gave no recall callback.

## 7. Files

| Path | Change |
|---|---|
| `libs/processing/include/pychron/processing/flux_fit.hpp`, `src/flux_fit.cpp` | `AnalysisState`; `UsedAnalysis` gains `tag`, `state`, `j`, `j_err` (4.1); `Edits::include_positions` (4.6) |
| `libs/processing/include/pychron/processing/flux_view.hpp`, `src/flux_view.cpp` | new: schema and conversions (4.2), `flux_abscissa`, `flux_scene` (4.3), the shared text (4.5) |
| `libs/processing/include/pychron/processing/options.hpp` | only the comment on `enabled_when` |
| `libs/processing/adapters/store/include/pychron/processing/flux_store.hpp`, `src/flux_store.cpp` | `level_flux_status` (4.4) |
| `apps/elctl/src/flux.cpp`, `flux.hpp` | one warning line per analysis out, with the reason; the text functions of 4.5 move out |
| `apps/pychron-ui/src/flux_window.{hpp,cpp}` | new |
| `apps/pychron-ui/src/flux_monitor_model.{hpp,cpp}`, `flux_analysis_model.{hpp,cpp}`, `flux_unknown_model.{hpp,cpp}` | new |
| `apps/pychron-ui/src/fit_actions.{hpp,cpp}` | new |
| `apps/pychron-ui/src/menu_hub.{hpp,cpp}` | `Menu::Fit` |
| `apps/pychron-ui/src/options_editor.cpp` | evaluates the `in` form of `enabled_when` |
| `apps/pychron-ui/src/packages_window.{hpp,cpp}` | `Fit flux…`, `flux_requested` |
| `apps/pychron-ui/src/entry_actions.{hpp,cpp}` | `bridge()` accessor; forwards `flux_requested` |
| `apps/pychron-ui/src/main.cpp` | `FitActions` in both start-up paths |
| `apps/pychron-ui/CMakeLists.txt` | the new sources, in the store block |
| `tests/processing/test_flux_view.cpp`, `test_flux_fit.cpp`, `test_flux_store.cpp` | new and extended |
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
  curve (361 points) present for the five kinds that have one and absent for
  the four that do not; analysis points without error bars; the curve at a monitor's abscissa equals that monitor's
  predicted J for Plane on the ring; the highlight layer; a scene without a
  fit has analyses and means only.
- `level_flux_status` for the three cases (`tests/processing/test_flux_store.cpp`).

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
  second Save says nothing is to be saved. Teardown with a save or a load in
  flight, and a close while busy, are run under the sanitizers.
- A head moved by another client: `Not saved: hole 7 was saved by ... UTC`,
  nothing written, the edits kept.
- `Save` unticked on a position leaves its head alone.
- Selecting another level with edits pending asks once; Cancel stays, Discard
  moves, Save saves then moves.
- Changing the monitor set reloads and the monitor-set combo's tooltip names the set's age.
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

## 11. Implementation notes (2026-10-07)

What the built window does that the text above did not say, or said
otherwise. The sections above were amended to agree; the rulings are numbered
for the commit messages and the review.

### Rulings

- **R1** `flux_status_line(const LevelFit&)` in `flux_view.hpp` is the
  window's status text (the model, then the MSWD and the J range). It is not
  `flux_summary`, which is the command line's.
- **R2** `flux_head_info` in the store adapter gives the author and time of
  the head that conflicted; `FluxSaveOutcome::conflict_hole` names the hole.
  The window reads both in the save's own job.
- **R3** An analysis whose J the weighted mean refuses (it has no error) keeps
  its J and has state `NoJ`; only an F that gives no J leaves it absent. Its
  plot tooltip says `not used: J has no error`.
- **R4** The curve has 361 points, not 181.
- **R5** Analysis points have no error bars; the means and the unknowns do.
- **R6** The status starts `plane, unweighted · ...`. A level with no
  monitors loads, and shows `fit_level`'s error with the level on screen.
- **R7** The fit on arrival is immediate; the 150 ms debounce is for edits
  and option changes.
- **R8** `SceneView` click and rubber-band hit-testing consider only points
  that carry an analysis uuid, so a click reaches the analysis under a mean.
  This is a change to the view shared by every figure window.
- **R9** A monitor-group choice that differs from the level's as-saved
  selection is an edit, also after its reload. `Revert` then reloads with no
  selection.
- **R10** Closing with Discard drops the edits. Preset messages show in the
  status label, after the fit's line.
- **R11** `Save` boxes unticked stay unticked across the reload after a save
  of the same level, and are no edit.
- **R12** The conflict line says `UTC` after the time.
- **R13** There is no blocking message on a change of the monitor group: the
  Save answer of that question saves the fit on show, then loads the new
  group. A direct Save with a sample typed takes the group-change path.
- **R14** The Packages window re-reads its open level on another window's
  change only when nothing in it is unsaved (grid, dose table, level fields),
  and keeps its selection. Section 5.7 was wrong that it already did.
- **R15** While a fit fails, the monitor `Fit` boxes still show each
  monitor's state from the edits and stay changeable, so a monitor unticked
  one too many can be ticked back without `Revert`; `N` and the mean columns
  stay filled and only the predicted columns are blank.
- **R16** The table columns are sized to their contents once per window:
  when a table first has rows, and again when the first predictions arrive.
  Never after the user dragged one, and not again for another level (one with
  longer identifiers needs a drag).
- **R17** A flux window that is not on screen reads nothing on the bridge's
  `changed()`: it sets a stale flag, and when it is next shown it reads the
  tree, and the level when one was open and nothing is edited. Hidden with
  edits pending it keeps them, and shows `· level changed elsewhere, Reload
  to see it` when shown.
- **R18** `open_level` for the level already on show, or being read, does
  nothing (the caller raises the window): no question, no read. A level that
  could not be read is asked for again.
- **R19** Reload reads the level again and puts back what is pending: the
  `Edits`, the options when they were changed, the unticked `Save` boxes and
  the chosen monitor group. It asks nothing. An edit that names an analysis
  record or a hole the level as read has no place for is dropped before the
  fit (`fit_level` is never asked about it), and the status says `<n> edits
  no longer apply`. The state `edited()` compares against is the level's as
  read now. `Revert` is the way to drop edits. The reload on a change
  elsewhere when nothing is edited is as it was. While a save runs Reload does
  nothing: what is pending is being saved, and the save reads the level again
  when it wrote.
- **R20** `(saved fit)` stays in the preset list while the level has a saved
  fit, also once a preset was chosen; choosing it applies the saved options.
  The `Save` boxes a save left unticked are kept, as no edit, over every read
  of the same level, and are ticked again by another level and by `Revert`.

### As built

- `Edits::include_positions` (4.6) is as specified. The window puts a hole in
  `exclude_positions` or `include_positions` by what the saved fit would do
  with it, so an edit undone leaves nothing behind.
- The shared text functions of 4.5, plus `flux_status_line` (R1).
- `evaluate_position()` in `flux_fit.hpp` is shared by `fit_level` and by the
  scene with no fit (the analyses as `fit_level` counts them).
- `tests/processing/flux_level_inputs.hpp` builds `LevelInputs` without a
  store; `seed_second_flux_level` in `flux_seed.hpp` adds a second level.
- `PresetBar::set_pinned_item` holds the `(saved fit)` entry; it stays over
  `select()` and `reload()`, and deleting the preset selected beside it falls
  back to it. `PresetBar::show_name` shows a preset as the one in use without
  loading it (after a Reload that kept the options).
- What a read of the level on show keeps is `FluxWindow::start_load`'s
  argument: nothing (another level, Revert), the `Save` boxes that are no
  edit (a change elsewhere, after a save, a monitor-group change), or
  everything pending (Reload). The kept state stays in the window's own
  members while the level is read, so a read that is superseded or fails
  loses none of it.
- After a save that wrote, the baseline a chosen monitor group is compared
  with is the selection of the fit that was saved.
- The no-fit `flux_scene` takes `FluxSceneOptions` too: the selected monitor
  is drawn on top while the fit fails.
- The Packages window asks its positions grid, as it asks its dose table,
  whether a cell is being edited.
- The `in` form of `enabled_when` is evaluated in `options_editor.cpp`, not in
  `options.cpp`.
- `FitActions` teardown: it deletes the window on `EntryActions::closing()`
  (before the bridge goes), and its destructor drains the bridge's worker
  before the source it owns is destroyed.
- Export uses `QSaveFile`, so a file is whole or as it was; then
  `mark_as_user_file`.
- The Recall item of the plot's context menu (listed on an analysis point)
  is a no-op without a callback.
- The options-schema test of `enabled_when` lives in
  `tests/ui/test_data_windows.cpp`.

- The Packages window, after Save Chronology, reads its chronology back so
  it is clean again; it compares doses by value; and it asks its dose table
  (not application focus) whether a cell is being edited.

### Known limits

- A sample typed in the Monitors group and not yet entered is dropped when
  Save is answered for another purpose, and by Reload.
- The tree's status word uses the default monitor set's sample, not the
  level's own.
- The whole tree is read again on every change notification that reaches a
  window on screen.
- Closing the window while a save runs still asks about the edits being
  saved; only a level switch is queued behind the save unasked.
- Nothing was run on PostgreSQL or with gcc.
- A Save click with a sample typed and not yet entered reloads the level
  under that sample and saves nothing, without saying so.
