# Data browsing, recall and visualization

Date: 2026-10-02
Status: V1 and the V2 figures implemented (section 14); V2 data and V3 proposed
Owner: Jake Ross
Depends on: `2026-10-02-arar-reduction-design.md` (UFloat, `reduce()`, fits),
`2026-10-01-dvc-schema-design.md` (store, revisions, reference resolution),
`2026-09-29-experiment-system-design.md` (`AnalysisRecord` v2),
`2026-10-01-spectrometer-window-design.md` (QCustomPlot, bridge pattern).
Reference: legacy pychron `pychron/options/`, `pychron/pipeline/`,
`pychron/pipeline/plot/`, `pychron/graph/`, `pychron/envisage/browser/`,
`pychron/processing/analyses/`, `pychron/dvc/`. Citations are `file:line` in
`pychron/` (Python, read-only).

## 1. Goal

Find analyses, look at one in depth, and plot many, with figures as
customizable as legacy pychron's, built from small composable reduction units
rather than legacy's pipeline engine.

Success for V1 (section 14):

1. Run the example queue in simulation; open Window > Data; the browser lists
   the runs; filter by type, identifier and date; double-click opens the
   recall window (summary, isotopes with every correction stage, evolutions
   with fits, error budget, extraction, spectrometer).
2. Select runs, Plot > Time Series: a stacked time-series figure with one
   panel per chosen quantity (Ar40, Ar40/Ar36, an intercept, a baseline, a
   gain, extract value, ...), coloured by group, with fits and statistics.
   Clicking a point omits it and the fit and statistics update. Options are
   edited in a dock generated from the figure's schema and saved as named
   presets. The figure exports to PDF and PNG.
3. Everything above except drawing is Qt-free and tested headless.

## 2. What legacy does, and what we keep

Four reviews of legacy were done for this spec (options manager, figures,
recall and browser, pipeline). The findings that shape the design:

| Area | Legacy | Keep | Change |
|---|---|---|---|
| Options | ~25 Traits classes (`options/*.py`) with per-figure fields, `AuxPlot` rows, `GroupOptions` palettes, named sets as JSON per user (`options_manager.py:165-495`) | Named presets per figure kind, factory defaults, one row per stacked panel, per-group colours, subview tabs | No schema version, class-path strings in files (`options.py:453-542`), factory sets overwritten on every launch (`options_manager.py:303`), UI handlers inside models, shared mutable instance across editors. Replace with a versioned, schema-described value tree (section 6). |
| Figures | `FigureEditor -> FigureModel -> FigurePanel (graph_id) -> plotter (group_id) -> Graph` (`pipeline/plot/`); stats computed in the plotter as a side effect | Panels per graph id, overlays per group, stacked aux panels sharing x, mean/plateau/isochron annotations, click-to-omit, hover inspector, limit editing | Compute separated from render: a unit computes a Qt-free `Scene` (section 8); the UI only draws it. Fit results never live on analyses as a side effect of plotting (`plotter/blanks.py:35-39`). Panel order top-to-bottom, never reversed (`options.py:976-985`). |
| Time series | Relative hours from the last analysis of each group (`plotter/series.py:261-301`); no date axis; missing values plotted as 0 (`arar_age.py:256-306`) | Every quantity the series node offers (isotopes, baselines, IC, ratios, age, F, yield, peak center, lab environment, extraction) | Absolute date axis by default with selectable format, shared time origin, missing values skipped and counted. |
| Recall | Main, Isotopes (+ intermediate stages), History, Meta, Regressions, Extraction, DetectorIC, Error Components, ICFactor, PeakCenter tabs (`analyses/view/analysis_view.py:276-378`) | All of it | Built from a Qt-free `RecallModel`; fit edits from the evolutions tab become pending revisions instead of being lost (`regression_view.py:143-152`). |
| Browser | PI/project/sample/irradiation/level/load/spectrometer/type/date filters, fuzzy search, recent, find references, named analysis sets, configurable columns (`envisage/browser/*`) | All filter dimensions, configurable columns, saved selections, next/previous recall | Paging newest-first with a cursor (legacy LIMIT returns the *oldest* 500 and a wrong total, `dvc_database.py:1665-1672`); no pickles; a source interface so files and the database browse the same way. |
| Pipeline | Linear node chain over a mutable `EngineState` bag (`state.py:34-71`), dialogs inside `run()`, no snapshots so Run From is wrong, ~20 node bugs | Templates, Run From, enable/skip, review checkpoint, fit -> review -> persist separation, auto/listen pipelines | Typed ports, pure units, results cached by fingerprint so any option change recomputes only what depends on it (section 7). |

## 3. Scope

