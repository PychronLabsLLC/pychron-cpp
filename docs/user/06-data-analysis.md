# Data analysis

How to find analyses, look at one, refit its isotope evolutions, fit blanks
and detector factors from reference runs, draw ideograms, age spectra and
isochrons, and write a data table. Everything here is in `pychron-ui`
(the **Data** browser) except the export, which `elctl` can also do.

Before this page: [01 Getting started](01-getting-started.md) installs and
opens the program. After it: [07 Configuration reference](07-configuration-reference.md)
lists the files mentioned below, [08 elctl reference](08-elctl-reference.md)
the command line, [10 Troubleshooting](10-troubleshooting.md) and
[11 Glossary](11-glossary.md) what to do when something looks wrong and what
the words mean.

Other pages already cover three subjects, and this one points to them
instead of repeating them:

- the publication data report (CSV or JSON): [export.md](../export.md)
- entering samples and irradiations ("packages"): [entry.md](../entry.md)
- bringing a legacy pychron database in: [legacy_import.md](../legacy_import.md)

## Contents

1. [Where the data comes from](#where-the-data-comes-from)
2. [The data browser](#the-data-browser)
3. [Recall: one analysis in detail](#recall-one-analysis-in-detail)
4. [Isotope evolutions: refitting many analyses](#isotope-evolutions-refitting-many-analyses)
5. [Blanks and IC factors from reference analyses](#blanks-and-ic-factors-from-reference-analyses)
6. [Figures](#figures)
7. [Options and named presets](#options-and-named-presets)
8. [How the reduction works](#how-the-reduction-works)
9. [Reading the numbers](#reading-the-numbers)
10. [Samples and packages](#samples-and-packages)
11. [Exporting](#exporting)
12. [Unverified or not yet implemented](#unverified-or-not-yet-implemented)

## Where the data comes from

The data browser reads from one of two **analysis sources**. Which one is
decided when the program starts; the title of the browser window says which
(`Data - <source name>`).

| Source | What it is | When you get it | Can save edits? |
|---|---|---|---|
| **Database store** | The lab's PostgreSQL database (or a SQLite file for a single computer). It holds the catalog (samples, irradiations), every analysis, and the full revision history of each analysis's fits. | `pychron-ui --db <url>`, or an install that has a `database` set (every data-reduction install does; `elctl init` and the setup wizard write it into the site file). | Yes: fits, blanks and IC factors are saved as new revisions, and old ones can be restored. |
| **Records folder** | A folder of per-analysis `.json` files that an acquisition run writes: `<data>/records/<identifier>/<run id>.json`. | When no database is given. `<data>` is `--data <dir>`, else the install's `data` folder, else `<lab>/data`. | No. Recall, figures and export work; every **Save** and **History** control is disabled with a tooltip saying a source that keeps revisions is needed. |

If the program was built without the database library it says so on
standard error and browses the records folder instead.

### A records folder needs a `references.toml`

A record file holds what was measured, not the irradiation. To get ages
from a records folder, put `references.toml` next to the identifier folders
(`<data>/records/references.toml`). The data-reduction setup profile writes a
commented starter. It is re-read when it changes.

| Table | Key | Meaning |
|---|---|---|
| `[flux."<identifier>"]` | `j`, `j_err` | J and its 1-sigma uncertainty for that identifier |
| | `position_jerr` | optional extra J uncertainty from the irradiation position; default `0` |
| `[production."<irradiation>"]` | `K4039`, `K3839`, `K3739`, `Ca3937`, `Ca3837`, `Ca3637`, `Cl3638`, `Ca_K`, `Cl_K` | each is `[value, error]`. Any other key is an error |
| `[[chronology."<irradiation>"]]` | `start`, `end` (UTC, `2026-01-10T08:00:00Z`), `power` (default `1`) | one entry per dose, in order |

An analysis with no flux has no age. An unknown with no production ratios
or no chronology is reduced without interference or decay correction for
them, gets no age, and the Recall window says why (see
[Reading the numbers](#reading-the-numbers)). A broken `references.toml`
stops the rescan and the browser shows `Rescan failed: <file>: <reason>`.
Files that fail to parse are skipped and listed as "problems"; an
`<identifier>/artifacts` folder and `*.extraction.json` files are ignored.

A database analysis carries its own flux, production ratios and chronology
from the catalog, so none of this applies to it.

## The data browser

Open it with **View > Data** in the full program. In a data-reduction install
the browser is the main window.

### Filters

The left column filters; the table shows the newest analyses first.

| Control | What it does |
|---|---|
| Search box | Analyses whose run id, identifier or sample **starts with** the text (not case sensitive). |
| Date drop-down | Any date, last 24 hours, last week, last month, last year. "Last" is counted back from the **newest analysis in the source**, not from today. |
| **Hide invalid** | On by default. Hides analyses tagged `invalid`. |
| Analysis type, Spectrometer, Sample, Identifier | Tick values to keep only those. Within one list, ticks are alternatives (or); between lists they all apply (and). Each list shows only values that exist given the other filters, but a value you ticked stays visible even if nothing matches. |
| **Rescan** | Asks the source for new analyses (the database change log, or the folder). |

The table has columns Run ID, Type, Sample, Identifier, Spec., Date (UTC),
Extract, Tag, and (hidden by default) Project, Irradiation, UUID. Right-click
a column header to show or hide columns. Rows are tinted: blanks, air,
cocktails and detector-IC runs have their own colours, and any analysis with
a tag other than `ok` has the error colour.

The browser loads one page at a time (200 analyses by default;
**Preferences** sets 20 to 5000). The status line reads
`<shown> of <total> analyses`, and **Load more** fetches the next page.
Every figure, fit and export acts on the rows you have **selected**; with
nothing selected it acts on **all rows shown (loaded)**, so press Load more
first if you want the whole set.

### The toolbar

The buttons along the top of the window each have an icon with its name
under it; hold the pointer over one for what it does. From left to right, in
three groups:

| Button | Opens |
|---|---|
| **Time series**, **Ideogram**, **Age spectrum**, **Inverse isochron** | A figure window of the selected analyses. |
| **Isotope evolutions...** | The isotope-evolution refit window. |
| **Blanks...**, **IC factors...** | The reference-fit window. |
| **Recall** (or double-click, or Enter on a row) | The recall window for the current row. **Ctrl+N** and **Ctrl+B** (Cmd on macOS) step to the next and previous row and recall it. |
| **Export** | A data report (CSV or JSON) of the selection. See [Exporting](#exporting). |

## Recall: one analysis in detail

The recall window shows what was measured and every step of turning it into
an age. Several can be open at once. Its title line is
`<run id> <sample> <type>`; an asterisk means unsaved fit edits.

| Tab | Shows |
|---|---|
| **Summary** | *Computed*: age, 40Ar\*/39ArK (F), K/Ca, %40Ar\* and so on, each with its 1-sigma, percent error and units. *Corrected ratios*: the argon ratios after all corrections. If values are missing, a note says why (no J, no production ratios, an isotope with an unknown intercept or blank, ...). |
| **Isotopes** | One row per isotope with detector, fit, number of points, and the value after each correction **stage**; choose one stage or all. Stages: `I` intercept; `Bs` baseline; `I-Bs` baseline corrected; `Bk` blank; `I-Bs-Bk` blank corrected; `IC` detector factor; `x IC` IC corrected; `Decay`; `IFC` interference corrected. A last column says where the blank came from. |
| **Evolutions** | The raw measurements against time with the fit drawn through them and the intercept at time zero. Switch between **Signal**, **Baseline** and **Sniff**. See below. |
| **Error budget** | Which inputs the age's variance comes from, as a percentage each, largest first (the isotopes, their baselines, J, the decay constant, ...). |
| **Identity**, **Extraction**, **Spectrometer** | Sample, project and irradiation; extraction settings (value, duration, cleanup, positions); gains, deflections, source settings and peak centers at the time of the run. |
| **History** | Database only. See below. |

### Editing a fit in Recall

On the **Evolutions** tab (Signal or Baseline) choose an isotope (or, for
baselines, a detector) and change **Fit** (average, linear, parabolic,
cubic, exponential), **Error** (SEM or SD), and optionally **Filter
outliers** with a number of **Iterations** and a **Std devs** cut-off. Click
a point, or drag a box around several, to leave them out of the fit or put
them back; **Include all points** resets that. The numbers on the other tabs
update at once but nothing is stored until you press **Save**. **Revert**
drops the edits. Closing the window with edits pending asks whether to save.

**Save** writes a new revision of the intercepts and/or baselines for this
analysis. If someone else saved a newer revision first, nothing is written
and the status says `Not saved: <who>`; recall the analysis again and redo
the edit.

### History

The **History** tab lists the stored revisions of one kind at a time
(Intercepts, Baselines, Blanks, IC factors, Tags, Annotation, Signals) with
sequence, date (UTC), author, host, change kind and message. Select one row
to see its values or two rows to see what differs. **Restore selected**
makes an older revision the current one; newer revisions stay in the history
and can be restored again. It is refused while edits are pending, and when
the chosen revision is already current.

## Isotope evolutions: refitting many analyses

**Isotope evolutions...** in the toolbar refits the intercepts and baselines of every
selected analysis from its raw measurements, flags the ones that look wrong,
and can save the new fits for all of them in one change.

The **Fits** panel (a form with a **preset** bar, see
[Options and named presets](#options-and-named-presets)) holds one row per
fit. A row has:

| Field | Meaning |
|---|---|
| Series | `signal` (an isotope) or `baseline` (a detector). |
| Isotope | An isotope (`Ar40`, or `H1:Ar40` for one detector) or, for baselines, a detector name. |
| Fit | `average`, `linear`, `parabolic`, `cubic`, `exponential`, or `auto_n`: use one fit when there are at least *n* points and another otherwise (fields `n_threshold` default 30, `n_true` default parabolic, `n_false` default linear). Default `linear`. |
| Error | `SEM` (standard error of the intercept; default) or `SD` (a prediction error, larger). |
| Filter outliers, Iterations, Std devs | Drop points further than *Std devs* (default 2) from the fit and refit, up to *Iterations* (default 1) times. |

Global switches: **Keep left-out points** (default on: points an analysis
already leaves out stay out), **Keep reviewed values** (off: values a person
marked reviewed are normally refitted; turn on to leave them alone),
**Error bars** (1 to 3 sigma), **Show current values** (draws the stored
intercepts beside the refit ones).

### Goodness flags

Each refitted isotope is tested against optional thresholds; a failed test
puts a flag on the analysis (the **Flags** column) and the status line counts
them. All are off until you set a number.

| Check | Flags the fit when |
|---|---|
| Flag error above (%) | the intercept's percent error exceeds the value |
| Smart filter a,b,c,d | the error is at least a x v^b + c x v + d for a value v (legacy default `0.0003,0.5,0.00005,0.015`; empty is off) |
| Flag outliers above | the outlier filter removed more points than this |
| Flag slope above (fA/s), Slope check above (fA) | a signal above the intensity is growing faster than the slope at time zero |
| Flag curvature above, Curvature at | the curvature at a point index (or at a fraction of the points when between 0 and 1) exceeds the value |
| Flag adjusted R-squared at or below | the fit's adjusted R-squared is at or below the value (not for averages) |
| Baseline error above (% of signal), then flag error from (%) | the baseline's error is large relative to the signal |
| Flag blank at or above (% of signal) | the blank is that large a share of the signal |

**Classify sniffs** adds a trained classifier: pick an isotope of the
previewed analysis and press **Good** or **Bad** under *Train the
classifier* to teach it; the training file is kept and a later refit flags
signals whose sniff it calls bad. Click an analysis to preview its signals
or baselines in the upper panel.

### Saving

**Save** writes new intercepts and baselines revisions for all refitted
analyses in one changeset. **Leave out flagged when saving** saves only the
analyses with no flag. Like every save, it needs the database store, and it
stores nothing at all if any analysis was changed by someone else since it
was loaded (`Not saved: ...`).

## Blanks and IC factors from reference analyses

Blanks and detector intercalibration (IC) factors are not measured on the
unknown: they are fitted from **reference** analyses run near it in time.
**Blanks...** and **IC factors...** in the toolbar do that for the selected
unknowns.

1. At the top, choose which reference types to look for, the window in
   hours either side of each unknown (default 10), and whether the
   references must be on the **same spectrometer** (on by default) and
   **same extract device**. Defaults: for blanks `blank_unknown`,
   `blank_air`, `blank_cocktail`, `blank`; for IC factors `air`. Press
   **Find references**. Windows around neighbouring unknowns are merged.
   Invalid analyses and the unknowns themselves are never used.
2. In the references table, untick any reference to leave it out (it is
   drawn hollow). Click a reference point in the plot to toggle it too.
3. In the **Fits** panel set one row per isotope (blanks) or per detector
   pair (IC factors).
4. Each unknown gets a **predicted** value, the fit evaluated at its own run
   time. The plot shows references, the fit with its envelope, the unknowns'
   stored values and the predicted ones, against hours relative to the
   newest run.
5. **Save** stores them as new Blanks (or IC factors) revisions in one
   changeset, marks them reviewed, and records which references were used.
   Unknowns you left out are not fitted.

Blank rows: `isotope` (default `Ar40`), `fit`, `error`. IC rows:
`numerator` detector (default `H1`), `denominator` detector (default
`CDD`), `standard_ratio` (default 295.5, the ratio the air shot should have),
`fit`, `error`, and `mode`:

- `ic_factor` (default): the factor is the measured numerator/denominator
  ratio divided by the standard ratio, fitted over time.
- `source_correction`: the Ar40/Ar36 pair is read as a source mass
  discrimination, and a factor for Ar36 to Ar39 on each detector follows from
  it by the mass ratio.

**Fit kinds** for both: `preceding` (default for blanks), `succeeding`,
`bracketing_average`, `bracketing_interpolate` (use the neighbouring
references), `average` (default for IC), `weighted_mean`, and the
regressions `linear`, `parabolic`, `cubic`, `exponential` on time in hours.
Polynomial regressions are weighted by 1/sigma squared when every included
reference has an error, otherwise ordinary least squares.

**Error kinds:** `SEM`, `SD` (the references' scatter; for a regression a
prediction error), `MSEM` (SEM times the square root of MSWD when MSWD is
above 1), `CI` (95% confidence half-width), `MC` (Monte Carlo over 500
refits of the references varied by their errors; repeatable). Interpolating
fits carry the references' own errors whatever you choose.

A row that cannot be fitted (no included reference, or fewer than the fit
needs) is reported as a warning and the others still go ahead. Also
available here: **Error bars**, **Show current values**, **Keep reviewed
values**.

## Figures

Press a figure's button in the browser's toolbar. Each opens its own window with
the figure in the middle, an **Options** dock on the right (presets and every
setting), and an **Analyses** dock below listing each analysis with its
group and an **Included** tick.

Toolbar: **Group by** (how analyses are split into groups, drawn in
different colours), **Reset view**, **Export...** (the picture as PDF or
PNG), **Export table...** (the data report of exactly these analyses; see
[export.md](../export.md)).

Every figure window reduces the selected analyses with the default
reduction settings: constants preset `default`, no decay-constant error.
The window you work in is: **select** analyses, **reduce** them, **group**
them, apply your **exclusions**, draw the figure. Changing an option redraws
within a fraction of a second; the status line says
`<n> analyses - <seconds> s` and counts warnings (hover to read them).

### Including and excluding analyses

Click a point (or drag a box) in the plot, or untick it in the Analyses
dock, to exclude it from the figure's statistics. Excluded analyses are
still drawn (as ghosts, or hidden, per *Excluded analyses* in Appearance).
Double-click a row in the Analyses dock to open that analysis in Recall. The dock says why a point is out: `excluded by
you`, `included by you`, `filtered`, or `tag: omit` or `tag: outlier`.
Analyses tagged `omit` or `outlier` start out excluded but you can put them
back. Analyses you selected are used even if they are tagged `invalid`; the
browser's **Hide invalid** filter is what keeps them out of a selection.

> A figure's exclusions live in the window. They are not stored in the
> database and are gone when the window closes. What is kept in a preset is
> the figure's **options**, not its analyses.

### Time series

Any quantity against run time, in panels from top to bottom; use it to
watch blanks, air shots, extraction values and spectrometer stability.
Default grouping: none.

Per panel: `quantity` (default `Ar40`), `enabled`, `title`, `height`,
`scale` (linear or log), `y_min`, `y_max`, marker shape, size and colour,
`show_errors`, `fit` (none, average, weighted_mean, linear, parabolic, cubic,
exponential) with `fit_error` (sem, sd, msem) and `show_envelope`,
`deviation` (plot as absolute or percent deviation from the fit),
`show_statistics`, `reference_value` (a horizontal line). Up to 12 panels.

Axes: `x.kind` is `time` (with `x.time_format`, `auto` or a strftime string),
`relative` (hours from `last`, `first` or `now`) or `index` (run number);
`x.min`, `x.max`, `x.padding_percent`, `x.title`.

Factory presets: **Default** (Ar40 and Ar40/Ar36), **Air monitor**,
**Blanks** (baseline-corrected intensities of each isotope, averaged),
**Unknowns** (age, K/Ca on a log axis, %40Ar\*), **Spectrometer** (baseline,
intercept and extract value of Ar40).

### Ideogram

A probability curve of ages (or another quantity) with the analyses drawn as
points below it. Default grouping: identifier.

| Setting | Meaning | Default |
|---|---|---|
| `quantity` | what is plotted | `age` |
| `mean`, `error_kind` | weighted or arithmetic mean; its error: `msem`, `sem`, `sd` | weighted, msem |
| `j_error_in_mean` | add the J uncertainty of the group in quadrature to the mean's | on |
| `probability` | `cumulative` (sum of each analysis's normal curve) or `kernel` (kernel density of the values; errors ignored) | cumulative |
| `x.limits` | `auto`, `asymptotic` (stop where the curve falls to `x.asymptotic_percent` of its peak, default 10) or `centered` (`x.centered_range` either side of the mean); plus `x.min`, `x.max`, `x.padding_percent`, `x.title` | auto |
| Display | `show_mean_indicator`, `show_mean_text`, `show_percent_error`, `show_mswd`, `show_probability`, `show_n`, `show_original_curve` (a dashed curve including excluded analyses), `fill_curve` | all on except probability and fill |

Panels (up to 8, top to bottom): `probability` (the curve),
`analysis_number` (points ordered by age, with error bars),
`analysis_number_nonsorted` (points in run order), and `value` (another
quantity, such as `kca`, against the age axis). Spans (up to 32) shade a
range of the age axis with a label, colour and opacity, on every panel or on
one named panel.

Factory presets: **Default**, **Probability only**, **Ages only**,
**With K/Ca**, **Presentation** (large fonts, 2-sigma bars, filled curve).

### Age spectrum

Apparent age of each heating step against the cumulative gas released.
Default grouping: aliquot (one spectrum per aliquot, steps in order).

| Setting | Meaning | Default |
|---|---|---|
| `quantity`, `gas` | the step value and the step width | `age`, `k39` (39ArK) |
| `plateau.method` | `fleck` (steps overlap at `overlap_sigma`, default 2, Fleck et al. 1977) or `mahon` (MSWD within its limits, Mahon 1996) | fleck |
| `plateau.nsteps`, `plateau.gas_fraction` | a plateau needs at least this many steps (3) and this percent of the gas (50) | 3, 50 |
| `plateau.weighting` | `inverse_variance`, or `volume_fraction` (weight by gas) | inverse_variance |
| `plateau.error_kind`, `plateau.j_error` | error of the plateau age; add J | msem, on |
| `integrated.include_excluded`, `integrated.j_error` | the integrated (total-gas) age uses excluded steps; with J | on, off |
| Display | `step_nsigma` (box height, 1 to 3), `show_plateau`, `show_plateau_text`, `show_integrated_text`, `show_weighted_mean_text` (when there is no plateau), `show_step_labels`, `dim_non_plateau`, `show_percent_error`, `show_mswd` | |
| `y.ignore_gas_percent` | the Y range ignores steps carrying less than this percent of the gas | 1 |

The plateau search finds the longest run of included steps that meets the
rule, starting from every included step. Excluded steps take no part in it.
In the Groups list, **Plateau from step** and **to step** (step letters) fix
the plateau by hand instead of searching.

Panels (up to 6): `age_spectrum`, or `value` (such as `kca`,
`radiogenic_yield`). Factory presets: **Default**, **With K/Ca**, **With
%40Ar\***, **Mahon**.

### Inverse isochron

Each analysis plotted as 39Ar/40Ar against 36Ar/40Ar with an error ellipse,
a fitted line, and the age and trapped 40Ar/36Ar the line gives. Default
grouping: aliquot.

| Setting | Meaning | Default |
|---|---|---|
| `method` | `new_york` (York 1969 with Mahon 1996 errors), `york` (basic York errors), `reed` (MSWD-scaled errors, correlations ignored) | new_york |
| `error_kind` | `se` or `mse` (errors scaled by the square root of MSWD when MSWD is above 1) | se |
| `include_j_error` | age with J uncertainty | on |
| `exclude_non_plateau` | leave out steps outside a Fleck plateau (3 steps, 50% gas, 2 sigma) | off |
| Display | `ellipse` (`1sigma`, `2sigma`, `95%`, `none`), `fill_ellipses`, `show_points`, `marker_size`, `show_envelope`, `show_results`, `show_percent_error`, `show_nominal_intercept` and `nominal_intercept` (the atmospheric 40/36 line, 298.56) | 1 sigma |
| Axes | `x.min`, `x.max`, `y.min`, `y.max` | automatic |

At least three analyses are needed. The result is an age, the trapped
40Ar/36Ar, the MSWD and the line's probability.

Factory presets: **Default**, **Filled 95%**, **Plateau steps**.

### Settings every figure shares

| Section | Setting | Meaning (default) |
|---|---|---|
| Layout | `title` (use `{graph}` for the graph's name), `graph_columns` (1), `panel_spacing` (4 px) | |
| Appearance | `font.family`, `font.title` 12, `font.axis` 10, `font.tick` 9, `font.annotation` 9, `background`, `plot_background` (`#ffffff`), `show_grid` (on), `error_bar_nsigma` (1; 0 to 3), `excluded_style` (`ghost` or `hidden`) | |
| Legend | `show_legend` (on), `legend_location` (`top_right`; `top_left`, `bottom_left`, `bottom_right`) | |
| Statistics | `statistics_location` (`top_left`), `statistics_sig_figs` (4) | |
| Groups (list, up to 32) | per group: `color`, `marker` (`auto`, circle, square, diamond, triangle, cross, plus, star), legend `label`, `fill_alpha` (70), and for spectra the plateau steps | |

Colours are `#rrggbb` or `#rrggbbaa`. A value outside its range or the wrong
type is dropped when a preset is loaded, with a warning.

### What can be plotted: quantities

Wherever a setting asks for a *quantity* it takes a small expression:

- a **named value**: `age` (analytical error only), `age_w_j` (with J
  error), `age_w_position` (with J and position error), `F` (40Ar\*/39ArK),
  `kca`, `cak`, `kcl`, `clk`, `radiogenic_yield` (%40Ar\*), `rad40`, `k39`,
  `j`, `timestamp`, `aliquot`, `step_index`, `extract_value`,
  `extract_duration`, `cleanup_duration`, `weight`;
- an **isotope** with an optional correction **stage**: `Ar40`
  (detector-corrected, the default), `Ar40.intercept`, `.baseline`,
  `.blank`, `.ic_factor`, `.bs_corrected`, `.bk_corrected`,
  `.ic_corrected`, `.decay_corrected`, `.interference_corrected`; a
  detector can be named, `H1:Ar40.intercept`;
- a **record** of the run: `gain.<detector>`, `deflection.<detector>`,
  `source.<name>`, `env.<name>` (such as `env.lab_temperature`),
  `peak_center.<detector>`;
- a **ratio** of two of these: `Ar40/Ar36`. Ratios divide values that keep
  their correlations, so the error of `Ar40.bs_corrected/Ar40.intercept` is
  right, not an independent-errors guess.

If an analysis lacks the quantity it is left out of that panel; a missing
value is never drawn as zero. The Options dock offers the list of what the
selected analyses can supply.

## Options and named presets

Every figure and fit window is driven by an **options** tree. The dock's
**preset bar** has a drop-down of named presets and the buttons **Save**,
**Save as...**, **Delete** and **Factory**.

A preset can come from three layers; when a name exists in more than one the
higher layer wins:

| Layer | Where | Who changes it |
|---|---|---|
| Factory | built into the program (the lists in [Figures](#figures)) | nobody; **Factory** reloads the factory preset of the name shown |
| Lab | `<lab>/figures/<kind>/*.toml` (in a data-reduction install, `<install folder>/figures/`) | whoever manages the lab's files; shared by copying the file |
| Yours | the program's per-user configuration folder, `presets/<kind>/*.toml` | you, with **Save** and **Save as...** |

The drop-down marks each as factory, lab or yours. **Save** and **Save as**
always write to *yours*, never over a factory or lab file; saving under a
factory name makes your version shadow it, and **Delete** (yours only) brings
the factory one back. Names may be 1 to 64 characters, not blank, with no `/`
or `\`; the file is named from the name in lower case with other characters
turned into `_`.

A preset file is plain TOML:

```toml
schema = "figure.ideogram"
version = 1
name = "My ideogram"

fill_curve = true
error_bar_nsigma = 2

[[panels]]
kind = "analysis_number"

[[panels]]
kind = "probability"
height = 2.0
```

`schema` must match the figure (`figure.time_series`, `figure.ideogram`,
`figure.spectrum`, `figure.inverse_isochron`, `figure.isotope_evolution_fit`,
`figure.blank_fit`, `figure.icfactor_fit`). Dotted keys such as `x.limits`
are written as a table `[x]`. An unknown key is kept and written back
unchanged; a bad value is dropped with a warning shown on the status line
(hover for the details); a preset from a newer version of the program also
loads with a warning. A file with the wrong `schema` is an error.

The settings are listed under each figure above; the tables use the file's
key names.

## How the reduction works

"Reducing" an analysis turns what the spectrometer measured into an age. The
steps are the same everywhere (Recall, figures, export), implemented in
`libs/reduction` (arithmetic and ages) and `libs/processing` (what to
reduce, with which stored values).

### 1. Fit each isotope's evolution to get the intercept

During a run each isotope is measured repeatedly on its detector. The
signal as a function of time since the gas was admitted is fitted and
**evaluated at time zero**; that intercept is the isotope's value (fA, or
counts per second on an ion counter). Fit choices are `average`, `linear`,
`parabolic`, `cubic`, `exponential` (decaying to a constant), and the error
is `SEM` (the standard error of the fitted value at time zero) or `SD`
(a prediction error). An optional **outlier filter** removes points further
than a number of standard deviations from the fit and refits, repeating a
set number of times. The same function gives the live number during
acquisition and the stored one, so they agree.

A **baseline** (the detector's reading with the beam off) is fitted the same
way, usually as an average.

### 2. Subtract baseline and blank, apply the IC factor

For each isotope, in this order:

1. **Baseline corrected** = intercept - baseline. The baseline's own error
   propagates only when the isotope is marked to include it; otherwise its
   nominal value is subtracted.
2. **Blank corrected** = baseline corrected - blank. The blank is what the
   extraction line and spectrometer contribute with no sample. It is **not**
   subtracted for analyses whose type begins `blank`, `detector_ic` or
   `background`, since those are the measurements of the blank and the
   detectors themselves.
3. **IC corrected** = blank corrected x discrimination x IC factor. The IC
   factor puts every detector on the scale of the reference detector
   (see [Blanks and IC factors](#blanks-and-ic-factors-from-reference-analyses)).

A stored value can carry a **manual override** of its value or its error;
when set, it replaces the fitted one everywhere. The history shows it.
Optional **dead-time** correction exists in the reduction library but is off
unless a dead time is supplied for the detector.

If an argon isotope's intercept, baseline, blank or IC factor is unknown,
that analysis has no reduced values (the Summary says which input is
missing) rather than a wrong number.

### 3. Correct 37Ar and 39Ar for decay

37Ar (half-life about 35 days) and 39Ar (269 years) decay between the end of
irradiation and the measurement. Each is corrected back using the
irradiation **chronology** (the doses with start, end and power) and the
analysis date; with several doses each is weighted by its power and duration
(McDougall and Harrison 1999, eq. 3.22). Decay is measured from the **start**
of each dose by default; the *Decay from the irradiation end* option on the
reduce step (`use_irradiation_endtime`) changes that.

### 4. Remove interfering reactions

Calcium, potassium and chlorine in the sample react in the reactor to make
argon isotopes that are not from the sample's own 40K decay. The
**production ratios** of the irradiation level correct for them: (36Ar/37Ar)Ca,
(38Ar/37Ar)Ca, (39Ar/37Ar)Ca, (40Ar/39Ar)K, (38Ar/39Ar)K, (37Ar/39Ar)K,
(36Ar/38Ar)Cl, with Ca/K and Cl/K for the K/Ca and K/Cl ratios. From the
corrected 37Ar, 39Ar and 38Ar the reduction takes out the calcium- and
potassium-derived argon, splits what is left of mass 36 and 38 into
atmospheric and chlorine-derived parts (the chlorine part uses the 36Cl
decay constant and the time since irradiation), and optionally a
solar/cosmogenic split. Atmospheric 40Ar is then removed by the
atmospheric 40Ar/36Ar ratio, giving **radiogenic 40Ar\*** and the
**interference-corrected** isotopes. If the calcium-derived 37Ar comes out
zero or negative, the calcium correction is set to zero (the `legacy`
constants preset allows a negative correction instead). A "fixed
(37Ar/39Ar)K" mode, which estimates the potassium share of 37Ar from a
constant, exists in the reduction library for analyses with no usable 37Ar.

The remaining equations: F = 40Ar\*/39ArK; %40Ar\* = 40Ar\*/40Ar total; and the
age

> t = (1/lambda_K) ln(1 + J F)

where lambda_K is the total 40K decay constant (electron capture plus beta)
and **J** is the irradiation parameter from the neutron-flux monitors.

### Constants

The reduction takes its decay constants and atmospheric ratios from a named
preset. The figure windows and the browser's export use `default`;
`elctl export --constants` chooses another.

| Constant | `default` | `legacy` | `legacy_preferences` |
|---|---|---|---|
| Atmospheric 40Ar/36Ar | 298.56 +/- 0.31 | 295.5 +/- 0.5 | 295.5 +/- 0 |
| Atmospheric 40Ar/38Ar | 1575 +/- 2 | same | same |
| lambda_e (electron capture) | 5.81e-11 +/- 1.6e-13 per year | same | error 0 |
| lambda_b (beta) | 4.962e-10 +/- 9.3e-13 per year | same | error 0 |
| lambda_37Ar | 0.01975 per day | same | same |
| lambda_39Ar | 7.068e-6 per day | same | same |
| lambda_36Cl | 6.308e-9 per day | same | same |
| Fixed (37Ar/39Ar)K | 0.01 +/- 0.01 | 0.01 +/- 0.0001 | 0.01 +/- 0.01 |
| Negative Ca correction allowed | no | yes | no |

The age is reported in Ma by default. A flux record may carry its own
lambda_K total (and then it replaces lambda_e + lambda_b).

### Uncertainties and what goes into them

Every number is carried with its uncertainty and the correlations between
numbers that share an input (two ratios with the same 40Ar, for instance), so
derived values have correct errors rather than an independent-errors
estimate. The ages come in three flavours that share the same F:

| Name | Uncertainty includes |
|---|---|
| `age` | the measurement (intercepts, baselines, blanks, IC factors, constants' ratios); **not** J |
| `age_w_j` | the above plus the uncertainty of J |
| `age_w_position` | the above plus the J position (irradiation gradient) error, when recorded |

The uncertainty of the decay constant enters the ages only when the
reduction is run with *Include decay constant error* (`elctl export
--decay-error`); it is off in the figure windows. The age of the flux
monitor is never propagated.

## Reading the numbers

- **Compare ages with the right error.** Comparing two ages of the same
  irradiation, the J error is common to both and should not be counted
  twice: use `age` (analytical) for within-sample comparisons and `age_w_j`
  for comparison with other irradiations or other labs.
- **MSWD** says whether the scatter of the points is consistent with their
  quoted errors. About 1 is good. The program marks an MSWD with an asterisk
  when it is outside the 95% interval expected for that number of points
  (Mahon 1996); well above the interval means more scatter than the errors
  explain (so the quoted mean error should be **MSEM**, the standard error
  scaled by the square root of MSWD, which is the default). Well below
  usually means the errors are overestimated. For a single point there is no
  MSWD and none is shown.
- **p** (shown when *MSWD probability* is on) is the chance of a scatter at
  least this large by luck; a very small p says the points are not one
  population.
- **Ideogram**: a single clean peak means one age population; shoulders and
  extra peaks mean mixed populations, inheritance or loss. The curve is
  built from the analyses' own errors (`cumulative`), or only from their
  spread (`kernel`).
- **Age spectrum**: a **plateau** is a run of consecutive steps (at least 3
  and half the 39ArK gas by default) whose ages agree within error. A
  plateau age is a weighted mean of the plateau steps; the **integrated
  age** is the age of all the gas together, comparable to a total-fusion
  age. No plateau does not mean a bad sample: it means the gas release is
  not flat, and the integrated age or the isochron may be what you report.
- **Inverse isochron**: the line's intercept on the 36Ar/40Ar axis gives
  the trapped (non-radiogenic) 40Ar/36Ar. Near 298.56 means ordinary air
  and the plateau or weighted mean ages stand; very different from it means
  excess argon, and the isochron age is the more reliable one. The age needs
  at least three points; if the regression cannot be made the figure says
  so as a warning.
- **K/Ca** against step: a K/Ca that is steady across a plateau supports
  one phase degassing; changes mark a different phase.
- **%40Ar\*** that is very low (mostly atmospheric) makes ages poorly
  determined; look at the error, not just the value.
- **Tags**: `invalid` analyses are hidden by default; `omit` and `outlier`
  analyses stay in the table and in the figures as excluded points, and are
  marked `included = no` in the export, so nothing is quietly thrown away.
- **Why an analysis has no age.** Recall's Summary gives the reason in plain
  words: no J for the identifier, no production ratios or chronology for the
  irradiation, an isotope with an unknown value, a zero 39Ar, or
  `1 + J F` not positive. Unknowns without ratios or chronology are still
  reduced (without those corrections) so that their intensities can be
  inspected, but they are never fed into group ages.
- **A figure window and the report agree** because they use the same
  reduction and grouping; the report's note column explains each missing
  age.

A data table that follows the Schaen et al. (2021) reporting standard is
the right deliverable for a paper; see [export.md](../export.md).

## Samples and packages

Entering the samples (and their locations, materials, projects) and
irradiation **packages** (levels, positions, flux monitors, J values) lives
under the **Entry** menu in the full program when it is connected to a
database: **Samples...**, **Packages...**, **Import samples...**, the holder
list, and **Entry Settings...**. The packages window can write the level
sheets as a PDF. Everything about it, including the `elctl entry` commands,
is in [entry.md](../entry.md). The data browser needs nothing from you
there except that the samples and flux were entered: an unknown without
a J cannot have an age, and a sample without a location leaves empty
location columns in the export.

## Exporting

Three ways to get numbers out, all producing the same report (see
[export.md](../export.md) for what every column means):

- **Export** in the data browser: the selected analyses (or all shown),
  grouped by aliquot if any is a heating step, otherwise by identifier;
  file name ends `.csv` or `.json`. It is named after the sample when the
  selection has only one.
- **Export table...** in a figure window: the figure's own analyses, grouping
  and exclusions (a spectrum also uses its plateau and weighting options).
- `elctl export` from the command line, with filters and `--constants`,
  `--sigma`, `--decay-error` and `--plateau*` options; see
  [08 elctl reference](08-elctl-reference.md).

The picture of a figure (**Export...** on the figure toolbar) is a PDF or
PNG. A file the program writes for you to open elsewhere is marked as a user
file, so a downloaded, unsigned macOS program does not leave it quarantined.
If macOS refuses to open it anyway, see
[the installation runbook](../installation_runbook.md) and
[10 Troubleshooting](10-troubleshooting.md).

## Unverified or not yet implemented

- **Pipelines.** The processing library composes select, reduce, filter,
  group, edit, statistics and figure steps into pipelines that can be written
  as TOML (`pipeline_to_toml`), but the program has no screen for choosing,
  saving or loading a pipeline. The figure windows build a fixed one. The
  *Filter* step (rules on a quantity, such as dropping analyses with
  `Ar40 < 1e4`) is therefore available to code, not to a user; the only
  filters you can set in the interface are the browser's.
- **Reduction settings in the interface.** The figure windows and the
  browser's Export always use constants preset `default`, no decay-constant
  error, and decay from the start of each dose. Only `elctl export` can
  change them.
- **Preset menus for the reduction.** There is no UI to edit production
  ratios, J or the chronology of a database analysis from the data
  browser; use the entry windows ([entry.md](../entry.md)).
- **Saving from a records folder.** Fits, blanks and IC factors cannot be
  saved to a records folder, only to the database.
- **Dead-time correction.** The reduction supports it, but the browser has
  no control to turn it on.
- **Live evolutions during a run.** The experiment window's evolutions panel
  (signal, baseline, sniff and peak-center results of the run in progress)
  is described in [04 Experiments](04-experiments.md), not here.
- **Where the per-user preset folder is.** It is Qt's per-user
  configuration location for the application (`presets/` below it); the
  exact path differs by system and was not checked on every platform.
- The README the data-reduction profile writes says "File > Data Browser".
  In the code the browser is the main window of a data-reduction install and
  **View > Data** of the full program; if the menu you see differs, trust
  the menu.
