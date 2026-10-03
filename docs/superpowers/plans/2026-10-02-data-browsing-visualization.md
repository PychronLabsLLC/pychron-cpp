# Data Browsing and Visualization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Find analyses, recall one in depth, and plot many with figures as
customizable as legacy pychron's, built from composable reduction units.

**Architecture:** group statistics in `libs/reduction`; a new Qt-free
`libs/processing` (model, quantities, datasets, schema-described options and
presets, units with a fingerprint-cached runner, scenes, recall model);
source adapters as separate targets (`processing_records`,
`processing_store`); the Qt UI only renders scenes and edits options.

**Spec:** `docs/superpowers/specs/2026-10-02-data-browsing-visualization-design.md`.

## Global constraints

- `libs/processing` depends on `core` and `reduction` only. Adapters that
  need the experiment or persistence libraries are separate targets.
- No unit opens a dialog or keeps state between calls; user decisions are
  options (the `edits` unit carries clicks).
- Every schema default must validate, and every factory preset must load
  without warnings (`Options.FactoryPresetsAreClean`).
- Missing values are absent, never 0. A figure reports what it could not plot
  as a scene warning.
- Do not name a parameter `signals`, `slots` or `emit` in a public header:
  the UI includes these headers with Qt's keyword macros active.

## Done (2026-10-02, stage V1)

### Statistics (`libs/reduction/stats.hpp`)

- [x] Weighted and arithmetic means with SD/SEM/MSEM; MSWD; Mahon 1996
      limits; MSWD probability; chi-squared without scipy.
- [x] Cumulative probability curves.
- [x] Fleck and Mahon plateaus; inverse-variance and volume-fraction plateau
      means.
- [x] York, NewYork (Mahon 1996) and Reed regressions with x-intercept.
- [x] Tests against values from legacy pychron run on the same inputs.

### Processing core (`libs/processing`)

- [x] `Analysis`, `RawData`; `reduce_analysis` with every correction stage
      as a UFloat.
- [x] `Quantity` grammar, labels, units, `available_quantities`.
- [x] `Dataset` with group paths and exclusion precedence.
- [x] `Schema`/`Options`/`PresetStore`: validation, TOML round trip,
      unknown keys kept, migrations, factory < lab < user.
- [x] `Unit`, `UnitRegistry`, `Pipeline` (validate, order, TOML), `Runner`
      (fingerprint cache, errors as values, cancel).
- [x] Units: `select`, `reduce`, `filter`, `group`, `edits`, `group_stats`,
      `time_series`.
- [x] `Scene`; `build_time_series` (time/relative/index axes, fits with
      envelopes, deviation, groups, statistics, warnings).
- [x] `RecallModel`, `make_evolution_scene`.
- [x] `MemorySource` (tests, previews).

### Record source (`pychron::processing_records`)

- [x] `RecordDirectorySource` over `<data>/records`: incremental rescan,
      paging, facets, raw series, `references.toml` (flux, production,
      chronology).

### UI (`apps/pychron-ui`)

- [x] `SceneView` (QCustomPlot): stacked panels, linked x, date axis, log
      axes, error bars, bands, statistics text, legend; click, shift-drag,
      hover, zoom/pan, reset, PNG/PDF.
- [x] `OptionsEditor` generated from a schema, with row lists and
      `enabled_when`.
- [x] `ProcessingBridge`: worker thread, per-channel coalescing and cancel.
- [x] `DataBrowserWindow`, `RecallWindow`, `FigureWindow`; Window > Data in
      `MainWindow`; records and presets wired in `main.cpp`.
- [x] Qt Test suite `test_data_windows`.

## Remaining

### V2 figures (done 2026-10-02)

- [x] `arar_groups`: integrated (total-gas) age, J error in means,
      inverse-isochron points with exact UFloat correlations, isochron age
      (York fit of x on y, legacy calculate_isochron); kernel density.
- [x] Ideogram unit and scene: cumulative or kernel curve, dashed curve with
      excluded analyses, weighted-mean indicator with MSWD/n/p, J error in
      the mean, analysis-number (sorted, by time) and value panels,
      auto/asymptotic/centered limits.
- [x] Spectrum unit and scene: `StepLayer` boxes at n sigma, Fleck/Mahon
      plateau or fixed steps per group, plateau bar and text, integrated
      age, weighted mean when there is no plateau, value spectra.