V1 (this document's first stage):

- `libs/reduction`: group statistics (section 5).
- `libs/processing` (new, Qt-free): analysis model, sources, quantities,
  datasets, option schemas and presets, units and the runner, the scene
  model, the time-series figure, the recall model.
- `pychron::processing_records`: an `IAnalysisSource` over a records
  directory (`FilePersister` layout).
- `apps/pychron-ui`: data browser, recall window, figure window with the
  schema-driven options dock.

Later stages (section 14): ideogram, spectrum and inverse isochron scenes;
the database source with server-side browse; history and diff; blank,
IC-factor and isotope-evolution fit units with review and persist;
pipeline template editor; XY scatter; listen/auto pipelines.

Out of scope: flux visualization and flux fitting (own spec), MassSpec
transfer, report/email nodes, AusGeochem.

## 4. Architecture

```
            apps/pychron-ui (Qt)
   DataBrowserWindow  RecallWindow  FigureWindow  OptionsDock (generated)
        |                 |              |  SceneView (QCustomPlot)
        |                 |              |
        +--------- ProcessingBridge (worker thread, cancel, coalescing)
                          |
   libs/processing (Qt-free) ---------------------------------------------
     source/     IAnalysisSource: browse, facets, load, load_raw
     model/      Analysis (immutable), ReducedAnalysis (stages as UFloat)
     quantity/   Quantity: "Ar40/Ar36", "age", "gain.H1", ... -> value +- error
     dataset/    Dataset: analyses + grouping + exclusion (immutable)
     options/    Schema, Options (value tree), PresetStore (TOML, versioned)
     units/      Unit (typed ports, pure execute), Pipeline, Runner (cache)
     figures/    Scene model; builders: time_series (V1), ideogram, spectrum,
                 isochron (V2)
     recall/     RecallModel (tables for the recall window)
   libs/reduction  fits, reduce(), stats (weighted mean, MSWD, plateau, York)
   adapters:   processing_records (AnalysisRecord files, links experiment)
               processing_store   (IStore, links persistence; V2)
```

Rules:

- `libs/processing` depends on `core` and `reduction` only. The record and
  store adapters are separate targets so the core library never links the
  experiment stack or Qt.
- Every value that crosses a unit boundary is immutable and shared
  (`std::shared_ptr<const T>`). Nothing mutates an analysis after load.
- No unit opens a dialog. User decisions are options (section 7.4).
- The UI never computes statistics or geometry; it renders a `Scene`.

## 5. Group statistics (`libs/reduction/stats.hpp`)

Pure functions over doubles, ported from legacy with the deviations marked in
the header. Implemented in V1 and tested against values produced by running
legacy pychron on the same inputs.

| Function | Legacy | Notes |
|---|---|---|
| `weighted_mean(v, e, kind)` | `core/stats/core.py` `calculate_weighted_mean`, `analysis_group.py:600-670` | Zero errors skipped. Returns value, error per kind, sem, sd, mswd, acceptable, n. |
| `arithmetic_mean` | `_calculate_mean(use_weights=False)` | Deviation: Sem is sd/sqrt(n) (legacy reports sd). |
| `MeanErrorKind {Sd, Sem, Msem}` | `ERROR_TYPES`, `_modify_error` | Msem = Sem * sqrt(mswd) when mswd > 1. |
| `mswd`, `mswd_limits`, `mswd_acceptable`, `mswd_probability`, `chi2_*` | `calculate_mswd`, `get_mswd_limits` (Mahon 1996 95% interval), `calculate_mswd_probability` | Incomplete-gamma chi-squared, no scipy. |
| `cumulative_probability` | `probability_curves.py` | Sum of unit-area Gaussians. |
| `find_plateau`, `plateau_mean` | `processing/plateau.py`, `argon_calculations.py:140-221` | Fleck (overlap at n sigma) or Mahon (MSWD); nsteps, gas fraction; inverse-variance or volume-fraction weighting. Deviations: excluded steps neither break the overlap nor count as steps nor enter the mean. |
| `york_fit(points, York \| NewYork \| Reed)` | `core/regression/new_york_regressor.py` | York 1969 iteration; NewYork = Mahon 1996 errors; Reed slope from the same iteration with rho = 0, MSWD-scaled errors. Returns intercept, slope, errors, covariance, x-intercept and error, MSWD, probability. |

Kernel density, the Schaen 2020 and Deino filters, radial plots and integrated
ages over UFloats come with the ideogram and spectrum stage (V2). Integrated
age and the isochron age need UFloat inputs and live in
`processing/figures/arar_groups.hpp`, not in `stats.hpp`.

## 6. Options: schemas, values, presets

Legacy's options are its most valued and most fragile part. The rewrite keeps
the user model (named presets per figure kind, tabs of settings, a table of
panels, a table of groups) and replaces the mechanism.

### 6.1 Schema

A schema describes one kind of options (a figure, a unit) as data:

```cpp
enum class FieldType { Bool, Int, Double, String, Enum, Color, Quantity };

struct FieldSpec {
  std::string key;          // "x.time_format"
  std::string label;        // "Time format"
  std::string section;      // tab in the dock: "Axes", "Appearance", ...
  FieldType type;
  OptionValue default_value;   // monostate = unset (an optional field)
  bool optional = false;       // unset is a legal value (e.g. axis limits)
  double min, max, step;       // numeric bounds
  std::vector<std::string> choices;  // Enum
  std::string help;
  std::string enabled_when;    // "x.kind == time", evaluated by the UI
};

struct ListSpec {              // a table of rows: panels, groups, guides
  std::string key, label, section;
  std::vector<FieldSpec> fields;
  std::size_t min_rows, max_rows;
  std::vector<Options> default_rows;
};

struct Schema {
  std::string kind;            // "figure.time_series"
  int version;
  std::vector<FieldSpec> fields;
  std::vector<ListSpec> lists;
  std::vector<Migration> migrations;   // version n -> n + 1, on the value tree
};
```

The dock (section 11.4) is generated from the schema: one tab per section,
one editor per field type, a table per list. Adding an option is one
`FieldSpec`; no view code.

### 6.2 Values

`Options` is a value tree: scalar fields by key, plus lists of row `Options`.
Getters read through the schema: an absent key returns the schema default; a
key not in the schema is a programming error (debug assert, `Error` in
release). Unknown keys read from a file are kept in `extra` and written back,
so a newer file survives an older program (legacy drops them silently,
`options.py:547-560`).

`Options` is a value: copying it is cheap, and editing a copy never changes
another figure (legacy shares one instance across editors and cancel does not
revert, W11). The dock edits a copy and applies it.

### 6.3 Files

TOML, one file per preset:

```toml
schema = "figure.time_series"
version = 1
name = "Air monitor"

[x]
kind = "time"
time_format = "auto"

[[panels]]
quantity = "Ar40/Ar36"
fit = "weighted_mean"
show_statistics = true

[[panels]]
quantity = "Ar40"
scale = "log"
```

- Colours are `#rrggbb` or `#rrggbbaa`; one encoding (legacy has three).
- Unset optional values are absent keys, never 0 (legacy `ymin = ymax = 0`
  means unset, `aux_plot.py`).
- `version` older than the schema: migrations run in order; newer: load what
  is understood, keep the rest in `extra`, warn.

### 6.4 Presets

`PresetStore(root)` lists, loads, saves, duplicates, renames and deletes
presets under `root/<kind>/<name>.toml`.

- Factory presets are compiled in and never written to disk; "Reset to
  factory" loads them again (legacy rewrites factory files on every start,
  W1).
