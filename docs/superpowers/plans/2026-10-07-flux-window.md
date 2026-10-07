# Flux Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A window in `apps/pychron-ui` that fits and saves the J of one irradiation level, with every edit refitting at once.

**Architecture:** The window computes nothing. Qt-free functions in `libs/processing` give it the options schema, the plot scene, the status text, the warnings and the CSV (`flux_view.hpp`); `fit_level` reports each analysis's J and why it is out. The window holds `LevelInputs`, `FluxOptions`, `Edits` and the `LevelFit`, runs `load_level` and `save_level` as `EntryBridge` jobs, runs `fit_level` on the GUI thread, and redraws three table models and one `SceneView` from each fit. `FitActions` owns it and a `StoreSource`; `Fit ▸ Flux…` and the Packages window open it.

**Tech Stack:** C++20, Qt6 Widgets, QCustomPlot behind `SceneView`, GoogleTest (libs), QtTest offscreen (`tests/ui`), the DVC store on SQLite in tests.

**Spec:** `docs/superpowers/specs/2026-10-07-flux-window-design.md` (W1-W12, sections 4-8). It builds on `docs/superpowers/specs/2026-10-06-flux-fitting-design.md`; read both. `docs/flux.md` is the user guide of what exists.

## Global Constraints

- No Qt, nlohmann or persistence in `libs/processing` proper (`flux_fit.hpp`, `flux_view.hpp` stay pure); persistence types only in the store adapter (`flux_store.hpp`).
- The store is never called from the GUI thread (W10): `load_level`, `save_level`, `load_monitor_sets` run inside `EntryBridge::run` jobs. `fit_level` runs on the GUI thread.
- No colour literal (`QColor(0x..)`, `"#rrggbb"`, `Qt::red`) and no `QKeySequence(...)` literal in any new file under `apps/pychron-ui/src`: `tests/ui/test_theme.cpp` and `test_shortcuts.cpp` enforce it. Colours come from `theme()` or from the scene (`palette_color`).
- Every user-visible string in `tr()`, sentence case; an action that opens a dialog ends in `…`.
- New app sources are added by hand to the store-gated list in `apps/pychron-ui/CMakeLists.txt` (the `if(TARGET pychron_processing_store)` block); everything UI is under `#ifdef PYCHRON_UI_HAS_STORE`.
- Messages are inline in the status label; the core's error text is shown unreworded. Modal boxes only for the Save / Discard / Cancel question and a file that cannot be written.
- Saving is `save_level` unchanged (W11); the software string is `"pychron-ui " + <application version>`.
- A file written for the user goes through `pychron::mark_as_user_file`.
- A position's hole is `ordinal + 1` on the holder, never `hole_id`.
- Commits: Conventional Commits with the component scope. Branch `feat/flux-window` cut from `origin/develop`.
- Build and test: `cmake --preset dev-ui && cmake --build build/dev-ui -j 10 && ctest --preset dev-ui -j 8`; library-only tasks may iterate on `build/dev`. Never skip or disable a failing test. No `git stash`.

## Review Focus

Conditions the spec implies but does not spell out; each has a test in the task named.

1. **A save pressed inside the 150 ms debounce.** Edit, then Save at once. Expected: the pending refit runs first and what is saved is the fit of the edits on screen, never the previous fit. (Task 8)
2. **A stale load arriving late.** Select level A, then B before A has loaded. Expected: the window shows B; A's result is dropped whatever order they arrive in. (Task 6)
3. **The window closed, or the level changed, while a job runs.** Expected: no crash and no callback into a dead window or onto the wrong level. (Tasks 6, 8)
4. **The store changed from another window** (the Packages window saves positions; `EntryBridge::changed()` fires) while a level is loaded here. Expected: with no edits pending the level reloads; with edits pending nothing is discarded and the status says the level changed elsewhere. (Task 7)
5. **A preset or saved fit whose options cannot fit this level** (a Bowl preset on a ring of monitors; `sd` with a surface). Expected: the error in the status and on the field, the data still in view, Save disabled with the reason, and choosing another model recovers without a reload. (Task 7)

---

## File Structure

| File | Responsibility |
|---|---|
| `libs/processing/include/pychron/processing/flux_fit.hpp`, `src/flux_fit.cpp` (modify) | `AnalysisState`; each analysis's J and state in the fit result |
| `libs/processing/include/pychron/processing/flux_view.hpp`, `src/flux_view.cpp` (new) | Qt-free: options schema and conversions; `flux_scene`; summary, warnings, CSV text |
| `libs/processing/adapters/store/include/pychron/processing/flux_store.hpp`, `src/flux_store.cpp` (modify) | `level_flux_status` |
| `apps/elctl/src/flux.cpp`, `flux.hpp` (modify) | use the shared text functions; print the reason an analysis is out |
| `apps/pychron-ui/src/options_editor.cpp` (modify) | `enabled_when` form `<key> in a\|b` |
| `apps/pychron-ui/src/flux_monitor_model.{hpp,cpp}`, `flux_analysis_model.{hpp,cpp}`, `flux_unknown_model.{hpp,cpp}` (new) | the three table models |
| `apps/pychron-ui/src/flux_window.{hpp,cpp}` (new) | layout, wiring, jobs, status, prompts |
| `apps/pychron-ui/src/fit_actions.{hpp,cpp}` (new) | owner: source, window, menu action |
| `apps/pychron-ui/src/menu_hub.{hpp,cpp}`, `entry_actions.{hpp,cpp}`, `packages_window.{hpp,cpp}`, `main.cpp`, `CMakeLists.txt` (modify) | `Menu::Fit`; shared bridge; `Fit flux…`; wiring |
| `tests/processing/test_flux_view.cpp` (new), `test_flux_fit.cpp`, `test_flux_store.cpp` (modify) | core tests |
| `tests/ui/test_flux_models.cpp`, `test_flux_window.cpp` (new), `tests/ui/CMakeLists.txt` (modify) | UI tests |

