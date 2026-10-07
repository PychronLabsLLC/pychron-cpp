# Flux fitting

Date: 2026-10-06
Status: Implemented (owner decisions in section 3; implementation notes in section 12)
Owner: Jake Ross
Depends on: `2026-10-01-dvc-schema-design.md` (reference data 6, `flux_position`
6.1, reference resolution and pins 6.2, derived cache 4.3),
`2026-10-02-arar-reduction-design.md` (F, `Flux`, `make_j`, 5.5),
`2026-10-02-data-browsing-visualization-design.md` (`IAnalysisSource`,
`StoreSource`, group statistics 7), `2026-10-04-sample-irradiation-entry-design.md`
(levels, positions, holders, the entry settings document).
Scope: computing J for the positions of one irradiation level from its
fluence monitor analyses, and saving it. A Qt-free math layer in
`libs/reduction`, orchestration in `libs/processing`, and `elctl flux`.
Out of scope: a flux window in `pychron-ui`, contour and vertical-flux
plots, Monte Carlo errors, position error, interpolant models (RBF,
GridData, IDW, high-order polynomial, B-spline), a joint fit of several
levels, MassSpec flux transfer, and the legacy "freeze flux" action. See
section 11.

Legacy citations are `file:line` relative to the `pychron/` package of the
legacy Python repo (read-only), at commit `26e77ad17`.

## 1. Goal

A lab whose data lives in the DVC store can turn the monitor analyses of an
irradiation level into a J for every position of that level without the
legacy Python application, see what the fit did, and save it so that every
unknown of the level is reduced with the new J.

Success: on a store holding level `NM-300 A` with FC-2 monitors in eight
holes and unknowns in the rest,

```
elctl flux fit NM-300 A --model plane --weighted
```

prints a monitor table (hole, identifier, n, saved J, mean J, MSWD, predicted
J, deviation) and an unknown table (hole, identifier, saved J, predicted J,
deviation), and with `--save` writes one `flux_position` revision per
position in one changeset. The ages of the level's unknowns then use the new
J; an analysis whose flux is pinned keeps its pinned J. Running the same
command again reports no change and writes nothing.

## 2. What legacy does (summary of the survey)

One irradiation and one level per run: find monitors, fit, review in an
editor, save (`pipeline/pipeline_defaults.py:347-357`).

| Area | Legacy behaviour | Problems to avoid |
|---|---|---|
| Finding monitors | analyses of the level whose **sample name** equals the monitor name (default `FC-2`), tag not `invalid` (`dvc/dvc_database.py:2734-2757`); or picked by hand in the browser; or every position (`pipeline/nodes/find.py:370-381`). Unknowns are the positions with any other sample (`dvc_database.py:2874-2890`) | with "all positions" a position can be in both tables |
| Geometry | hole x, y from `irradiation_holders/<holder>.txt`, looked up as `geom[hole_id - 1]` (`pipeline/editors/flux_results_editor.py:284`), that is, position N is the Nth hole | none: entry, the UI grid and this port place position N at the Nth hole too (section 12, R12) |
| Per-analysis J | `J = (exp(lambda_k * t) - 1) / F` (`processing/argon_calculations.py:223-246`); monitor age and `lambda_k` enter as plain numbers | zero F returns `J = 1` (`:245-246`) |
| Position mean | J (not F) of the non-omitted analyses averaged, arithmetic or inverse-variance weighted (`processing/flux.py:22-43`); error SEM, SD, or SEM scaled by `sqrt(MSWD)` when MSWD > 1, the default (`core/regression/mean_regressor.py:105-135`); no automatic outlier rejection | arithmetic mean of one analysis has error 0 (`core/regression/base_regressor.py:137-149`); the arithmetic mean's MSWD is taken about the weighted mean (`core/stats/core.py:40-41`); SD of a weighted mean is the unweighted SD (`mean_regressor.py:130-131`) |
| Models | thirteen option strings (`pychron_constants.py:367-395`). Nine produce a J: Plane, Bowl, Weighted Mean, Matching, Nearest Neighbors, Bracketing, LeastSquares1D, WeightedMean1D, Bracketing1D | RBF, GridData, IDW and "Order 5 Polynomial" assign no J unless Monte Carlo is on (`pipeline/editors/flux_visualization_editor.py:301-302`) |
| Fit errors | `sef * sqrt(x C x')`, optionally scaled by `sqrt(MSWD)` (`core/regression/ols_regressor.py:172-218`) | that MSWD is the scatter of the position J about their **mean**, over `n - q` (`base_regressor.py:563-574`), so a real gradient inflates the error; a weighted plane mixes unweighted residuals with the weighted covariance; Bowl and LeastSquares1D ignore the weighted option; with `n <= q` the error is silently 0 (`base_regressor.py:397-398`); a position with zero error gives an infinite weight |
| Monte Carlo | perturb the monitor means, refit, half the 16-84 percentile width; 10 trials by default, unseeded (`core/stats/monte_carlo.py:26-109`) | not reproducible |
| Saving | one JSON per level in the meta repo, each position `j`, `j_err`, `mean_j`, `mean_j_err`, `mean_j_mswd`, `position_jerr`, `decay_constants`, `options`, `analyses` (`dvc/meta_repo.py:514-602`); commit `fit flux for <irrad><level>` | the monitor's name and age are written under `options` and read from a `monitor` dict nothing writes (`meta_repo.py:691-697`), so every analysis reports FC-2 at 28.201; material is saved as the monitor name (`pipeline/nodes/persist.py:292`); the error kind of the mean, the axis, degree, neighbour count and the positions left out of the fit are not saved; a save from labnumber entry replaces the whole position dict and so erases the fit's fields (`entry/labnumber_entry.py:727-734`); already loaded unknowns keep the old J |
| Monitor constants | built-in `FC Min` (28.201 Ma) and `FC SJ` (28.02 Ma) plus a hand-edited `flux_constants.yaml` read at import (`pychron_constants.py:350-405`) | no age uncertainty anywhere; the yaml's `FC MIN` and the built-in `FC Min` both appear |