- Layering: factory < lab (`<lab>/figures/<kind>/`, read-only) < user.
  A user preset with a factory or lab name shadows it.
- Names keep their case; the file name is a sanitized form and the real
  name is stored in the file (legacy lower-cases on load only, W3).
- The selected preset per kind is a UI setting (QSettings), not a file.

### 6.5 Shared vocabularies

- Colours: palette of 12 distinguishable colours (no near-white entries,
  legacy W12); group i uses `palette[i % 12]` unless the groups table
  overrides it.
- Markers: circle, square, diamond, triangle, cross, plus, star.
- Fonts: family and point size per role (title, axis title, tick, annotation);
  one "all fonts" field sets every family (legacy `fontname`).

## 7. Composable reduction units

### 7.1 Model

A unit is a pure function with declared typed inputs and outputs and an
`Options` described by its schema:

```cpp
enum class PortType { Dataset, GroupResults, Scene, Table };
using PortValue = std::variant<DatasetPtr, GroupResultsPtr, ScenePtr, TablePtr>;

class Unit {
 public:
  virtual std::string_view kind() const = 0;               // "filter"
  virtual const Schema& schema() const = 0;
  virtual std::vector<PortSpec> inputs() const = 0;        // name + type
  virtual std::vector<PortSpec> outputs() const = 0;
  virtual Result<UnitOutputs> execute(const UnitInputs&, const Options&, RunContext&) const = 0;
};
```

`RunContext` carries the analysis source, the reduction settings, a cancel
token and a diagnostics sink. `execute` must not keep state between calls.

Units register in a `UnitRegistry` by kind, as drivers do in
`DriverRegistry`; a plugin adds units without touching the runner.

### 7.2 Pipeline and runner

A `Pipeline` is a DAG of unit instances: `{id, kind, options, inputs: [(node,
port)]}`. Most are a chain (select -> reduce -> group -> figure), but one
dataset can feed several figures (an ideogram and a spectrum of the same
groups) without running anything twice.

The `Runner` evaluates a target node. Each node's output is cached under a
fingerprint:

```
fingerprint(node) = sha256("unit/1", kind, canonical(options),
                           fingerprint of each input, source generation)
```

- Change one option: that node's fingerprint changes, it and everything
  downstream recompute, everything upstream is reused. This is what makes a
  figure reactive (legacy needs a manual re-run, and Run From reuses a
  mutated state).
- Datasets carry their own content fingerprint (analysis ids, revision
  fingerprints, grouping, exclusion), so two paths to the same data share
  downstream cache entries.
- The source's `generation()` changes when new analyses arrive or a revision
  is committed, which invalidates the select units.
- Errors are values: a failed node returns its `Error`, its dependants are
  skipped, and the figure window shows the message in place.
- Cancellation: a token checked between units and inside long loops.
- The runner runs on one worker thread per figure window. Results are
  immutable, so the UI thread can hold them while the next run starts.

### 7.3 Data model

- `Analysis` (immutable): identity (uuid, identifier, aliquot, step, run id),
  type, timestamp, spectrometer, extract device, sample metadata (sample,
  project, material, PI, irradiation, level, position, load), tag, extraction
  (value, units, duration, cleanup, positions, pattern...), per isotope the
  stored values (intercept, baseline, blank, IC factor, with fit specs, n and
  provenance), detector gains, deflections, source parameters, environmentals,
  peak centers, and the reduction context it was loaded with (flux,
  production, chronology, constants; any may be absent).
- `ReducedAnalysis`: the analysis plus `reduce()`'s result and every
  per-isotope correction stage as a `UFloat` (intercept, baseline corrected,
  blank corrected, IC corrected, decay corrected, interference corrected).
  Correlations survive, so a ratio of two stages has the right error.
- `Dataset`: a vector of items, each `{shared_ptr<const ReducedAnalysis>,
  GroupPath, ExclusionState}`, plus the group names.
- `GroupPath {tab, graph, group, subgroup}` with names: legacy's
  `tab_id/graph_id/group_id/subgroup` (`pipeline/grouping.py:20-58`) as one
  value.