---

### Task 1: Each analysis's J and why it is out

**Files:** modify `libs/processing/include/pychron/processing/flux_fit.hpp`, `src/flux_fit.cpp`, `apps/elctl/src/flux.cpp`; tests `tests/processing/test_flux_fit.cpp`, `apps/elctl/tests/test_flux_cmd.cpp`.

**Interfaces — produces** (namespace `pychron::processing`):

```cpp
enum class AnalysisState { Used, OmittedByTag, OmittedBySavedFit, OmittedByEdit, NotReduced, NoJ };
std::string_view to_string(AnalysisState) noexcept;   // "used", "omitted by tag", "omitted by saved fit", "omitted here", "not reduced", "no J"

struct FittedPosition::UsedAnalysis {
  std::string uuid, record_id, tag;
  bool omitted = false;                    // unchanged meaning: by rule (tag, saved fit, edit); what a save writes
  AnalysisState state = AnalysisState::Used;
  std::optional<double> j, j_err;          // absent for NotReduced and NoJ
  std::string reduction_error;             // NotReduced only
};
```

- [ ] **Step 1: Failing tests** in `test_flux_fit.cpp` (suite `FluxFitLevel`):

```cpp
TEST(FluxFitLevel, EachAnalysisCarriesItsJ)            // used analyses: j, j_err == reduction::j_of(f, monitor) to 1e-15 relative; state Used
TEST(FluxFitLevel, AnalysisStateSaysWhyItIsOut)        // tag "outlier" -> OmittedByTag; saved.omitted -> OmittedBySavedFit; Edits::omit -> OmittedByEdit;
                                                       // f == nullopt -> NotReduced with reduction_error; F = 0 -> NoJ; an omitted analysis with an F still has j
TEST(FluxFitLevel, StatePrecedenceIsEditThenSavedThenTag)   // an analysis omitted by all three reports OmittedByEdit; by saved + tag reports OmittedBySavedFit
TEST(FluxFitLevel, IncludeClearsTheState)              // tagged + in Edits::include -> Used, omitted false
TEST(FluxFitLevel, OmittedIsUnchangedByTheNewFields)   // for every analysis: omitted == (state is one of the three Omitted*)
```

and in `test_flux_cmd.cpp`:

```cpp
TEST_F(FluxCmd, AnAnalysisThatIsOutIsNamedWithItsReason)   // seed_tag one analysis "outlier", --omit another:
   // out contains "warning: hole 1: 66001-02 omitted (tag outlier)" and "warning: hole 2: 66002-01 omitted (here)"
```

- [ ] **Step 2: Run to verify failure** (compile: no `AnalysisState`).
- [ ] **Step 3: Implement.** `fit_level` already computes each J for the mean; keep it. Compute J also for omitted analyses that have an F (for the plot). CLI line, one per analysis out, after the existing warnings: `warning: hole <N>: <record id> omitted (tag <tag>)` | `omitted (saved fit)` | `omitted (here)` | `not reduced: <error>` | `no J`; the existing aggregate lines for rejected and unreduced analyses are replaced by these.
- [ ] **Step 4: Run.** `pychron_processing_tests --gtest_filter='FluxFitLevel.*'`, the elctl binary `--gtest_filter='FluxCmd*'`, then both whole binaries and `pychron_processing_store_tests`.
- [ ] **Step 5: Commit.** `feat(processing): a flux fit says each analysis's J and why it is out`

---

### Task 2: The summary, warnings and CSV as shared text

The window must show the lines `elctl flux fit` prints and write its CSV, and cannot link `apps/elctl`. Move them down.

**Files:** create `libs/processing/include/pychron/processing/flux_view.hpp`, `src/flux_view.cpp`, `tests/processing/test_flux_view.cpp`; modify `apps/elctl/src/flux.cpp`, `flux.hpp`, `apps/elctl/tests/test_flux_cmd.cpp`.

**Interfaces — produces** (namespace `pychron::processing`, Qt-free):

```cpp
std::string flux_j_text(const std::optional<double>& v);            // "%.4e", absent "-"
std::string flux_model_line(const FluxOptions& options);            // "plane, weighted; mean arithmetic (msem); fit error msem"
std::string flux_summary(const LevelFit& fit);                      // "fit MSWD 1.12 (5 dof)   J min 1.0012e-03  max 1.0241e-03  delta 2.24 %"
// One line each, no "warning: " prefix: analyses out with reasons (Task 1), extrapolated holes, a mean MSWD
// outside its limits (by hole), the fit MSWD outside its limits, monitors left out, the saved monitor set
// missing from the store, the saved fit having used SD.
struct FluxWarningContext { bool monitor_set_given = false; bool fit_error_given = false; };
std::vector<std::string> flux_warnings(const LevelInputs& inputs, const LevelFit& fit, const FluxWarningContext& context = {});
std::string csv_field(std::string_view text);                       // RFC 4180
std::string flux_csv_header();                                      // the existing 19 columns, CRLF
std::string flux_csv_rows(const LevelFit& fit);                     // monitors then unknowns, CRLF
```

