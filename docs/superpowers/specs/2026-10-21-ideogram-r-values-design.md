# Ideogram R-values: matrix of group F ratios

Date: 2026-10-21
Status: Design agreed in conversation 2026-10-21; written spec awaiting review. Nothing built.
Owner: Jake Ross
Depends on: `2026-10-02-data-browsing-visualization-design.md` §7.5
(`group_stats`), §8 (ideogram), §10 (quantities); legacy pychron
`pipeline/plot/editors/rvalues_table.py`, `pipeline/results/base_matrix_result.py`,
`processing/analyses/analysis_group.py` `_get_weighted_mean_f`.

## 1. Intent

Intercalibration of standards and age recalculation against a monitor need
R = F̄_unknown / F̄_standard (F = 40Ar*/39ArK) with full precision and an
uncertainty. Legacy ideogram has `show_rvalues`: per panel, upper-triangle
table, cell `other.weighted_mean_f / ag.weighted_mean_f`, value only, 7
decimals. C++ has no R-value anywhere: ideogram (`libs/processing/src/ideogram.cpp`)
computes a mean per group of plotted quantity, nothing keeps or shows a
group F̄.

This spec: pure calculation in `libs/processing`, n×n table in ideogram
figure window, copy and CSV export.

## 2. Definitions

- Groups and graphs: those of ideogram dataset (`Dataset::graphs()`,
  `groups_of_graph(graph)`). One matrix per graph (legacy: one per panel).
- F̄_g: mean of quantity `F` (`quantity.cpp`, `arar->f.f`) over group g's
  included analyses (`exclusion.included()`) that have F. Always `F`,
  whatever ideogram `quantity` is.
- Mean kind = ideogram `mean` (`weighted` | `arithmetic`); error kind =
  ideogram `error_kind` (`msem` | `sem` | `sd`), via
  `reduction::weighted_mean` / `arithmetic_mean` exactly as ideogram does
  (msem expands by √MSWD when MSWD > 1).
- `j_error_in_mean` ignored: F contains no J, so groups of different J
  (each analysis carries own flux) need no J term.
- R_ij = F̄_j / F̄_i (row i = denominator, column j = numerator). Row of a
  standard reads R of every other group against it.
- σ_R_ij = |R_ij| · √((σ_i/F̄_i)² + (σ_j/F̄_j)²), σ = 1σ error of F̄.
- Groups treated as independent. Correlated errors (shared irradiation
  interference corrections, shared blanks, shared detector intercalibration)
  not accounted for; stated beside every output (§5, §6).

## 3. Interface (`libs/processing`)

New `include/pychron/processing/r_values.hpp`, `src/r_values.cpp`:

```cpp
struct GroupF {
  int group = 0;
  std::string name;                       // Dataset::group_name
  std::optional<reduction::Mean> f;       // absent: no included analysis has F
  std::string error;                      // why f absent
};

struct RCell {
  double r = 0;
  double r_err = 0;                       // 1 sigma
};

struct RValueMatrix {
  int graph = 0;
  std::string graph_name;
  std::vector<GroupF> groups;             // order of groups_of_graph
  // cells[i][j] = F_j / F_i; empty on the diagonal and in row and column of
  // a group without f, or where F_i == 0.
  std::vector<std::vector<std::optional<RCell>>> cells;
};

// Over every graph of `dataset`, with mean and error kind read from
// `ideogram_options` (schema "figure.ideogram"). Fails only on options it
// cannot read; a group without F is a GroupF::error, not a failure.
Result<std::vector<RValueMatrix>> r_value_matrices(const Dataset& dataset, const Options& ideogram_options);

inline constexpr std::string_view kRValueNote =
    "Uncertainties treat groups as independent; correlated errors (shared irradiation "
    "corrections, blanks, ...) are not accounted for.";
```

Rules:

- Ideogram mean and F̄ must not drift apart. Mean selection
  (weighted/arithmetic, error kind) cut out of `build_ideogram` into one
  helper in `libs/processing` (e.g. `group_mean(values, errors, options)`)
  called by both. `build_ideogram` output unchanged: existing
  `Ideogram.*` tests pass untouched.