- `ExclusionState`: one explicit precedence instead of legacy's four fields
  (`analysis.py:492-505`):

  | Source | Set by | Precedence |
  |---|---|---|
  | `user` (include or exclude) | clicking a point, the analyses table | highest |
  | `filter` | a filter unit with `mode = omit` | middle |
  | `tag` | the analysis tag in the dataset's excluded-tag set (default omit, invalid, outlier) | lowest |

  `included()` reads the highest set source. A figure draws excluded points
  in its "excluded" style and leaves them out of every statistic.

### 7.4 Interactive edits

Clicking a point does not mutate anything. The figure window adds the
analysis to the `exclusions` option of the pipeline's `edits` unit (which sits
just before the figure in every template) and reruns. Because the edit is an
option, it is saved with the pipeline, undoable, and recomputes exactly the
downstream nodes. The same mechanism carries manual group assignment
("group selected").

The review checkpoint of legacy (`ReviewNode` veto/resume) becomes a unit
output flag `needs_review`; persist units (V3) refuse to run until the user
marks the upstream result reviewed, which is also an option.

### 7.5 V1 unit catalogue

| Kind | In -> Out | Options |
|---|---|---|
| `select` | -> Dataset | analysis ids (explicit) or a `BrowseQuery` (section 9.1) |
| `reduce` | Dataset -> Dataset | constants preset, include decay error, J error mode, excluded tags |
| `filter` | Dataset -> Dataset | rules `quantity comparator value` with and/or, mode `omit` or `remove` (no `eval`, legacy `filter.py:66-123`) |
| `group` | Dataset -> Dataset | level (group, graph, tab), key (none, identifier, sample, aliquot, analysis type, mass spectrometer, step, load, time bin), bin hours |
| `edits` | Dataset -> Dataset | user exclusions by analysis uuid, manual group assignments |
| `group_stats` | Dataset -> GroupResults | per group: quantity, mean kind (weighted, arithmetic), error kind, nsigma |
| `time_series` | Dataset -> Scene | section 8.3 |

V2 adds `ideogram`, `spectrum`, `inverse_isochron`, `xy_scatter`,
`subgroup`, `mswd_filter`. V3 adds `table`, `export_csv`, batch
isotope-evolution refits and the reference fits:

| Unit | Ports | Options |
|---|---|---|
| `isotope_evolution_fit` | analyses -> (Scene, IsotopeFits) | per isotope (a key, or a name matching every key of it): fit, error, outlier filter, goodness thresholds (max percent error, max outliers, max slope); keep_user_excluded, skip_reviewed |

After legacy FitIsotopeEvolutionNode: every included analysis's raw
signals are refitted (reading `load_raw` from the source), keeping the
points each analysis already leaves out unless told otherwise. Legacy's
goodness checks flag a refit whose percent error, outlier count or slope at
t = 0 exceeds its threshold (signal-to-baseline and curvature checks are
not ported yet). The scene plots each isotope's refitted and current
intercepts against run time, flagged ones marked. `IsotopeFits` holds per
analysis its heads, the refits (`EditedFit`, stored value, slope,
outliers), flags and the edited analysis for previews;
`IRevisionSource::save_isotope_fits` writes one intercepts revision per
analysis in one changeset (`<ISOEVO> refit Ar40(linear),...`). Saved fits,
here and from recall, are marked reviewed.


| Unit | Ports | Options |
|---|---|---|
| `blank_fit` | (unknowns, references) -> (Scene, ReferenceFits) | per isotope: fit, error; nsigma, show_current |
| `icfactor_fit` | (unknowns, references) -> (Scene, ReferenceFits) | per detector pair: numerator, denominator, standard ratio, fit, error; nsigma, show_current |

After legacy FitBlanksNode / FitICFactorNode (`references_series.py`):

- Blanks fit each reference's baseline-corrected intercept of the isotope
  against run time; IC factors fit (N / D) / standard ratio of the
  references' blank-corrected signals on the two detectors, and the
  prediction is the IC factor of the denominator detector.
- Fit kinds: preceding, succeeding (the last / first included reference at
  or before / after the unknown, clamped at the ends), bracketing average
  (mean of the two, error sqrt(eL² + eH²) / 2), bracketing interpolate
  (linear between them, error sqrt(((1 - f) eL)² + (f eH)²)), average and
  weighted mean, linear, parabolic, cubic and exponential regressions on
  hours. Polynomials are weighted by 1/σ² when every included reference has
  an error (legacy `WeightedPolynomialRegressor`), else ordinary least
  squares; exponentials are unweighted. Errors: SEM (weighted: propagated,
  sqrt(x'(X'WX)⁻¹x)), SD (SEM and the residual scatter), MSEM (SEM ×
  sqrt(MSWD) when MSWD > 1), CI (t(0.975, n − p) × MSEM) and MC (the spread
  of 500 seeded refits of references perturbed by their errors; Box–Muller
  over `mt19937_64` so every compiler draws the same numbers).
- IC mode `source_correction` (legacy `set_beta`): the Ar40/Ar36 pair's
  fit v = measured/standard gives β = ln(1/v)/ln(m40/m36) and IC factors
  (m/m40)^β = v^k, k = ln(m40/m)/ln(m40/m36), on the detectors of Ar36..Ar39,
  saved with `source_correction` set. Legacy's discrimination mode
  (`set_discrimination`) is not ported: it computes `(disc / (m40 − m36))
  ** (m − m36)`, which does not reduce to a per-amu power law; it needs a
  decision on the intended formula first.