- [x] Inverse isochron unit and scene: `EllipseLayer` (1, 2 sigma, 95%),
      York/NewYork/Reed fit with envelope, trapped 40/36 and age,
      exclude-non-plateau, atmospheric marker.
- [x] Figure window for any figure kind (default grouping per kind);
      browser Plot menu; step boxes clickable; x error bars.

### V2 figures remaining

- [ ] XY scatter (any two quantities) and `subgroup`, `mswd_filter` units.
- [ ] Movable annotations: dragged text offsets stored in figure options.
- [ ] Figure documents (`*.pyfig.toml`: pipeline + options + view limits),
      open question Q3.
- [ ] Ideogram: peak labels, Schaen 2020 / Deino outlier filters, inset;
      spectrum: integrated weighting by volume or variance, isochron-trapped
      spectrum ages; isochron: inset, normal isochron.
- [ ] Composite figure (spectrum and isochron side by side).

### V2 data (store source done 2026-10-02)

- [x] `IStore::browse(BrowseRequest)` and `facet` in SQL (joins over
      identifier, sample, project, PI, material, irradiation_position, level,
      load, extract device, repository_member, head tag), keyset paged on
      (timestamp, uuid), totals on request, facets ignoring their own filter;
      `load_analysis_detail`, `load_blob(sha)`, `latest_change_seq`.
      Tested on SQLite and PostgreSQL (`tests/persistence/test_browse.cpp`).
- [x] Catalog writes for principal investigators, projects, materials,
      samples, extract devices; identifiers and irradiation positions carry
      a sample (the browse sample is the identifier's, else its position's).
- [x] `StoreSource` (`pychron::processing_store`): a pool of worker threads
      each owning a connection; load assembles detail + head payloads
      (manual overrides, fit and outlier settings) + `resolve_refs`
      (flux, production, chronology, gains); raw series from blobs with
      start/end windows; `refresh()` follows the change log and drops the
      load cache (`tests/processing/test_store_source.cpp`).
- [x] `pychron-ui --db <url>` browses a store instead of the records
      directory (the schema must be current; it is not migrated).
- [x] Recall History tab (`IRevisionSource::history` per kind, author and
      host from the store) and a diff of two revisions (`diff_revisions`).
- [x] Editing fits in the Evolutions tab: fit kind, error type, outlier
      filter, click-to-exclude points; pending until saved as an intercepts
      revision on the loaded head (conflicts reported, nothing written).
- [x] Restoring an older revision from History (`restore_revision`, a
      CAS head move recorded as a `rollback` changeset), and editing
      baseline fits per detector, saved with intercept edits in one
      changeset.
- [x] Editing blanks and IC factors: through the V3 reference fits (below).
- [ ] Saved selections and named queries (spec 9.3).
- [ ] Browser source picker in the UI when both a database and records
      exist (Q1); today `--db` chooses at start-up.

### V3 reduction workflows

- [x] Blank and IC factor fits from references (`blank_fit`,
      `icfactor_fit` -> Scene + `ReferenceFits`), `find_references`,
      `IRevisionSource::save_reference_fits` (one changeset, reviewed set),
      and the Blanks / IC factors window from the browser's Plot menu.
- [x] Reference fits: weighted polynomial regressions, CI and Monte Carlo
      errors, presets in the window (`PresetBar`), source-correction IC
      mode, keep reviewed values, references table dock.
- [ ] Discrimination IC mode: decide the formula (legacy's
      `set_discrimination` power is suspect, see spec 7.5).
- [x] Batch isotope-evolution refits (`isotope_evolution_fit` -> Scene +
      `IsotopeFits`, goodness flags, `save_isotope_fits`) and the Isotope
      evolutions window.
- [ ] Remaining legacy goodness checks (signal-to-baseline, curvature,
      smart filter) and baseline batch refits.
- [ ] Tables and CSV export units.
- [ ] Pipeline template editor (graph view of units, per-node options dock).
- [ ] Listen/auto pipelines driven by `IAnalysisSource::generation()`.

## Notes for implementers

- A unit's options are part of its fingerprint through
  `Options::canonical()`; anything that changes a result must be an option
  or an input, never hidden state.
- Units that read the source must return `reads_source() == true` so the
  source generation enters their fingerprint.
- `ProcessingBridge` evaluates several targets per job with one runner, so a
  window can ask for its scene and the dataset behind it without computing
  twice.
- `SceneView` keeps the zoom when a new scene has the same panels (a
  click-to-omit rerun); a different shape resets the view.