Dead in legacy and not ported: `DVC.freeze_flux` and its action
(`dvc/dvc.py:666-705`), `dvc.save_flux` (`:932-964`), the flux monitor editor
(`entry/editors/flux_monitor_editor.py`), the manual bracket columns
(`flux_results_editor.py:510-535`), backward J transfer
(`entry/j_transfer.py:153-154`).

## 3. Decisions

Owner decisions of 2026-10-06 are marked (owner).

| # | Decision |
|---|---|
| F1 | (owner) **Nine models**: Plane, Bowl, Weighted Mean, Matching, Nearest Neighbors, Bracketing, LeastSquares1D, WeightedMean1D, Bracketing1D. These are every model a lab can have saved a J with. The four that need Monte Carlo to produce anything are not ported. |
| F2 | (owner) **Corrected math only.** Where legacy is wrong (section 5.4) the port is right and there is no switch that reproduces the legacy number. |
| F3 | (owner) **Core and CLI first.** No window in this spec; the review step (omit an analysis, leave a position out) is done with command-line flags. The window is a later spec on the same two layers. |
| F4 | (owner) **`j_err` is analytical.** It comes from the monitors' F only, as in legacy. The monitor age and `lambda_k` uncertainties are systematic and common to every position of the irradiation, so they are saved beside the J (`monitor_age_err`, `lambda_k_total_err`) for an external-error stage to use, and never folded into `j_err`. |
| F5 | (owner) **No Monte Carlo and no position error.** Every kept model has a closed-form error. New fits write no `position_jerr`. An imported one stays in its imported revision; a position re-saved here gets a new head without it, so the reduction stops using it for that position (section 12). |
| F6 | (owner) **Monitor sets are a revisioned document in the store**, `pychron/flux_monitors.json`, read and written like the entry settings. Every client sees the same sets. |
| F7 | Math in `libs/reduction`, orchestration in `libs/processing`, command in `apps/elctl`: the split the ArAr reduction already has. |
| F8 | A monitor position is saved with the **model's** J as `j` and its own mean as `mean_j`, as legacy: the J of a hole is what the level's model says it is, and the monitor's deviation from it stays visible. |
| F9 | A fit is repeatable from what was saved: the options, the omitted analyses and the positions left out are all in the revision, and a fit with no model named starts from them. |
| F10 | A save is one changeset, every position or none, with a compare-and-swap on each head. |
| F12 | (owner) The default monitor sets ship with age uncertainties 0.046 Ma (Kuiper 2008) and 0.16 Ma (Renne 1998), 1 sigma as stored. |
| F13 | (owner) `Sd` is not an error kind of the least-squares models (Plane, Bowl, LeastSquares1D). Asking for it is an error; an imported fit saved with it refits with `Msem` and says so. |
| F11 | No store migration. `flux_value`, `flux_value_analysis` and the holder tables already hold everything (DVC spec 6.1). |

## 4. Monitor sets

`pychron/flux_monitors.json`, a `document` reference (entry spec section 7
pattern: `load`, `save` on top of the head read, `Conflict` when someone
saved in between). Unknown keys are kept.

```json
{
  "default": "FC-2 (Kuiper 2008)",
  "monitors": [
    { "name": "FC-2 (Kuiper 2008)", "sample": "FC-2", "material": "sanidine",
      "age_ma": 28.201, "age_err_ma": 0.046,
      "lambda_ec": [5.80e-11, 9.9e-13], "lambda_b": [4.883e-10, 1.4e-12] },
    { "name": "FC-2 (Renne 1998)", "sample": "FC-2", "material": "sanidine",
      "age_ma": 28.02, "age_err_ma": 0.16,
      "lambda_ec": [5.81e-11, 0.0], "lambda_b": [4.962e-10, 0.0] }
  ]
}
```

- `sample` is the sample name monitor analyses are found by; `name` is what
  a saved J records as `monitor_name`.
- `lambda_k = lambda_ec + lambda_b`, errors in quadrature. Pairs are
  `[value, 1 sigma]` in 1/a.
- A store with no such document behaves as if it held the two sets above
  (legacy `FC Min` and `FC SJ`, `pychron_constants.py:350-365`). The first
  save writes them.
- Validation on load and save: unique non-empty names, `default` names one
  of them, ages and decay constants positive, errors not negative. A document
  that fails is an error naming the key; it is never half used.

Legacy carries no age uncertainty. 28.201 +/- 0.046 Ma is the figure of
Kuiper et al. 2008; for Renne et al. 1998 the literature quotes 0.16 Ma
(without the decay constant uncertainty) and 0.28 Ma (with it), and 0.16 is
used since the decay constants are carried separately. The owner accepted
both defaults (F12). They affect nothing computed in this spec (F4): they are
recorded with each J, and a lab changes them with `elctl flux monitors set`.