- `skip_reviewed`: values already marked reviewed (analyses carry the
  `reviewed` flags of their blanks and IC factors) are shown but not
  refitted or saved, like legacy's `check_refit`.
- Excluded references (the references' `edits` unit) are shown hollow and
  left out of the fit; excluded unknowns are not fitted.
- `ReferenceFits` (a new port type) holds, per fitted unknown, its heads
  and one row per isotope (blanks) or denominator detector (IC factors):
  value, fit, error kind, reference detector and standard ratio (IC
  factors) and every reference shown with its exclusion.
- `IRevisionSource::save_reference_fits` writes a blanks or IC factors
  revision for every unknown in one changeset, on the heads they were
  fitted at (all or nothing, DVC spec 5.4): edited rows get value, error,
  fit, error type, references and `reviewed`; keys the revision lacks get
  new rows; manual overrides are cleared. Message
  `<BLANKS> fits=Ar40(linear),...` or `<ICFactor> fits=CDD(average)`.
- `find_references(source, unknowns, query)`: analyses of the query's types
  (blank types for blanks, air for IC factors) within N hours of any
  unknown (windows merged, legacy `bin_datetimes`), optionally on the same
  spectrometer or extract device, without invalid runs or the unknowns.

### 7.6 Templates

A pipeline saves as TOML:

```toml
schema = "pipeline"
version = 1
name = "Air time series"

[[units]]
id = "select"
kind = "select"
options = { analysis_types = ["air"], last_hours = 168 }

[[units]]
id = "reduce"
kind = "reduce"
inputs = ["select"]

[[units]]
id = "group"
kind = "group"
inputs = ["reduce"]
options = { key = "mass_spectrometer" }

[[units]]
id = "edits"
kind = "edits"
inputs = ["group"]

[[units]]
id = "figure"
kind = "time_series"
inputs = ["edits"]
preset = "Air monitor"     # or inline options
```

Templates are validated on load: unknown kinds, unknown ports, type
mismatches and cycles are errors that name the unit. V1 ships the figure
templates the UI builds for "Plot > Time Series"; the template editor is V3.

## 8. Figures

### 8.1 Scene model (Qt-free)

A figure unit's output, enough to draw without further computation:

```cpp
struct PointRef { std::string analysis; };      // what a click selects

struct PointLayer {                              // scatter with error bars
  std::vector<double> x, y, x_err, y_err;        // errors already * nsigma
  std::vector<PointRef> refs;
  std::vector<bool> excluded;
  MarkerStyle marker, excluded_marker;
  std::string label;                             // legend
  std::vector<std::string> tooltips;             // one per point
};
struct LineLayer { std::vector<double> x, y; LineStyle style; std::string label; };
struct BandLayer { std::vector<double> x, low, high; Color fill; };       // fit envelopes
struct StepLayer { ... };                        // spectrum boxes (V2)
struct EllipseLayer { ... };                     // isochron (V2)
struct TextLayer { std::string text; Anchor anchor; Font font; Color color; };
struct GuideLayer { Orientation o; double value; std::optional<double> to; LineStyle style; std::string label; };

struct Axis {
  std::string title;
  Scale scale;                                   // linear | log
  std::optional<double> min, max;                // fixed limits
  AxisFormat format;                             // number | time | category
  std::string time_format;                       // when format == time
  bool visible, ticks_visible;
};

struct Panel { std::string id; std::string quantity; double height; Axis y;
               std::vector<Layer> layers; };
struct Graph { std::string title; Axis x; std::vector<Panel> panels; };   // panels top to bottom
struct Scene { std::vector<Graph> graphs; int rows, columns; Style style;
               std::vector<std::string> warnings; };
```

- Every point knows its analysis, so the UI's click and hover map straight
  back to a uuid; exclusions are by uuid, never by renderer index (legacy
  `index.metadata["selections"]`, pain point 7).
- Statistics text is computed by the unit and placed as `TextLayer`s with a
  corner anchor; the UI may let the user drag them, and the dragged offset is
  stored in the figure options (legacy keeps overlay positions transient).
- Warnings ("12 analyses have no Ar36; not plotted") replace legacy's silent
  zeros.

### 8.2 Rendering (`SceneView`, UI)

QCustomPlot, one `QCPAxisRect` per panel stacked in a `QCPLayoutGrid`, x axes
linked within a graph, graphs in a grid per `rows x columns`.

- Click toggles exclusion; shift-drag rubber band toggles every point inside
  (legacy `RectSelectionTool`).
- Wheel and drag zoom and pan (x shared within a graph); double-click resets.
  The current view limits are view state, not options, until the user chooses
  "Keep these limits", which writes them to the panel's fixed limits.
- Hover shows the point's tooltip (run id, tag, value +- error, date).
- Right-click: include/exclude, recall analysis, reset zoom, keep limits,
  copy image, save as PDF/PNG/SVG.
- Rebuilding a scene of the same shape reuses the QCustomPlot objects and only
  replaces data, so a click-to-omit does not flicker.

### 8.3 Time series (V1)

Options (`figure.time_series`, version 1):

| Section | Field | Default | Notes |
|---|---|---|---|
| Axes | `x.kind` | `time` | `time` (calendar), `relative` (hours from `x.origin`), `index` (run order) |
| | `x.origin` | `last` | `first`, `last`, `now`; relative kind |
| | `x.time_format` | `auto` | `auto` or a strftime pattern (`%Y-%m-%d %H:%M`) |
| | `x.min`, `x.max` | unset | ISO-8601 or hours, per kind |
| | `x.padding_percent` | 2 | |
| | `x.title` | auto | |
| Layout | `graph_columns` | 1 | graphs per row when grouped by graph |
| | `panel_spacing` | 4 | px |
| | `title` | empty | `{identifier}`, `{sample}`, `{group}` placeholders |
| Appearance | fonts (title, axis, tick, annotation) | | |
| | `background`, `plot_background`, `show_grid` | | |
| | `error_bar_nsigma` | 1 | 0 hides error bars |
| | `excluded_style` | `ghost` | `ghost` (hollow grey), `hidden` |
| Legend | `show_legend`, `legend_location` | true, top right | |
| Statistics | `statistics_location` | top left | |
| | `statistics_sig_figs` | 4 | |

Panels table (`panels`, 1-12 rows), top to bottom:

| Field | Default | Notes |
|---|---|---|
| `quantity` | `Ar40` | any quantity expression (section 10) |
| `enabled` | true | |
| `title` | from quantity | |
| `height` | 1.0 | relative weight |
| `scale` | `linear` | `log` |
| `y_min`, `y_max` | unset | |
| `marker`, `marker_size` | circle, 5 | per-group colour unless `marker_color` set |
| `show_errors` | true | |
| `fit` | `none` | `none`, `average`, `weighted_mean`, `linear`, `parabolic`, `cubic`, `exponential` |
| `fit_error` | `sem` | `sd`, `sem`, `msem` (means); `sem`, `sd` (curves) |
| `show_envelope` | true | 1-sigma band of the fit |
| `deviation` | `none` | `none`, `absolute`, `percent` (from the group mean; legacy `use_dev`) |
| `show_statistics` | true | per group: mean +- error, n/total, MSWD (or fit intercept, slope), min/max |
| `reference_value` | unset | horizontal guide |

Groups table (`groups`, optional): colour, marker, line style per group
index; defaults from the palette.

Statistics per group and panel use `reduction::weighted_mean` /
`arithmetic_mean` for means and `reduction::fit` for curves (x in hours from
the origin), on included points only. A quantity missing for some analyses
skips them and adds a warning.

## 9. Browsing

### 9.1 Source interface

```cpp
struct BrowseQuery {
  std::string text;                         // run id / identifier / sample prefix
  std::vector<std::string> identifiers, samples, projects, principal_investigators,
      materials, analysis_types, mass_spectrometers, extract_devices, loads,
      irradiations, levels, repositories;
  std::optional<double> from, to;           // UTC epoch seconds, inclusive
  std::optional<double> last_hours;         // relative "recent" window
  std::vector<std::string> exclude_tags;    // default {"invalid"}
  int limit = 200;
  std::optional<BrowseCursor> after;        // keyset paging, newest first
};

struct AnalysisSummary {                    // one browser row
  std::string uuid, runid, identifier, sample, project, material, analysis_type,
      mass_spectrometer, extract_device, load, irradiation, tag;
  int aliquot, increment;
  double timestamp;
  std::optional<double> extract_value;
  std::string extract_units;
};

struct BrowsePage { std::vector<AnalysisSummary> rows; std::optional<BrowseCursor> next;
                    std::optional<std::size_t> total; };

class IAnalysisSource {
 public:
  virtual std::string name() const = 0;
  virtual std::uint64_t generation() const = 0;
  virtual Result<BrowsePage> browse(const BrowseQuery&) = 0;
  virtual Result<std::vector<std::string>> facet(Facet, const BrowseQuery&) = 0;
  virtual Result<std::shared_ptr<const Analysis>> load(const std::string& uuid) = 0;
  virtual Result<RawData> load_raw(const std::string& uuid) = 0;   // lazy
  virtual Result<void> refresh() = 0;       // rescan; bumps generation if anything changed
};
```

- Newest first, keyset paged on `(timestamp, uuid)`; the total is exact when
  cheap. "Load more" fetches the next page.
- `facet` lists the distinct values of one dimension under the other filters
  (projects for the selected PIs, levels of an irradiation...), feeding the
  filter lists like legacy's cascading selectors, without legacy's click-order
  dependence (pain point 11): each facet is computed from all other filters.
- `load_raw` is separate because raw series are the bulk of the data and only
  recall and isotope-evolution figures need them.
- Sources are thread-safe for concurrent `load`; the store source opens one
  connection per thread (store rule).

### 9.2 Sources

- `RecordDirectorySource(root)` (V1): scans
  `<root>/<identifier>/<runid>.json` (`FilePersister`), indexes summaries in
  memory, parses records on `load`, holds raw series from the record. No
  flux or production in a record: ages are absent unless a reference file is
  configured (`references.toml`: J per identifier, production ratios and
  chronology per irradiation; V1 keeps this minimal).
- `StoreSource(config)` (V2): `IStore` gains `browse(BrowseRequest)` (SQL
  over `analysis`, `identifier`, `sample`, `project`,
  `principal_investigator`, `material`, `irradiation_position`, `level`,
  `load`, `extract_device`, `repository_member`, head tag), `facet`,
  `load_analysis_detail`, `load_blob(sha)` and `latest_change_seq`. An
  analysis' sample is its identifier's, else its irradiation position's.
  `load` combines the detail, the head payloads (manual overrides applied)
  and the reference payloads `resolve_refs` picks (flux, production,
  chronology, gains; acquisition gains win over the reference) into one
  `Analysis`. Calls run on a small pool of worker threads that each own a
  connection; `refresh` reads the change log from where the source opened
  and drops cached analyses when anything changed. `pychron-ui --db <url>`
  selects it at start-up.

### 9.3 Saved selections

"Analysis sets" (legacy `analysis_table.py:117-185`): a name plus run ids
and uuids, saved as TOML under the user directory; the last N selections are
kept automatically. Named queries save a `BrowseQuery`.

## 10. Quantities

One vocabulary for figure panels, filter rules, browser columns, statistics
and exports. Legacy has at least five (`YTITLES`, `set_names_via_keys`,
`index_attrs`, `get_value`, `ATTR_MAPPING`) and returns 0 for anything
unknown.

```
quantity  := term ( "/" term )?                 ratio of two terms
term      := isotope ( "." stage )?
           | name
           | name "." key                       gain.H1, deflection.H1, source.trap,
                                                env.lab_temperature, peak_center.H1
isotope   := "Ar40" | "Ar39" | ... | detector ":" isotope   ("H1:Ar40")
stage     := intercept | baseline | blank | ic_factor
           | bs_corrected | bk_corrected | ic_corrected | decay_corrected
           | interference_corrected | n
name      := age | age_w_j | age_w_position | F | kca | cak | kcl | clk
           | radiogenic_yield | rad40 | k39 | j | timestamp | aliquot | step_index
           | extract_value | extract_duration | cleanup_duration | weight
           | analysis_type_code
```

- An isotope without a stage is `ic_corrected` (legacy `get_intensity`).
- Ratios divide `UFloat`s, so correlated errors propagate (a ratio of two
  stages sharing a blank is right).
- `Quantity::parse` validates the grammar; `info()` gives a label with units
  (`⁴⁰Ar/³⁶Ar`, `Age (Ma)`); `eval(ReducedAnalysis)` returns
  `std::optional<Value>`: absent, never 0.
- `available_quantities(dataset)` lists what the dataset supports (isotopes
  and detectors present, whether ages exist) for the panel editor's
  completion list.

## 11. UI

### 11.1 `ProcessingBridge`

Owns the source and a `Runner` on a worker thread. `submit(pipeline,
target)` cancels the previous run of the same window, runs, and posts the
result with the pipeline's revision number; stale results are dropped. The
same pattern as `SpectrometerBridge` (spectrometer window spec).

### 11.2 Data browser (Window > Data)

- Left: search box (run id/identifier/sample prefix), filter lists for
  analysis type, mass spectrometer, project, sample, irradiation/level, load
  (each fed by `facet`), date range with presets (today, last 24 h, week,
  month, all), "exclude invalid".
- Centre: analyses table (`AnalysisTableModel`), configurable visible
  columns (QSettings), sorted newest first, multi-select, "Load more".
  Row colour by analysis type (legacy `use_analysis_colors`).
- Actions: Recall (double-click, Enter), Ctrl+N / Ctrl+B next and previous
  recall (legacy), Plot > Time Series (selection or all), Save selection.

### 11.3 Recall window

One window per analysis (or reuse, a setting), tabs from `RecallModel`:

| Tab | Content |
|---|---|
| Summary | run id, sample, type, date, spectrometer; computed values (age with and without J error, F, K/Ca, K/Cl, 40Ar*, radiogenic yield) with value, error, %; corrected ratios (40/39, 40/36, 40/38, 38/39, 37/39, 36/39) |
| Isotopes | one row per isotope: detector, fit, intercept, baseline, blank, IC, n; a stage selector shows any stage; an "intermediate" mode shows every stage side by side (legacy `IntermediateTabularAdapter`) |
| Evolutions | raw signal per isotope (and baseline, sniff) with the stored fit and its envelope, outliers marked, intercept at t = 0 |
| Error budget | error components (`ArArResult::age_error_components`) as % bars |
| Extraction | extraction spec and actuals, positions, durations, the response/setpoint series when present |
| Spectrometer | gains, deflections, source parameters, peak centers |
| Run | plan, scripts, conditionals installed and tripped, events (records only) |

V2 adds History and fit editing, through `IRevisionSource`
(`IAnalysisSource::revisions()`, implemented by `StoreSource`; record
directories keep no revisions):

- History: the revisions of one kind (intercepts, baselines, blanks, IC
  factors, tag, comment, signals), newest first, with author, host,
  changeset kind and message; the head is marked. One selected revision
  shows its rows as a table; two show a diff (changed cells highlighted,
  added and removed rows coloured). Every kind is shown as a
  `RevisionTable`, so one diff serves all of them. Restore makes the
  selected older revision the current one (`restore_revision`: a
  compare-and-swap head move, changeset kind `rollback`, message
  `<ROLLBACK> <kind> to revision <seq>`); no revision is written, so the
  newer revisions stay in the list and can be restored in turn. Restore is
  disabled while fit edits are pending.
- Fit editing in Evolutions (signal and baseline panels): per isotope (or,
  for baselines, per detector, applied to every isotope on it), fit kind,
  error type, outlier filter (iterations, standard deviations), and points
  left out by clicking them (or dragging a box). The window refits the raw
  series (`apply_fit_edits`) and recomputes every tab; the edits stay
  pending (title marked, Revert) until Save writes one changeset with an
  intercepts revision (signal edits) and/or a baselines revision (baseline
  edits): the rows of the heads the edits were made on with the edited
  rows' value, error, fit, error type, n, fn, outlier filter and user
  exclusions replaced and any manual override on them cleared, message
  `<ISOEVO> ...`. The commit is a compare-and-swap on those heads: if
  someone else moved either first, nothing is written and the status says
  who. Closing the window with pending edits asks whether to save them.

### 11.3a Blanks and IC factors window

Plot > Blanks... / IC factors... in the browser opens a window for the
selected unknowns: a finder bar (reference types, hours either side, same
spectrometer, same extract device, Find), the scene (one panel per isotope
or detector pair; click a reference to leave it out or put it back), a
Fits dock (named presets, `PresetBar`, shared with figure windows, above
the editor generated from the unit's schema), a References dock (run id,
type, time, an Included box doing the same edit as a click), and Save,
enabled when the source keeps revisions and something was fitted. A lost compare-and-swap
writes nothing and the status says so.

### 11.3b Isotope evolutions window

Plot > Isotope evolutions... refits the selected analyses: the summary
scene (click a point to leave its analysis out), a preview of the selected
analysis's evolutions with the refits, a Fits dock (presets, one row per
isotope), an Analyses dock (Included box, goodness flags, refits), and
Save, optionally leaving flagged analyses out. A lost compare-and-swap
writes nothing.