- [ ] **Step 1: Failing tests** (`test_flux_view.cpp`, suite `FluxText`): `ModelLinePerModel` (plane weighted; `nearest` shows the neighbours; `bracketing` shows the interpolation; `bracketing1d` shows none), `SummaryFormat` (the exact string for a hand-built `LevelFit`), `WarningsCoverEveryKind` (one `LevelFit`/`LevelInputs` built to trigger each kind: assert each line's text), `WarningsHonourTheContext` (monitor-set line absent when `monitor_set_given`; SD line absent when `fit_error_given` or the model is not least squares), `CsvIsRfc4180` (a sample `FC-2, "new"` round-trips; 19 fields per row; CRLF).
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** by moving the bodies out of `apps/elctl/src/flux.cpp` unchanged in output; `elctl` calls the shared functions and prefixes `warning: `. Delete the moved declarations from `apps/elctl/src/flux.hpp` (tests that called `elctl::csv_field` etc. call `pychron::processing::` instead).
- [ ] **Step 4: Run.** Every existing `FluxCmd*` test passes unchanged in expectation (output is byte-identical); `FluxText.*` pass.
- [ ] **Step 5: Commit.** `refactor(processing): the flux summary, warnings and CSV are shared text`

---

### Task 3: The options schema, and `enabled_when` with `in`

**Files:** modify `flux_view.hpp`/`.cpp`, `apps/pychron-ui/src/options_editor.cpp`; tests `tests/processing/test_flux_view.cpp`, `tests/ui/test_options_editor.cpp` (the existing file for the editor; find its real name under `tests/ui`).

**Interfaces — produces:**

```cpp
SchemaPtr flux_options_schema();                       // kind "flux", version 1, one shared instance
Options to_options(const FluxOptions& options);
Result<FluxOptions> flux_options_from(const Options& options);
```

Fields, verbatim from spec 4.2: `model.kind` (enum `plane|bowl|weighted-mean|matching|nearest|bracketing|ls1d|mean1d|bracketing1d`, default `plane`), `model.weighted` (bool false, `model.kind in plane|bowl|ls1d`), `model.neighbors` (int 1..64, 2, `model.kind == nearest`), `model.interpolation` (enum `weighted|average|linear`, `model.kind == bracketing`), `model.axis` (enum `x|y`, `model.kind in ls1d|mean1d|bracketing1d`), `model.degree` (int 1..4, 1, `model.kind == ls1d`), `mean.kind` (enum `arithmetic|weighted`), `mean.error` (enum `sem|msem|sd`, default `msem`), `fit.error` (enum `sem|msem|sd`, default `msem`, `model.kind in plane|bowl|weighted-mean|ls1d|mean1d`). Sections `Model` and `Errors`. Labels: `Model`, `Weighted fit`, `Neighbours`, `Interpolation`, `Axis`, `Degree`, `Mean`, `Error of the mean`, `Error of the fit`. `help` for each: the one-sentence description of that option from `docs/flux.md`. Factory presets: `Default` (empty TOML), `Weighted plane` (`[model]\nweighted = true`).

- [ ] **Step 1: Failing tests.** Core (`FluxSchema`): `DefaultsAreFluxOptionsDefaults` (`flux_options_from(Options(schema)) == FluxOptions{}`); `RoundTripForEveryModel` (for each of the nine kinds with non-default values of every option: `flux_options_from(to_options(o)) == o`); `SdWithASurfaceIsTheMathLayersError` (what contains `sd is not an error kind of a fitted surface`; with `weighted-mean` it is accepted); `FactoryPresets` (`PresetStore(tmp).list(schema)` names `Default` and `Weighted plane`; loading the second gives `fit.weighted`); `EnabledWhenNamesOnlyKnownKeysAndValues` (every `enabled_when` parses to a key of the schema and values among that key's choices). UI (`test_options_editor`): `enabled_when_in_a_list` — a two-field schema with `b` enabled when `a in x|z`: the editor of `b` is enabled for `x` and `z`, disabled for `y`; the `==`, `!=` and bare-key forms still behave.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement.** `condition_holds` (`options_editor.cpp:48`) gains the form `<key> in <v1>|<v2>|...` (split on `|`, trim, equal to the option's string value). Conversions map the CLI spellings with `parse_model_kind`, `parse_mean_kind`, `parse_mean_error_kind` and their `to_string`s; a field disabled for the model keeps its value and is still converted.
- [ ] **Step 4: Run.** `FluxSchema.*`; the editor's UI test.
- [ ] **Step 5: Commit.** `feat(processing): a schema for flux options`

---

### Task 4: The scene

**Files:** modify `flux_view.hpp`/`.cpp`; test `tests/processing/test_flux_view.cpp`.

**Interfaces — produces:**

```cpp
enum class FluxAbscissa { Angle, X, Y };
FluxAbscissa flux_abscissa(const FluxOptions& options);
double flux_hole_abscissa(FluxAbscissa kind, double x, double y);   // Angle: degrees(atan2(x, y)), in (-180, 180]
struct FluxSceneOptions { std::optional<int> highlight_hole; };
ScenePtr flux_scene(const LevelInputs& inputs, const LevelFit& fit, const FluxSceneOptions& options = {});
ScenePtr flux_scene(const LevelInputs& inputs, const FluxOptions& options, const Edits& edits);   // no fit
```

Scene shape (spec 4.3): `kind = "flux"`, one graph, one panel `p0`, `quantity = "J"`; x title `Hole angle (degrees)` | `X` | `Y`. Layers in this order, with these labels (the legend): band (no label) and line `Fit`; `PointLayer` `Analyses` (circle, size 4; refs = uuids; excluded per analysis not `Used`; spread about the hole's abscissa by `(i - (n - 1) / 2) * step`, `step` = 4 / max(n - 1, 1) degrees for Angle or 4 % of the axis range / max(n - 1, 1) for X, Y, in record-id order); `PointLayer` `Monitor means` (diamond, size 8, error bars; excluded when not `used_in_fit`); `PointLayer` `Unknowns` (square, size 6, error bars); highlight layers (no label). Colours: `palette_color(0)` fit, `(1)` analyses, `(2)` means, `(3)` unknowns, `(4)` highlight; the band the fit colour at alpha 48. The curve: 181 points, `reduction::fit_flux` on the used monitors' means with `fit.options.fit`; Angle: a circle of the used monitors' mean radius, angle -180..180; X or Y: from the least to the greatest coordinate over all positions. No band or line for Matching, NearestNeighbors, Bracketing, Bracketing1D, nor when `fit_flux` fails at the curve points. The no-fit overload computes each analysis's J with `reduction::j_of` and each monitor's mean with `reduction::mean_j` under `options` and `edits`, and has layers 2 and 3 only.

- [ ] **Step 1: Failing tests** (suite `FluxScene`, on the eight ring monitors and four unknowns of `tests/reduction/flux_golden.hpp`, built as `test_flux_fit.cpp` builds them — share its builder by moving it into `tests/processing/flux_level_inputs.hpp`):

```cpp
TEST(FluxScene, AbscissaPerModel)                 // the three 1-D kinds with axis x/y -> X/Y; the other six -> Angle
TEST(FluxScene, HoleAbscissa)                     // (0,10) -> 0; (10,0) -> 90; (0,-10) -> 180; (-10,0) -> -90; X and Y pass through
TEST(FluxScene, OneAnalysisPointPerAnalysisWithAJ)   // 24 points, refs == the uuids, excluded all false; with one omitted and one NotReduced: 23 points, one excluded
TEST(FluxScene, AnalysesSpreadAboutTheirHole)     // three analyses of a hole at abscissa a: x == a - 2, a, a + 2 (Angle)
TEST(FluxScene, MeansAndUnknowns)                 // 8 means with y_err == mean_j_err; a monitor left out is excluded; 4 unknowns with y == predicted j
TEST(FluxScene, TheCurvePassesThroughThePredictions)   // Plane: the line interpolated at each ring monitor's angle == that monitor's predicted j to 1e-9 relative;
                                                  // band low/high == j -/+ j_err there
TEST(FluxScene, WhichModelsHaveACurve)            // LineLayer and BandLayer present for plane, weighted-mean, ls1d, mean1d (bowl on kMixed); absent for matching, nearest, bracketing, bracketing1d
TEST(FluxScene, Highlight)                        // highlight_hole 3 adds layers holding exactly hole 3's analyses and mean
TEST(FluxScene, WithoutAFitTheDataIsStillThere)   // the no-fit overload: analyses and means layers only, same points as with a fit
TEST(FluxScene, TooltipsSayWhy)                   // an omitted analysis's tooltip contains its record id and "omitted (tag outlier)"
```

- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** `FluxScene.*`, then the whole processing test binary.
- [ ] **Step 5: Commit.** `feat(processing): the scene of a flux fit`

---

### Task 5: The three table models

**Files:** create `apps/pychron-ui/src/flux_monitor_model.{hpp,cpp}`, `flux_analysis_model.{hpp,cpp}`, `flux_unknown_model.{hpp,cpp}`, `tests/ui/test_flux_models.cpp`; modify `apps/pychron-ui/CMakeLists.txt`.

**Interfaces — produces** (namespace `pychron::ui`; each a `QAbstractTableModel`; none knows the store):

```cpp
class FluxMonitorModel {
 public:
  enum Column { Fit, Save, Hole, Identifier, Sample, N, SavedJ, SavedJErr, MeanJ, MeanJErr, MeanPercent, Mswd,
                PredJ, PredJErr, PredPercent, Dev, ColumnCount };
  void set_fit(const processing::LevelFit* fit);            // nullptr or a failed fit: rows from set_inputs, predicted blank
  void set_inputs(const processing::LevelInputs* inputs);   // the monitor rows when there is no fit
  void set_skip(const std::set<int>& skip_positions);
  int hole_at(int row) const;  int row_of(int hole) const;
 signals:
  void fit_toggled(int hole, bool in_fit);                  // the Fit box
  void save_toggled(int hole, bool save);
};
class FluxUnknownModel {   // Column { Save, Hole, Identifier, Sample, SavedJ, SavedJErr, PredJ, PredJErr, PredPercent, Dev, ColumnCount }
  void set_fit(const processing::LevelFit*);  void set_inputs(const processing::LevelInputs*);  void set_skip(const std::set<int>&);
  int hole_at(int row) const;
 signals: void save_toggled(int hole, bool save);
};
class FluxAnalysisModel {  // Column { Use, Record, Tag, J, JErr, State, ColumnCount }
  void set_position(const processing::FittedPosition* position);   // nullptr: empty
 signals: void use_toggled(const QString& uuid, bool use);
};
```

The models hold pointers the window keeps alive and resets on every fit (begin/endResetModel). Headers, verbatim: monitors `Fit, Save, Hole, Identifier, Sample, N, Saved J, ±, Mean J, ±, %, MSWD, Pred. J, ±, %, Dev %`; unknowns `Save, Hole, Identifier, Sample, Saved J, ±, Pred. J, ±, %, Dev %`; analyses `Use, Record, Tag, J, ±, State`. J via `processing::flux_j_text` (blank, not `-`, for absent); `%` and MSWD `%.2f`. `State` text: `used`, `omitted (tag <tag>)`, `omitted (saved fit)`, `omitted (here)`, `not reduced`, `no J`. Check boxes are `Qt::CheckStateRole` on `ItemIsUserCheckable`; `setData` emits the signal and does NOT change the model (the window refits and resets it). `Fit` is not checkable (and its `Qt::ToolTipRole` is `No usable analysis`) for a monitor with the `NoUsableAnalysis` note; `Use` is not checkable for `NotReduced` (tooltip: the reduction error) and `NoJ`. `Qt::BackgroundRole`: the theme's warning row tint when the mean MSWD is outside its limits, the muted row tint when the monitor is not in the fit — use tints `theme()` already has (read `theme.hpp`); add none unless none fits, and then in `theme.cpp` only.

- [ ] **Step 1: Failing tests** (`test_flux_models.cpp`, QtTest; a hand-built `LevelFit`, no store): `monitor_headers_and_cells` (each column of one row, exact text), `monitor_fit_box_emits_and_does_not_change` (setData(Fit, Unchecked) -> `fit_toggled(3, false)` once; the cell still reads Checked), `monitor_without_usable_analysis_is_not_checkable`, `monitor_tints`, `monitor_rows_without_a_fit` (set_fit(nullptr) + set_inputs: holes and identifiers shown, predicted cells blank), `unknown_cells_and_save_box`, `analysis_states_and_use_box` (one row per `AnalysisState`; `use_toggled(uuid, false)`), `skip_positions_untick_save`.
- [ ] **Step 2: Run to verify failure** (compile).
- [ ] **Step 3: Implement**; add the six files to the store block of `apps/pychron-ui/CMakeLists.txt`.
- [ ] **Step 4: Run.** `ctest --preset dev-ui -R 'ui.test_flux_models|ui.test_theme|ui.test_shortcuts'`.
- [ ] **Step 5: Commit.** `feat(ui): table models for the flux window`

---

### Task 6: The window: tree, load, fit, show

**Files:** create `apps/pychron-ui/src/flux_window.{hpp,cpp}`, `tests/ui/test_flux_window.cpp`; modify `flux_store.hpp`/`flux_store.cpp` (status), `tests/processing/test_flux_store.cpp`, `apps/pychron-ui/CMakeLists.txt`, `tests/ui/CMakeLists.txt` (add `${PROJECT_SOURCE_DIR}/tests/processing` to the include directories so `flux_seed.hpp` is reachable).

**Interfaces — produces:**

```cpp
// flux_store.hpp
enum class LevelFluxStatus { NoMonitors, NotFitted, Fitted };
LevelFluxStatus level_flux_status(const persistence::LevelSheet& sheet, std::string_view monitor_sample);

// flux_window.hpp
class FluxWindow : public QMainWindow {
 public:
  // All three must outlive the window.
  FluxWindow(EntryBridge& bridge, processing::IAnalysisSource& source, processing::PresetStore& presets, QWidget* parent = nullptr);

  void reload_tree();                                              // asynchronous
  void open_level(const QString& irradiation, const QString& level);   // asynchronous; asks first when edits are pending (Task 7)
  bool busy() const noexcept;                                      // a bridge job is running, or a refit is pending
  QString status() const;  bool status_is_error() const;
  QStringList warnings() const;                                    // the status tooltip, one per line
  QTreeWidget* tree() const noexcept;
  SceneView* view() const noexcept;
  FluxMonitorModel* monitors() const noexcept;  FluxUnknownModel* unknowns() const noexcept;  FluxAnalysisModel* analyses() const noexcept;
  QTableView* monitor_table() const noexcept;
  const processing::LevelInputs* inputs() const noexcept;          // nullptr until a level loaded
  const processing::LevelFit* fit() const noexcept;                // nullptr when the fit failed
  const processing::FluxOptions& options() const noexcept;
  void select_monitor(int hole);                                   // as clicking its row
};
```

Layout per spec 5.3: `QTreeWidget` left (two columns: name, status), a vertical splitter of the `SceneView` over a horizontal splitter of (monitors table over analyses table) and the unknowns table. Object names snake_case: `flux_window`, `flux_tree`, `flux_monitors`, `flux_analyses`, `flux_unknowns`, `flux_status`. Size 1400 x 820. Title `Flux`, then `Flux — <irradiation> <level>`.

Behaviour in this task:
- `reload_tree`: one bridge job reads `irradiations()`, each one's `levels()` and `level_sheet()`, and the monitor document's default set; the tree lists irradiations (newest first by name descending) with levels under them; column 2 is `no monitors` | `not fitted` | `fitted` in the muted tone.
- Selecting a level (or `open_level`): a bridge job runs `load_level(source, store, irradiation, level, MonitorSelection{})`. Status `Loading <irradiation> <level>…`. Each request takes a generation number; a result whose generation is not the latest is dropped (Review Focus 2). An error: status in the error tone, tables and plot empty, `inputs()` null.
- On arrival: options = `inputs.saved_options` or `FluxOptions{}` (presets come in Task 7); `Edits{}`; fit.
- Fitting: a single-shot 150 ms `QTimer`; then `fit_level` on the GUI thread. Success: models and `flux_scene(inputs, fit, {highlight})` set; status = `flux_model_line` first clause + ` · ` + `flux_summary` reshaped as spec 5.3 (`plane, weighted · fit MSWD 1.12 (5 dof) · J 1.0012e-03 – 1.0241e-03 (2.24 %)`), plus ` · <n> warnings` with `flux_warnings` in the tooltip. Failure: status = the error text unreworded, error tone; `fit()` null; models show rows without predictions; the scene is the no-fit overload.
- `select_monitor` / a row selection: the analyses table shows that monitor; the scene is rebuilt with `highlight_hole`.

Test support: `tests/processing/flux_seed.hpp` seeds one level with monitors (`seed_flux_level`) and can add a level without monitors; the tests below also need a SECOND level with monitors in the same store — add a free function for it there in the same style (no GoogleTest types) if none exists.

- [ ] **Step 1: Failing tests.** Store (`FluxLevelStatus`, in `test_flux_store.cpp`): the three statuses on the seeded level (before any save: NotFitted; after `save_level`: Fitted; a level whose positions carry no monitor sample: NoMonitors). Window (`test_flux_window.cpp`; QtTest; `QSKIP` without `PYCHRON_UI_HAS_STORE`; fixture: a temp-file SQLite store seeded with `seed_flux_level` from `flux_seed.hpp`, an `EntryBridge::open({url, "tester", "test-host"})`, a `StoreSource` opened on the same URL, a `PresetStore` on a temp directory; wait with `QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), 10000)`):

```cpp
void tree_lists_levels_with_their_status();          // NM-300 > A "not fitted"; after seed_level_without_monitors B "no monitors"
void opening_a_level_fills_tables_plot_and_status(); // 8 monitor rows, 4 unknown rows; view()->scene has 24 analysis points;
                                                     // status starts "plane · fit MSWD" and contains "(4 dof)"; title "Flux — NM-300 A"
void selecting_a_monitor_shows_its_analyses();       // select_monitor(3): analyses()->rowCount() == 3; the scene has the highlight layers
void a_level_that_cannot_load_says_why();            // B (no monitors): status_is_error, status contains "no monitor positions", inputs() == nullptr
void the_latest_selection_wins();                    // open_level(A) then at once open_level(B'): where B' is a second seeded level with monitors;
                                                     // after idle the title names B' and the monitor identifiers are B''s (Review Focus 2)
void closing_while_loading_does_not_crash();         // open_level then delete the window before the job returns; process events until the bridge is idle (Review Focus 3)
```

- [ ] **Step 2: Run to verify failure** (compile).
- [ ] **Step 3: Implement.** Add `flux_window.{hpp,cpp}` to the store block of the app's CMake list.
- [ ] **Step 4: Run.** `ctest --preset dev-ui -R 'ui.test_flux_window|ui.test_theme|ui.test_shortcuts'`; `pychron_processing_store_tests --gtest_filter='FluxLevelStatus.*'`.
- [ ] **Step 5: Commit.** `feat(ui): a flux window that loads and fits a level`

---

### Task 7: Edits, options, presets, the monitor group, prompts

**Files:** modify `apps/pychron-ui/src/flux_window.{hpp,cpp}`, `tests/ui/test_flux_window.cpp`.

**Interfaces — produces** (added to `FluxWindow`):

```cpp
  enum class Unsaved { Save, Discard, Cancel };
  void set_ask_unsaved(std::function<Unsaved(const QString&)> ask);   // default: QMessageBox with the three buttons
  bool edited() const noexcept;                                    // spec 5.6
  const processing::Edits& edits() const noexcept;
  void toggle_analyses(const QStringList& uuids);                  // as a plot click or rubber band
  void set_in_fit(int hole, bool in_fit);                          // as the Fit box
  void set_options(const processing::Options& options);            // as editing the dock
  OptionsEditor* options_editor() const noexcept;  PresetBar* preset_bar() const noexcept;
  QComboBox* monitor_set_combo() const noexcept;  QLineEdit* sample_edit() const noexcept;  QCheckBox* all_positions_box() const noexcept;
  void revert();  void reload();  void reset_omissions();
  QAction* revert_action() const noexcept;  QAction* reload_action() const noexcept;  QAction* reset_omissions_action() const noexcept;
```

Behaviour:
- W9: `view()->point_clicked` / `points_toggled`, `analyses()->use_toggled` -> `toggle_analyses`: for each uuid, an analysis now taking part goes into `Edits::omit` (and out of `include`); one now omitted goes into `Edits::include` (and out of `omit`); `NotReduced` and `NoJ` are ignored. `monitors()->fit_toggled` -> `set_in_fit` -> `Edits::exclude_positions`; re-including a monitor the saved fit excluded needs `Edits` to say so: add `std::set<int> include_positions` to `processing::Edits` (it overrides a carried exclusion, as `include` overrides a carried omission) with its `fit_level` handling and a core test — say so in the report as an addition to the spec's section 4.
- Right dock `Fit` (object name `flux_dock`): group box `Monitors` (form: `Monitor set` combo from `load_monitor_sets`, tooltip `sample <s> · <age> ± <err> Ma · lambda_k <v>`; `Sample` line edit with the set's sample as placeholder; `All positions` check box), then `PresetBar(presets, flux_options_schema())` with `current = [this]{ return to_options(options_); }`, then `OptionsEditor`. A change in the group reloads the level with `MonitorSelection{set, sample-or-nullopt, all_positions}` after the unsaved question; an explicit choice sets `FluxWarningContext::monitor_set_given`.
- W6: after a load, with `inputs.saved_options` the editor shows them and the preset combo shows an extra first entry `(saved fit)` selected; without, the preset bar's current preset is applied. `PresetBar::loaded` -> `set_options`. `OptionsEditor::changed` -> `flux_options_from`: an error is shown in the status (error tone) and marks the fit failed without refitting; success refits.
- `edited()`: `Edits` not empty, or options != the loaded ones, or the monitor group != the loaded selection, or any Save box unticked. The status ends ` · edited (not saved)`.
- `revert`: back to the loaded options, empty `Edits`, all Save boxes ticked; no store call. `reload`: asks, then loads again with the current monitor group. `reset_omissions`: `Edits{}` with `reset_omits = true`.
- The unsaved question `Save the flux of <irradiation> <level>?` before: selecting another level, `reload`, a monitor-group change, `closeEvent`. Cancel leaves everything as it was (the tree selection and the group widgets are put back). Save is wired in Task 8; until then Save behaves as Cancel.
- Review Focus 4: on `EntryBridge::changed()` — not edited: reload the tree and the level; edited: reload the tree only and append ` · level changed elsewhere, Reload to see it` to the status.

- [ ] **Step 1: Failing tests** (added to `test_flux_window.cpp`; `set_ask_unsaved` records the question and returns a chosen answer):

```cpp
void unticking_fit_drops_a_degree_of_freedom();         // set_in_fit(3,false): fit()->dof == 3; the row reads unchecked; edited(); status ends "edited (not saved)"
void a_plot_click_omits_an_analysis_everywhere();       // click at view()->point_position(uuid): monitors N 3 -> 2, mean changed, analyses row unchecked "omitted (here)",
                                                        // the scene point excluded; clicking again restores N == 3 and !edited()
void the_use_box_and_the_plot_click_are_the_same_edit();// toggling via analyses()->setData gives the same fit()->positions as the click
void a_saved_exclusion_can_be_ticked_back();            // seed a saved fit with hole 5 excluded: opens with 5 unticked; set_in_fit(5,true): used in the fit
void a_preset_applies();                                // preset_bar()->select("Weighted plane"): options().fit.weighted; status starts "plane, weighted"
void a_saved_fit_is_where_a_level_starts();             // seed_save_flux with nearest/3 neighbours: options() == those; combo text "(saved fit)"
void a_model_the_monitors_cannot_fit();                 // set model bowl (the ring cannot determine it): status_is_error and contains "do not determine a bowl";
                                                        // fit() == nullptr; monitor rows still shown, Pred. J blank; view has analyses; then model plane recovers (Review Focus 5)
void sd_with_a_surface_is_shown_on_the_field();         // fit.error = sd with plane: status contains "sd is not an error kind"; no refit ran
void another_monitor_set_reloads();                     // add a second set to the document; choose it: asks nothing when not edited; inputs()->monitor_set.name is the new one
void switching_level_with_edits_asks_once();            // Cancel: stays, edits kept, tree selection back on A; Discard: moves
void revert_and_reset_omissions();                      // revert: !edited(), loaded options; reset_omissions on a level with a saved exclusion: hole used again
void a_change_elsewhere_reloads_only_when_not_edited(); // emit bridge.notify_changed(): not edited -> busy then reloaded; edited -> edits kept, status contains "changed elsewhere"
```

and in `tests/processing/test_flux_fit.cpp`: `IncludePositionsOverridesACarriedExclusion`.

- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** The flux window UI test, the theme and shortcut guards, `FluxFitLevel.*`, and the elctl flux tests (an `Edits` field was added).
- [ ] **Step 5: Commit.** `feat(ui): the flux window's edits, options and presets`

---

### Task 8: Save, conflict, export

**Files:** modify `apps/pychron-ui/src/flux_window.{hpp,cpp}`, `tests/ui/test_flux_window.cpp`.

**Interfaces — produces:**

```cpp
  void save();                                              // asynchronous
  void set_save(int hole, bool save);                       // as a Save box
  QAction* save_action() const noexcept;
  Result<void> export_csv(const QString& path);
 signals:
  void saved(const QString& irradiation, const QString& level);
```

Behaviour (spec 5.5):
- `save()` first flushes a pending debounce (stop the timer, fit now) so what is saved is what is on screen (Review Focus 1); returns without a job when there is no fit.
- The job: `save_level(store, actor, fit_copy, SaveSelection{skip}, software)`; the fit is copied into the job. Status `Saving…`; the tool bar is disabled while it runs.
- Outcomes, verbatim: `Saved <w> positions (<u> unchanged)` (with `, <k> not saved` when positions were skipped), then the level is reloaded, `bridge.notify_changed()` and `saved(...)`; `Nothing to save: <u> positions unchanged`; a conflict `Not saved: hole <N> was saved by <user> at <YYYY-MM-DD hh:mm:ss> since this level was loaded. Reload and fit again.` in the error tone with the edits kept (`<user>` and the time from the conflicting head, read in the same job as `elctl` does); any other error `Not saved: <error>`.
- A result for a level that is no longer the one shown is dropped apart from `notify_changed()` (Review Focus 3).
- The Save action is enabled when `fit()` and not busy; otherwise its tooltip is the fit error, or `Loading…` / `Saving…`, or `No level open`.
- The unsaved question's Save answer now saves and, on success only, carries on with what was asked.
- `export_csv`: `flux_csv_header() + flux_csv_rows(*fit)` to a sibling temporary file then renamed, `mark_as_user_file`; an error without a fit (`Nothing fitted to export`). Tool bar `Export CSV…` asks for a path and shows a message box only when writing fails.

- [ ] **Step 1: Failing tests** (the fixture's own `IStore` is the "other client"):

```cpp
void save_writes_and_says_so();                 // "Saved 12 positions (0 unchanged)"; every position's head J read back from the store == the predicted J; then "fitted" in the tree
void a_second_save_has_nothing_to_write();      // "Nothing to save: 12 positions unchanged"; the store's change seq unchanged
void an_unticked_save_box_leaves_its_head();    // set_save(9,false): hole 9's head unchanged; status contains "1 not saved"
void save_inside_the_debounce_saves_what_is_shown();   // set_in_fit(3,false) then save() with no wait: the saved options_json of hole 3 says excluded (Review Focus 1)
void a_conflict_writes_nothing_and_keeps_the_edits();  // another client publishes hole 7 after the load: status starts "Not saved: hole 7 was saved by",
                                                // status_is_error, every other head unchanged, edits() unchanged
void save_is_disabled_with_the_reason();        // failed fit: !save_action()->isEnabled(), toolTip contains the fit error
void the_unsaved_question_can_save_then_move(); // edits on A, select B', answer Save: A saved (store read back), window on B'
void a_failed_save_cancels_the_move();          // as above with a conflict on A: still on A
void export_csv_writes_the_level();             // 13 lines, the header of processing::flux_csv_header(); without a fit: an error
void closing_while_saving_does_not_crash();     // (Review Focus 3)
```

- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** The flux window UI test.
- [ ] **Step 5: Commit.** `feat(ui): the flux window saves a level's J`

---

### Task 9: `Fit ▸ Flux…`, the Packages action, and the wiring

**Files:** create `apps/pychron-ui/src/fit_actions.{hpp,cpp}`; modify `menu_hub.{hpp,cpp}`, `entry_actions.{hpp,cpp}`, `packages_window.{hpp,cpp}`, `flux_window.{hpp,cpp}` (tool bar `Open in Packages`, recall), `main.cpp`, `apps/pychron-ui/CMakeLists.txt`; tests `tests/ui/test_flux_window.cpp`, `tests/ui/test_menu_hub.cpp` (the hub's existing test; find its real name), `tests/ui/test_entry_windows.cpp`.

**Interfaces — produces:**

```cpp
// menu_hub.hpp
enum class Menu { File, Queue, Rows, Executor, Scripts, View, Window, Help, Entry, Fit };   // kMenus = 10; kOrder: ..., Entry, Fit, Window, Help

// entry_actions.hpp
bool ensure_bridge();                      // now public: opens the bridge on first use, a message box on failure
 signals: void flux_requested(const QString& irradiation, const QString& level);   // forwarded from the Packages window

// packages_window.hpp
QAction* fit_flux_action() const noexcept; // "Fit flux…", enabled when a level is open
 signals: void flux_requested(const QString& irradiation, const QString& level);
void show_level(const QString& irradiation, const QString& level);                 // selects and opens it (for "Open in Packages")

// fit_actions.hpp
class FitActions : public QObject {
 public:
  // `entry` and `presets` must outlive this; `open_recall` may be empty.
  FitActions(QWidget* owner, std::string store_url, EntryActions& entry, processing::PresetStore& presets,
             std::function<void(const QString& uuid)> open_recall = {});
  FluxWindow* flux();                                       // made on first use; nullptr when the store or source cannot be opened
  void open_flux(const QString& irradiation, const QString& level);
  QAction* flux_action() const noexcept;                    // "Flux…"
};
```

Behaviour: `FitActions` contributes `Flux…` under `Menu::Fit`, `Scope::App`. `flux()` calls `entry.ensure_bridge()`, opens a `processing::StoreSource` on the URL (a `QMessageBox::critical` titled `Flux` with `The store could not be opened:\n<error>` on failure), makes the window with `setWindowFlag(Qt::Window)`, keeps it in a `QPointer`, and shows it. Members are declared source-then-window so the window is destroyed first (delete it in the destructor, as `EntryActions` does). `EntryActions::flux_requested` -> `open_flux`. The window's `Open in Packages` -> `entry.packages()->show_level(...)`. `FluxWindow::set_open_recall(std::function<void(const QString&)>)`: the scene view's `recall_requested` calls it; when empty the view's Recall item is hidden (read how `SceneView` exposes that; if it cannot hide the item, leave it and make the handler a no-op, and say so). `main.cpp`: in both start-up paths, where `EntryActions` is created with a URL, keep the pointer and create `new FitActions(&window, url, *entry, presets, <recall>)` — `<recall>` calls the `DataWorkspace`'s `open_recall` where that path has a workspace, else `{}`.

- [ ] **Step 1: Failing tests:**

```cpp
// test_menu_hub: fit_menu_sits_between_entry_and_window()  — with a contribution to Menu::Fit the bar's titles contain "Fit" after "Entry" and before "Window";
//                                                           with none, no "Fit" menu is shown; existing order assertions unchanged
// test_entry_windows: packages_fit_flux_asks_for_the_level() — open level A: fit_flux_action enabled; trigger -> flux_requested("P-1","A") once; with no level open it is disabled
// test_flux_window:
void the_menu_action_opens_the_window();          // MenuHub::reset(Bars::PerWindow); FitActions on the seeded store; flux_action()->trigger(): flux() visible; "Fit › Flux" among MenuHub::commands()
void packages_opens_the_flux_window_on_its_level();   // entry.packages() on NM-300 A, trigger fit_flux_action: flux() shows "Flux — NM-300 A"
void packages_sees_the_new_j_after_a_save();      // save in the flux window: the Packages grid's J cell for hole 9 becomes the predicted J without reopening
void open_in_packages_shows_the_level();
void a_store_that_cannot_open_gives_no_window();  // a URL to a missing file: flux() == nullptr (inject the message box as EntryActions' tests do, or check no window)
```

- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement**; add `fit_actions.{hpp,cpp}` to the store block.
- [ ] **Step 4: Run.** `ctest --preset dev-ui -j 8` whole (the menu change touches every window's bar).
- [ ] **Step 5: Commit.** `feat(ui): Fit ▸ Flux, and Fit flux from the Packages window`

---

### Task 10: Guide, agent guide, spec notes, full suites

**Files:** modify `docs/flux.md`, `AGENTS.md`, `docs/superpowers/specs/2026-10-07-flux-window-design.md`.

- [ ] **Step 1: `docs/flux.md`.** A section "The flux window" before the command line: how to open it (`Fit ▸ Flux…`, or `Fit flux…` in Packages); each region in a sentence or two; that every edit refits at once; the Fit, Save and Use boxes; the plot (click or shift-drag to omit, what each marker is, the curve and its band, which models have none); presets and where a level's options come from (W6); the monitor group (W7); Save, a conflict, Revert / Reload / Reset omissions; Export CSV; that the window and `elctl flux fit --save` write the same thing. Remove the sentence saying there is no flux window. Describe only what the code does: run the application or read `flux_window.cpp` for every string quoted.
- [ ] **Step 2: `AGENTS.md`.** In the flux bullet, one sentence: the flux window (`apps/pychron-ui` `flux_window.cpp`) computes nothing; its scene, options schema, status text and CSV are in `libs/processing` `flux_view.hpp`, shared with `elctl flux`.
- [ ] **Step 3: The spec.** Status `Implemented (owner decisions in section 3; implementation notes in section 11)`; a section "11. Implementation notes" recording what differs from the text: the shared text functions of Task 2 (4.2-4.4 grow a 4.5), `Edits::include_positions`, where `enabled_when`'s `in` form is evaluated, anything else that ended up different; amend the sections those contradict.
- [ ] **Step 4: Full suites.**

```bash
cmake --build build/dev -j 10 && ctest --preset dev -j 8
cmake --preset dev-ui && cmake --build build/dev-ui -j 10 && ctest --preset dev-ui -j 8
```

Expected: `100% tests passed` twice. Then the build without persistence, which must still compile the UI-less targets and the pure flux code: `cmake -S . -B build/nopersist -DPYCHRON_PERSISTENCE=OFF -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build/nopersist -j 10 --target pychron_elctl_tests pychron_reduction_tests pychron_processing_tests && ctest --test-dir build/nopersist -R 'Flux'`.

- [ ] **Step 5: Look at it.** Launch the application on a seeded SQLite store (seed one with a small program or the test fixture's helpers into a scratch file; `pychron-ui --db sqlite:<file>`), open `Fit ▸ Flux…`, and take screenshots of: a fitted level; a monitor selected with one analysis omitted; a failed fit. Check against spec 5.3: nothing clipped, columns readable at 1400 x 820, the status line fits. Fix what is wrong; attach the screenshots' paths to the report.
- [ ] **Step 6: Commit.** `docs: the flux window in the flux guide`
- [ ] **Step 7: Land** as `AGENTS.md` says, only when the owner asks.