- Pure: no I/O, no Qt. Cheap (one mean per group, n² divisions): called on
  UI thread after each figure run.
- Graph with fewer than 2 groups: matrix still returned (1×1 or empty);
  display decides (§4).

## 4. Display (`apps/pychron-ui` figure window)

- New ideogram option `show_rvalues`: `boolean("show_rvalues", "R-values table", "Display", false)`
  in `ideogram_schema()`. Presets unchanged (default false).
- `FigureWindow` of kind `ideogram`: after `on_result`, when
  `show_rvalues` is set, call `r_value_matrices(*dataset_, figure options)`.
  "R-values" panel below figure, visible only when option set and at least
  one graph has ≥2 groups; otherwise hidden.
- One `QTableWidget` per graph with ≥2 groups, headed by graph name. Row and
  column headers: group name and `F̄ ± nσ`. Cell: `R ± nσ`, n =
  `error_bar_nsigma`. Display precision: R and error to 6 decimals (legacy
  7 on value only); tooltip: R, σ (1σ) with round-trip precision. Empty
  cell: blank, tooltip `GroupF::error` when that is the cause.
- `kRValueNote` shown as fixed text under tables.
- Recomputed on every figure result (exclusion toggled, grouping, graph-by,
  options changed); no separate trigger.

## 5. Copy and export

- "Copy" button: TSV of each table as displayed (header row, header column,
  `R ± err` cells at nσ), graphs separated by a blank line, first line the
  note prefixed `# `.
- "Export CSV..." button: file dialog, long form, one row per ordered pair
  i ≠ j with both F̄ present:

  ```
  # Uncertainties treat groups as independent; correlated errors (...) are not accounted for.
  graph,numerator,denominator,f_num,f_num_err,f_den,f_den_err,r,r_err,mean,error_kind
  ```

  Errors 1σ. Numbers written shortest round-trip (`std::to_chars`), so file
  identical on every compiler. Fields with comma or quote quoted per RFC
  4180. After write: `pychron::mark_as_user_file(path)`.
- Writer is pure function in `libs/processing`
  (`std::string r_values_csv(const std::vector<RValueMatrix>&, const Options&)`),
  tested without Qt; window only picks path and writes bytes.

## 6. Tests

`tests/processing/test_r_values.cpp` (`RValues.*`), hand-computed values:

- Two groups, weighted mean, sem: R and σ_R match hand calculation.
- Reciprocal: cells[i][j].r · cells[j][i].r == 1 (to 1e-12); relative
  errors equal.
- msem with MSWD > 1 expands σ_F̄ by √MSWD; with MSWD ≤ 1 equals sem.
- Arithmetic mean option followed.
- Excluded analysis not in F̄.
- Group whose analyses lack F: GroupF::error set, its row and column empty,
  other cells present.
- Ideogram `quantity = age`: F̄ still from F; ideogram mean text unchanged.
- Two graphs: two matrices, groups per graph.
- `r_values_csv`: header, note line, row count n(n−1), round-trip numbers.

`tests/processing/test_arar_figures.cpp`: `Ideogram.*` unchanged and
passing after helper extraction.

`tests/ui/test_data_windows.cpp`: ideogram window, two groups,
`show_rvalues` false → panel hidden; true → panel shown, 2×2 table, cell
text `R ± err`; one group → hidden.

## 7. Out of scope

- Correlated errors between groups (shared interference corrections,
  blanks, detector IC): would need covariance carried through reduction.
- t-test probability table (legacy `TTestTable`).
- `elctl` / pipeline unit emitting matrix.
- Publication report, data release, EarthBank output.
- R into age recalculator (`2026-10-08-age-recalculator-design.md`):
  later spec may read `RValueMatrix`.
- Choosing a reference group or highlighting a row: full matrix makes it
  unnecessary.

## Open questions

None.