### 11.4 Figure window

- Centre: `SceneView`.
- Right dock: `OptionsDock` generated from the schema; preset combo with
  Save, Save as, Rename, Delete, Reset to factory; Apply on change with a
  200 ms debounce (or explicit Apply, a setting).
- Bottom dock: the figure's analyses table with exclusion and group columns;
  editing there is the same `edits` option as clicking.
- Toolbar: export PDF/PNG/SVG, copy, reset zoom.
- Status: run time, warnings count (click shows them).

## 12. Testing

- `stats`: values from legacy pychron run on the same inputs (done in V1).
- Quantities: grammar, every name and stage on a synthetic analysis, ratio
  error with correlated stages equals the UFloat result, missing values.
- Options: schema defaults, TOML round trip, unknown keys kept, migration from
  version 0, unset optional values, preset layering and factory reset.
- Runner: fingerprints stable; changing an option recomputes only downstream
  nodes (counted with an instrumented unit); errors stop dependants; cancel.
- Time series: scene for a known dataset (panel count, x in the selected
  format, excluded points not in the statistics, fit line through known
  points, warnings for missing values).
- Record source: build records with `RecordBuilder`, write with
  `FilePersister`, browse filters and paging order, facets, load and raw.
- UI (Qt Test, offscreen): browser filters and recall on a temp records
  directory; figure window renders a scene, a click excludes a point and the
  statistics text changes; options dock edits apply.