## 5. Math: `libs/reduction` `flux.hpp`

Pure functions over doubles and `UFloat`; no store, no clock, no state. No
new dependency: the largest system solved has five parameters.

### 5.1 J of one analysis

```cpp
struct MonitorConstants { double age_a = 0; double lambda_k = 0; };  // nominal values (F4)

// J = (exp(lambda_k * age) - 1) / F, the uncertainty of F propagated.
// Error (Config) when F is zero, negative or not finite.
Result<UFloat> j_of(const UFloat& f, const MonitorConstants& monitor);
```

### 5.2 Mean J of a position

```cpp
struct MonitorAnalysis { std::string record_id; UFloat f; bool omitted = false; };
enum class MeanKind { Arithmetic, Weighted };

struct PositionMean {
  double j = 0, j_err = 0, mswd = 0;
  bool mswd_acceptable = false;
  int n = 0;                              // analyses used
  std::vector<std::string> rejected;      // record ids whose F could not give a J
};

// Error (Config) when no analysis is left.
Result<PositionMean> mean_j(std::span<const MonitorAnalysis> analyses, const MonitorConstants& monitor,
                            MeanKind kind, MeanErrorKind error);
```

The J of each used analysis is averaged (J, not F, as legacy) with the
existing `weighted_mean` / `arithmetic_mean` of `stats.hpp`, whose
`MeanErrorKind` (`Sd`, `Sem`, `Msem`) is the legacy SD, SEM and "SEM, scaled
by sqrt(MSWD) when MSWD > 1". Those functions already carry three of the
corrections of 5.4 (the MSWD about the reported mean, a real SEM for the
arithmetic mean, the single error when n = 1). An analysis whose F gives no
J is left out and named in `rejected`.

### 5.3 Models

```cpp
struct Point { double x = 0, y = 0; };
struct Monitor { std::string label; Point at; double j = 0, j_err = 0; };  // label: for messages

enum class ModelKind { Plane, Bowl, WeightedMean, Matching, NearestNeighbors, Bracketing,
                       LeastSquares1D, WeightedMean1D, Bracketing1D };
enum class Interpolation { WeightedMean, Average, Linear };
enum class Axis { X, Y };

struct FitOptions {
  ModelKind kind = ModelKind::Plane;
  bool weighted = false;                       // Plane, Bowl, LeastSquares1D: by 1 / j_err^2
  MeanErrorKind error = MeanErrorKind::Msem;   // Plane, Bowl, LeastSquares1D and the mean kinds
  int n_neighbors = 2;                         // NearestNeighbors
  Interpolation interpolation = Interpolation::WeightedMean;  // Bracketing
  Axis axis = Axis::X;                         // the three 1-D kinds
  int degree = 1;                              // LeastSquares1D, 1..4
};

struct Predicted { double j = 0, j_err = 0; };
enum class FitNote { Extrapolated, MswdOutsideLimits };
struct PointNote { std::size_t point; FitNote note; };

struct FluxFit {
  std::vector<Predicted> at;       // parallel to `predict_at`
  std::vector<double> parameters;  // surface kinds: coefficients, highest power first, constant last
  double mswd = 0;                 // surface and mean kinds; 0 for neighbour kinds
  int dof = 0;
  std::vector<PointNote> notes;
};

std::size_t minimum_monitors(const FitOptions& options);
Result<FluxFit> fit_flux(std::span<const Monitor> monitors, std::span<const Point> predict_at,
                         const FitOptions& options);
```

| Kind | Form | Minimum monitors | Predicted J and error |
|---|---|---|---|
| Plane | `a x + b y + c` | 4 (3 gives no error) | least squares; `sigma^2 = x C x'` |
| Bowl | `a x^2 + b y^2 + c x + d y + e`, no `xy` term, as legacy | 6, at more than one radius (R7) | as Plane |
| LeastSquares1D | polynomial of `degree` along `axis` | `degree + 2` | as Plane |
| WeightedMean, WeightedMean1D | one J for the level, inverse-variance | 1 | `weighted_mean` with `error` |
| Matching | the nearest monitor | 1 | its `j`, `j_err` |
| NearestNeighbors | the `n_neighbors` nearest, inverse-variance mean | `n_neighbors` | `(sum 1/sigma^2)^-1/2` |
| Bracketing | the two nearest, by `interpolation` | 2 | WeightedMean: as above. Average: mean and sample SD. Linear: `j0 + f (j1 - j0)` with `f` the projection of the point on the segment, `sigma^2 = ((1-f) e0)^2 + (f e1)^2` |
| Bracketing1D | the monitors either side along `axis`, always linear (`interpolation` applies to Bracketing only, R6) | 2 | as Bracketing Linear |

Nearness is the plain distance in x, y, as legacy
(`core/regression/flux_regressor.py:154-161`); a tie is broken by the order
of the monitors, which the caller gives by hole.

Least-squares kinds. With design matrix `X`, weights `W` (`1 / j_err^2`, or
the identity when not `weighted`), `C = (X' W X)^-1` and `r` the residuals:

- The error kinds, with
  `s2 = r' W r / (n - q)` (weighted: the reduced chi-squared, so `s2` is the
  MSWD; unweighted: the residual variance):
  - weighted, `Sem`: `x C x'`. `Msem`: `x C x' * max(s2, 1)`.
  - unweighted, `Sem` and `Msem`: `s2 * x C x'` (the residual variance is the
    only scale there is, so the two are the same).
  - `Sd` is an error for Plane, Bowl and LeastSquares1D (F13): the scatter of
    one position about a fitted surface has no defensible definition when
    the fit is weighted, and legacy's is not one. The mean kinds keep it.
- `mswd` reported for an unweighted fit is `sum((r / j_err)^2) / (n - q)`:
  the monitors' errors judge the fit even when they did not weight it. A
  monitor with zero error is used in the fit but skipped in this sum, whose
  divisor is the monitors with non-zero error minus q (R5).
- A fit that barely determines the surface is refused, not only an exactly
  degenerate one: when the smallest pivot of the column-equilibrated design
  is below 1e-3 of the largest, or when any predicted J is not finite and
  positive, the error is `monitor positions do not determine a <model>` (R7,
  R8). A single ring of monitors cannot determine a Bowl, since `x^2 + y^2`
  is constant on it.
- `MswdOutsideLimits` (point index unused) when `mswd` is outside
  `mswd_limits(n, q)`.

### 5.4 Deviations from legacy

Each is a rule of this spec and has its own test.

| # | Legacy | Here |
|---|---|---|
| X1 | MSWD of a surface fit is the scatter of J about the level mean | about the fitted surface, over `n - q` |
| X2 | a weighted plane uses unweighted residuals with the weighted covariance | residuals, covariance and scale all weighted |
| X3 | Bowl and LeastSquares1D ignore the weighted option | honoured by all three least-squares kinds |
| X4 | `n <= q`: an error of 0 | `n < minimum_monitors`: an error, "bowl needs 6 monitor positions, 5 used" |
| X5 | a monitor with zero error has an infinite weight | a weighted fit or mean given a zero or non-finite `j_err` is an error naming the monitor |
| X6 | arithmetic mean of one analysis: error 0 | that analysis's own error |
| X7 | arithmetic mean's MSWD about the weighted mean; SD of a weighted mean unweighted | `stats.hpp` as it stands |
| X8 | Bracketing "Average": population SD of two values | sample SD (n - 1) |
| X9 | F of zero: `J = 1` | the analysis is rejected and named |
| X10 | extrapolation past the end monitors is silent | still extrapolates (Bracketing Linear, Bracketing1D), with an `Extrapolated` note for the point |
| X11 | NearestNeighbors needs 3 positions whatever `n_neighbors` is | needs `n_neighbors` |
| X12 | Plane accepts 3 monitors (exact fit, error 0) | needs 4 |
| X13 | SD offered for surface fits (`sef * sqrt(1 + x C x')`, unweighted residuals) | not offered (F13) |

Unchanged from legacy: the J formula, averaging J rather than F, the two mean
kinds, the error kinds of the means, neighbour selection, extrapolation, no
automatic outlier rejection, a Bowl with no `xy` term.

## 6. Orchestration: `libs/processing`

Qt-free, in two headers. `flux_fit.hpp` in `libs/processing` proper holds the
types and the pure `fit_level` (no store, no JSON). `flux_store.hpp` in the
`processing_store` adapter, built only with persistence, holds the monitor
sets, the options JSON, `load_level` and `save_level`.

```cpp
struct MonitorSet { /* one entry of section 4 */ };
struct MonitorSets { std::string default_name; std::vector<MonitorSet> sets; std::string other_json = "{}"; };
Result<LoadedMonitorSets> load_monitor_sets(persistence::IStore&);
Result<persistence::CommitOutcome> save_monitor_sets(persistence::IStore&, const persistence::Actor&,
                                                     const MonitorSets&, const LoadedMonitorSets&);

struct MonitorSelection {
  std::string monitor_set;            // empty: the saved fit's, else the document's default
  std::optional<std::string> sample;  // nullopt: the saved fit's under its own set, else the set's (R18, R20)
  std::optional<bool> all_positions;  // nullopt: by sample when `sample` is given, else as saved, else false
};

struct LevelInputs;   // positions, geometry, monitor analyses with F, saved revisions, monitor set
struct FluxOptions {  // reduction::FitOptions plus the mean
  reduction::FitOptions fit;
  reduction::MeanKind mean = reduction::MeanKind::Arithmetic;
  reduction::MeanErrorKind mean_error = reduction::MeanErrorKind::Msem;
};
struct Edits {
  std::set<std::string> omit, include;   // record ids; `include` overrides a tag or a saved omission
  std::set<int> exclude_positions;       // holes left out of the fit
  bool reset_omits = false;              // ignore the omissions and exclusions of the saved fit
};

Result<LevelInputs> load_level(IAnalysisSource&, persistence::IStore&, std::string_view irradiation,
                               std::string_view level, const MonitorSelection&);
Result<LevelFit>    fit_level(const LevelInputs&, const FluxOptions&, const Edits&);   // pure
Result<FluxSaveOutcome> save_level(persistence::IStore&, const persistence::Actor&, const LevelFit&,
                                   const SaveSelection&, std::string_view software);
bool same_flux_value(const persistence::FluxValue&, const persistence::FluxValue&);   // "unchanged" (R16)
```

### 6.1 `load_level`

1. The level sheet (`IStore::level_sheet`): positions, samples, identifiers.
   An unknown irradiation or level is an error.