## 13. Decisions

| Decision | Choice | Reason |
|---|---|---|
| Where statistics live | `libs/reduction/stats` | Pure math beside fits and `reduce()`; the conditionals and future tables can use it. |
| New library | `libs/processing`, Qt-free | Same layering rule as every library but persistence; figure computation is testable headless. |
| Options representation | Schema-described value tree, not one C++ struct per figure | The dock, presets, templates, migrations and unknown-key preservation come from one mechanism; legacy's per-class views are its largest duplication (W8). |
| Options file format | TOML | Configs already use TOML and toml++; comments are allowed in hand-edited presets. |
| Pipeline shape | DAG with fingerprint cache | Reactive updates and correct partial re-runs; a chain is the common special case. |
| Interactive edits | Options of an `edits` unit | Reproducible, undoable, saved with the pipeline; no mutation of analyses. |
| Plot library | QCustomPlot | Already a UI dependency (GPLv3-compatible); handles stacked axis rects, error bars and PDF export. |
| Time axis | Calendar by default | The most requested legacy gap (W13); relative hours and run index remain. |
| Missing values | Skipped with a warning | Legacy plots zeros (`arar_age.py:256-306`). |
| First source | Records directory | Works today with the simulator; the database ingest path (outbox) is not done yet. |

## 14. Stages

| Stage | Content | Status |
|---|---|---|
| V1 | stats; processing core (model, quantities, dataset, options + presets, units + runner, scene, time series, recall model); record source; UI browser, recall, figure window + options dock | Done (plan: `docs/superpowers/plans/2026-10-02-data-browsing-visualization.md`) |
| V2 | ideogram, spectrum, inverse isochron (scenes, stats annotations, plateau and isochron options) | Done |
| V2 (rest) | XY scatter; `StoreSource` with `IStore::browse/facet/load_blob`; recall History + diff; editing fits in recall | Next |
| V3 | fit units (blanks, IC factors, isotope evolution) with references, review flag and persist through revisions; tables and CSV export; pipeline template editor; listen/auto pipelines | Later |

## 15. Open questions

1. Q1: Should the browser default to the database when both a database and a
   records directory are configured, or show both as sources in one list?
   Proposed: one source at a time, chosen in the browser toolbar.
2. Q2: Reference data for records without a database (J, production,
   chronology): a `references.toml` beside the records, or wait for the
   store? Proposed: minimal TOML in V1, dropped once acquisition ingests into
   the store.
3. Q3: Should figure documents (pipeline + options + view limits + overlay
   positions) be saved as one file the way legacy `save_figure` dumps JSON?
   Proposed: yes in V2, as `*.pyfig.toml`, re-runnable against the source.