2. The monitor set (F6). With no set named, the one the level's saved fit
   used; failing that, the document's default. When the saved fit names a
   set the document does not have, the default is used and
   `LevelInputs::saved_monitor_set` / `saved_monitor_set_missing` say so
   (R17).
3. Monitor positions are those whose sample name equals the set's `sample`
   (or `MonitorSelection::sample`). Unknown positions are the others that
   have an identifier. With `all_positions`, every position that has analyses
   is a monitor and appears in the monitor table only. What the selection
   does not say is as the level's newest saved fit had it (F9, R18, R20):
   its `monitor_sample` when no sample is given and the set in use is the
   saved fit's own (same name as its `monitor_reference`); its
   `all_positions` when `MonitorSelection::all_positions` is `nullopt` and
   no sample is given. Another set (one named that is not the saved one, or
   the default standing in for a saved set the document lacks) uses its own
   `sample`; a sample given with no word on the positions selects by
   sample. `LevelInputs` carries
   what was used (`monitor_set.sample`, `all_positions`), and a save writes
   it.
4. The monitor analyses, through the source, reduced as any analysis is; F
   is `ReducedAnalysis::arar->f`, which needs no J. An analysis whose
   reduction failed is carried with its error and takes no part. An analysis
   starts omitted when its tag is `omit`, `invalid`, `outlier` or `skip`
   (legacy `EXCLUDE_TAGS`, `pychron_constants.py:192`).
5. Hole x, y from the level's holder (`irradiation_holder` reference): a
   position's hole is the holder hole with `ordinal == position - 1`;
   `hole_id` is only a label (R12). A level with no holder, or a position
   beyond the holder, is an error naming it: `position <N> of level <L> of
   <irrad> is beyond holder <name> (<M> holes)`.
6. Each position's head `flux_position` revision, if any: the saved J, and
   for monitors the saved omissions and options.

### 6.2 `fit_level`

Pure. An analysis is omitted when it is in `Edits::omit`, or (unless
`reset_omits`) was omitted in the saved fit, or starts omitted by its tag,
and is not in `Edits::include`. An analysis that did not reduce, or gives
no J, takes no part but is not "omitted": a save does not carry it forward
as an omission (R15). A monitor position is left out of the fit when it is
excluded or has no usable analysis; it still gets a predicted J. It is
excluded (`FittedPosition::excluded`) when it is in `exclude_positions`, or
(unless `reset_omits`) the saved fit says `excluded: true`. A saved revision
with no `excluded` key (saved before R15, or imported) excludes a monitor
only when it says `used_in_fit: false` and has a `mean_j`: the monitor had
analyses and still was not used. `used_in_fit: false` alone excludes
nothing, since it is also what a monitor with no analyses yet, and every
unknown, is saved with.

Result, per position: hole, identifier, sample, x, y, `n`, saved `j`, mean
`j` and error and MSWD (monitors), predicted `j` and error,
`dev = (saved - predicted) / predicted * 100`, whether it took part in the
fit, and its notes. For the level: the options and monitor set used, the fit
MSWD and degrees of freedom, the parameters, min and max predicted J and
`(max - min) / max * 100`.

A monitor position whose every analysis is rejected (no J from its F) is
left out of the fit and noted, not a failure of the level (R11).
`exclude_positions` accepts any position of the level; excluding a position
that is not a monitor does nothing, a hole that is not a position is an
error listing the level's holes (R10). `LevelInputs::saved_sd_replaced` is
set when the saved fit asked for `Sd` of a surface and `Msem` was used
instead (R1).

Errors (nothing is fitted): no monitor positions; fewer used monitors than
the model needs (X4); a zero error in a weighted fit (X5); a layout that does
not determine the model (R8).

### 6.3 `save_level`

`SaveSelection` names the positions not to save (default: none). One unit of
work:

1. For each saved position a `flux_position` revision with a `FluxValue`:
   - `j`, `j_err`: predicted.
   - `mean_j`, `mean_j_err`, `mean_j_mswd`: monitors only.
   - `lambda_k_total`, `lambda_k_total_err`, `monitor_name`,
     `monitor_material`, `monitor_age`, `monitor_age_err`: from the monitor
     set (F4).
   - `position_jerr`: absent (F5).
   - `analyses`: every monitor analysis of the position with `is_omitted`,
     true only for an analysis omitted by rule (tag, `--omit`, a carried
     saved omission), never for one that could not be used (R15).
   - `options_json`: section 6.4.
2. A position with no reference object gets one (key
   `<irrad>/<level>/<pos>`), created before the changeset commits (see the
   known limit in section 12).
3. Each head moves by compare-and-swap from the revision `load_level` read.
4. `commit(ChangesetKind::Reference, "fit flux for <irrad><level>")`, the legacy
   message, so history reads the same in both systems.

Before anything is written, every position to be saved is checked (R19):
its `j` (and a monitor's `mean_j`) must be finite and above zero, and
`j_err` (`mean_j_err`) finite and not negative. Otherwise the whole save is
an error naming the lowest such hole and nothing is written, not even a
reference object.

A position whose new `FluxValue` is the same as its head's is not written; a
save that would write nothing commits nothing and says so. "The same" is
`same_flux_value` (R16): every field equal, and `options_json` equal as
parsed JSON (key order and spacing ignored, since a jsonb column returns its
own) with the `software` key left out of both. Any head that moved
since the load to a different value makes the whole save a `Conflict` naming
the position, who saved and when; a head that moved to a value equal to the
new one counts as unchanged (R14). The outcome type is `FluxSaveOutcome`
(R13).

Consequences that need no code here: the derived cache is keyed by an input
fingerprint that includes the flux revision (DVC spec 4.3), so the ages of
the level's unknowns are recomputed on next use; a pinned analysis keeps its
pin (6.2); a revision is immutable, so nothing entry does can erase a fit;
undo is moving a head back (`history`, `move_head`).

### 6.4 `options_json`

```json
{ "model_kind": "Plane", "use_weighted_fit": true,
  "predicted_j_error_type": "msem", "error_kind": "msem", "mean_kind": "arithmetic",
  "n_neighbors": 2, "interpolation_style": "Weighted Mean", "one_d_axis": "X", "degree": 1,
  "monitor_reference": "FC-2 (Kuiper 2008)", "monitor_sample": "FC-2",
  "used_in_fit": true, "excluded": false, "all_positions": false,
  "fit_mswd": 1.12, "fit_dof": 5,
  "software": "pychron-cpp 0.4.0" }
```

`used_in_fit` is information: it is false for every position that took no
part, unknowns included. `excluded` is true only for a monitor the user left
out, and is what a refit carries forward (R15). `monitor_sample` and
`all_positions` are how the fit's monitors were chosen, which the next load
repeats (R18). `software` is not part of the comparison that decides
"unchanged" (R16). Keys are only ever added; an existing key is never
renamed.

`model_kind`, `use_weighted_fit`, `predicted_j_error_type`,
`interpolation_style` and `monitor_reference` are the legacy keys with the
legacy model strings (`persist.py:282-294`), so a revision written here and
one imported from a meta repo read alike. The rest are new. Reading is
tolerant: a missing key takes its default, an unknown model string (an
imported `RBF` fit) means "no saved options", and legacy error strings
(`SEM`, `SD`, `SE but if MSWD>1 use SE * sqrt(MSWD)`) parse as
`parse_mean_error_kind` already does.

## 7. `elctl flux`

`flux.cpp` with a `flux_stub.cpp` for builds without persistence, like
`export`. The store is named with `--db <url>`, as in `export` and `entry`.

```
elctl flux fit <irradiation> [<level>]
    --model plane|bowl|weighted-mean|matching|nearest|bracketing|ls1d|mean1d|bracketing1d
    --weighted | --unweighted
    --mean arithmetic|weighted          --mean-error sem|msem|sd
    --fit-error sem|msem|sd             # error of the predicted J; sd: the mean kinds only
    --neighbors N   --interpolation weighted|average|linear
    --axis x|y      --degree 1..4
    --monitors NAME   --sample NAME   --all-positions | --monitor-positions
    --omit RECORD_ID...   --include RECORD_ID...   --reset-omits
    --exclude-position HOLE...
    --no-save-position HOLE...
    --csv FILE
    --save
elctl flux show <irradiation> <level>
elctl flux history <irradiation> <level> [<hole>]
elctl flux monitors [list | show NAME | set FILE | default NAME]
```

- Without `--save` nothing is written.
- With no `<level>`, each level of the irradiation in turn, fitted
  independently; a level that fails is reported and the rest continue; the
  exit code is 1 if any failed. `--omit`, `--exclude-position` and
  `--no-save-position` need a level.
- Options not given come from the level's saved fit, and from the defaults
  (Plane, unweighted, arithmetic mean, `msem` for both errors: legacy's)
  when there is none. So does the choice of monitors: `--sample` and
  `--all-positions` of the saved fit hold until `--sample` or
  `--monitor-positions` (the opposite of `--all-positions`; giving both is a
  usage error) says otherwise (R18). The saved sample holds only under the
  saved fit's own monitor set, and `--sample` alone undoes a saved
  `--all-positions` (R20). So `elctl flux fit NM-300 A --save`
  repeats the last fit on the data as it is now.
- When the saved fit named a monitor set the store does not have, the
  default is used and the command warns `saved fit used monitor set
  '<name>', which the store does not have: using '<default>'`, unless
  `--monitors` is given (R17). `--exclude-position` on a hole that is not a
  monitor warns `hole <N> is not a monitor position: excluding it changes
  nothing`.
- Output: a header (irradiation, level, holder, monitor set with age and
  `lambda_k`, model and options), the monitor table, the unknown table, the
  level summary, then warnings: rejected analyses, extrapolated positions, a
  mean or fit MSWD outside its limits, monitors left out. J is printed with
  its error and the error as a percentage.
- `--csv` writes both tables (RFC 4180, every row the width of the header,
  monitors then unknowns with a `kind` column) and passes the file through
  `mark_as_user_file`. The destination is never truncated before the run
  has succeeded (R19): up front only its directory is checked (by making a
  sibling temporary file, so an unwritable destination still stops the run
  before anything is saved); the rows are written to the temporary, which is
  renamed over the destination at the end. When no level was fitted nothing
  is written and an earlier file is left as it was; the temporary is removed
  on every failure.
- `show` prints the saved J of each position with its model, who saved it
  and when. `history` lists the revisions of the level's positions, newest
  first, grouped by changeset.
- `monitors set FILE` validates and saves the document from a JSON file;
  `default NAME` changes the default.
- Exit codes, as `export`: 0 on success (warnings included); 1 when a level
  could not be fitted, a save conflicted, or a name asked for does not
  exist; 2 for usage and fatal errors. A flag that takes a value refuses a
  value starting with `--`. When the saved fit used `Sd` on a surface the
  command prints `saved fit used SD, which a fitted surface does not have:
  using msem` unless `--fit-error` is given (R1).

## 8. Testing

Golden values come from two places, since F2 rules out comparing errors with
legacy:

- **Legacy tests whose numbers are right**: `calculate_flux` inverting the
  age equation (`processing/tests/argon_calculations_test.py:91-97`); the
  weighted mean of 80 and 90 (`core/regression/tests/error_propagation.py:333-355`);
  Bracketing Linear at two and four monitors and Bracketing1D
  (`core/regression/tests/regression.py:300-372`, `:579-656`); Plane
  coefficients `[1, 2, 0]` and Bowl `[0, 0, 1, 2, 0]`
  (`error_propagation.py:809-880`); the tube-holder Bracketing1D case
  (`regression.py:681-790`).
- **`tools/flux_reference.py`** (numpy only, checked in, not run in CI): the
  least-squares kinds written out from the formulas of 5.3, for a fixed set
  of monitors. Its output is committed as
  `tests/reduction/flux_golden.hpp`, as `reduce_golden.hpp` is.

`tests/reduction/test_flux.cpp`:
- `j_of` and `mean_j`: both mean kinds, the three error kinds, one analysis,
  an omitted analysis, a rejected F (X6, X7, X9).
- every model against its golden values; one test per row of 5.4.
- a surface with a real gradient and consistent errors has MSWD near 1 (X1).
- the minimum monitor count of each kind, one fewer being an error (X4,
  X11, X12).
- ties and co-located monitors in the neighbour kinds; `x1 == x0` in
  Bracketing1D.

`tests/processing/test_flux_fit.cpp` (SQLite only, R9):
- a built level: tables equal the math layer's on the same numbers.
- monitors found by sample name; `--sample`; all positions.
- tags start an analysis omitted; `include` brings it back.
- save, then load: the saved J is the predicted J; a second save writes
  nothing.
- omissions and excluded positions survive save and refit; `reset_omits`
  clears them.
- a head moved between load and save: `Conflict`, nothing written.
- an unknown's age changes after a save; a pinned one does not.
- a level imported from the legacy fixture loads as "saved", its options are
  read, and it refits.
- a missing holder, a missing hole, no monitors: errors that name the cause.
- monitor sets: defaults when absent, validation, round trip, conflict.

`apps/elctl/tests/test_flux_cmd.cpp`: `fit` printing and `--save`;
whole irradiation with one failing level; `show`, `history`, `monitors`;
the CSV's header and row widths; the stub's message without persistence.

## 9. Documentation

`docs/flux.md`: the workflow, each model in one paragraph with when to use
it, the options, how errors are formed, what is saved, and a section "How
this differs from legacy Pychron" listing 5.4. A bullet in `AGENTS.md`.

## 10. Open items

None. O1 (monitor age uncertainties) and O2 (`Sd` on a surface fit) were
settled by the owner on 2026-10-07 as F12 and F13.

## 11. Not in this spec

- **Flux window** in `pychron-ui`: the two tables with use and save toggles,
  J against angle with the individual analyses and point omission, the 2-D
  contour with slices, the per-position grid. Own spec, on sections 5 and 6
  unchanged.
- **Flux visualization and vertical flux** (read-only legacy tools).
- **Position error**: if wanted later, analytic, `|grad J| * sigma_xy` on the
  fitted surface, not sampled.
- **External error** in the age calculation using `monitor_age_err` and
  `lambda_k_total_err` (F4): the ArAr reduction's concern.
- **Interpolant models**, a joint fit of several levels, MassSpec flux
  transfer, estimating J from the chronology (entry already does).

## 12. Implementation notes (2026-10-07)

What ended up different from, or more precise than, the text above (sections
2, 5.3, 6, 6.1-6.4, 7 and 8 are amended to match; R15-R19 are the rulings of
the final review). Rulings are numbered as
decided during implementation.

- **R12, hole lookup (a correction to this spec).** A position's hole is the
  holder hole with `ordinal == position - 1`; `hole_id` is only a label.
  Section 6.1(5) said "by hole id" and section 2 listed legacy's
  `geom[hole_id - 1]` as a problem: both were wrong. Entry, the UI grid and
  legacy all place position N at the Nth hole, and the importer's ids for
  holders without hole numbers are file line numbers. The error for a
  position beyond the holder is `position <N> of level <L> of <irrad> is
  beyond holder <name> (<M> holes)`.
- **R7, a ring cannot determine a Bowl.** Monitors all at one radius make
  `x^2 + y^2` constant, so the Bowl is refused; it needs monitors at more
  than one radius. The golden header has a second monitor set, `kMixed`, for
  the bowl cases.
- **R8, conditioning.** A least-squares fit (Plane, Bowl, LeastSquares1D)
  refuses a layout that barely determines it: when the smallest pivot of the
  column-equilibrated design is below 1e-3 of the largest, or when any
  predicted J is not finite and positive, the error is `monitor positions do
  not determine a <model>`.
- **R4.** A test ruling only: a test may build `FitOptions` in a local variable instead of a designated initializer when the compiler warns.
- **F5 consequence.** A position imported with a `position_jerr` and then saved here has a new head without one; the reduction no longer uses the imported value for it, which stays in the older revision.
- **R6.** Bracketing1D is always linear; `--interpolation` applies to
  Bracketing only.
- **R5, R5a.** In an unweighted least-squares fit a monitor with zero error
  is used in the fit but skipped in the reported MSWD, whose divisor is
  (monitors with non-zero error - q).
- **R10.** `--exclude-position` accepts any position of the level; excluding
  an unknown is a no-op; a hole that is not a position is an error listing
  the level's holes.
- **R11.** A monitor position whose every analysis is rejected (no J from
  its F) is left out of the fit and noted, not a failure of the level.
- **R1.** `LevelInputs::saved_sd_replaced`; the CLI warning `saved fit used
  SD, which a fitted surface does not have: using msem` is printed unless
  `--fit-error` is given or the resolved model is not least-squares.
- **R13.** The save outcome type is `FluxSaveOutcome`.
- **R14.** A position whose head moved since the load to a value equal to
  the new one counts as unchanged, not as a conflict; a head moved to a
  different value is a conflict and nothing is written.
- **R3 (revised by R16).** The `software` key is part of `options_json`.
  It was also part of the equality that decides "unchanged"; it no longer
  is.
- **R15, what a refit carries forward.** `options_json` gains `excluded`,
  true only for a monitor the user left out (`--exclude-position`, or an
  exclusion carried from the saved fit). `used_in_fit` stays as information
  and is no longer read as an exclusion: it is false for a monitor with no
  analyses yet and for every unknown, and reading it back left monitors
  measured after a save silently out of the fit. A revision without the key
  (saved earlier, or imported) excludes a monitor only when it says
  `used_in_fit: false` and has a `mean_j`. Likewise `is_omitted` is saved
  true only for an analysis omitted by rule; one that did not reduce or gave
  no J is saved not omitted, so it returns once it is usable.
  `--reset-omits` discards both the carried omissions and the carried
  exclusions.
- **R16, "unchanged" (revises R3).** A position is unchanged when every
  field of its `FluxValue` equals the head's and the options are equal as
  parsed JSON without the `software` key (`same_flux_value`). Text equality
  never held on PostgreSQL, whose jsonb returns its own key order and
  spacing; and a version bump alone no longer rewrites every position (the
  new version is recorded when something else changes).
- **R17, a monitor set the store lacks.** The fallback to the default set
  stays; `LevelInputs::saved_monitor_set` and `saved_monitor_set_missing`
  report it and `elctl flux fit` warns unless `--monitors` is given.
- **R18, a saved fit's monitors (F9; narrowed by R20).** `options_json`
  gains `all_positions`. With no `--sample`, the newest saved revision's
  `monitor_sample` is the monitor sample, under that fit's own monitor set;
  with none of `--all-positions`, `--monitor-positions` and `--sample`, its
  `all_positions` applies.
  `MonitorSelection::all_positions` is `std::optional<bool>`. `flux show`
  asks for the monitor sample's positions, so it lists every position of
  the level whatever the saved fit used.
- **R20, the saved sample stays with its set (narrows R18).** Every save
  writes `monitor_sample`, so applying it whatever set was in use gave the
  saved fit's monitors to another standard: after any save, `--monitors
  OTHER` (and the R17 fallback default) fitted the FC-2 positions with the
  other set's age, silently. The saved sample is applied only when the
  resolved set's name equals the saved fit's `monitor_reference`; any other
  set uses its own `sample`, and `MonitorSelection::sample` always wins. A
  sample given with `all_positions` not given selects by sample even when
  the saved fit had `all_positions: true`; `--all-positions` given
  explicitly still wins.
- **R19, nothing half done.** `--csv` replaces its destination only at the
  end and only when a level was fitted (temporary file and rename).
  `save_level` refuses the whole save when a J to save is not finite and
  above zero, or its error not finite and at least zero, naming the hole.
- **R9.** The flux store tests run on SQLite only; `PYCHRON_TEST_PG_URL` is
  not exercised by them.
- **R2.** Level seeding for tests lives in `tests/processing/flux_seed.hpp`
  (free functions, shared with `apps/elctl/tests`).
- **Monitor sets.** A set needs a non-empty `sample`; a document with no
  `default` key takes the first set; only unknown top-level keys are kept;
  `monitors set FILE` replaces the whole document (the file must list every
  set to keep).
- **Reduction of monitors.** Monitors are reduced with the position's saved
  flux removed, so F never depends on a previously saved J; an analysis whose
  reduction reports an error takes no part.
- **The saved fit of a level** (for default options, monitor set, monitor
  sample and `all_positions`) is the newest saved revision over all its
  positions that says the thing in question; a saved monitor-set name the
  document lacks falls back to the default set, with a warning (R17).
- **`elctl flux`.** The store is named with `--db <url>`; exit codes 0 / 1 /
  2 as in section 7 (`flux.hpp` is the authority); a value-taking flag refuses
  a value starting with `--`. Orchestration is split in two headers
  (`flux_fit.hpp` pure, `flux_store.hpp` store and JSON) and the admin
  subcommands (`show`, `history`, `monitors`) live in `flux_admin.cpp`.
- **Known limit.** A new reference object for a position is created before
  the changeset commits, so a conflicted save on a fresh level can leave
  reference objects with no value. They resolve nothing and are reused by the
  next save.
