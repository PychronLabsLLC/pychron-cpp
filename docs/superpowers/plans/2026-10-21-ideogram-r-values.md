# Ideogram R-values Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compute an n×n matrix of R = F̄_j / F̄_i between the groups of each ideogram graph, show it under the ideogram, and let the user copy it or export it as CSV.

**Architecture:** All arithmetic and all text (cell text, TSV, CSV) live in a new pure module `libs/processing` `r_values.hpp/.cpp`. The group mean used by the ideogram is cut out of `build_ideogram` into a shared helper so the ideogram's mean and F̄ cannot drift. The UI adds a small `RValuesPanel` widget in a bottom dock of `FigureWindow`, filled after each figure result. The window computes nothing itself.

**Tech Stack:** C++20, GoogleTest (`tests/processing`), Qt 6 Widgets + QtTest (`tests/ui`).

**Spec:** `docs/superpowers/specs/2026-10-21-ideogram-r-values-design.md`

## Global Constraints

- F̄ always from quantity `F`, over included analyses (`exclusion.included()`), whatever the ideogram's `quantity`.
- Mean kind from ideogram `mean` (`weighted` | `arithmetic`), error kind from `error_kind` (`msem` | `sem` | `sd`); `j_error_in_mean` ignored.
- R_ij = F̄_j / F̄_i; σ_R = |R| · √((σ_i/F̄_i)² + (σ_j/F̄_j)²), 1σ.
- Diagonal empty; the row and column of a group without F̄ are empty; a cell is empty where F̄_i == 0.
- Note text, verbatim: `Uncertainties treat groups as independent; correlated errors (shared irradiation corrections, blanks, ...) are not accounted for.`
- Display: `R ± err` at nσ (`error_bar_nsigma`), 6 decimals each.
- CSV header, verbatim: `graph,numerator,denominator,f_num,f_num_err,f_den,f_den_err,r,r_err,mean,error_kind`; errors 1σ; numbers shortest round-trip (`std::to_chars`); fields quoted by `csv_quote` (`report.hpp`, RFC 4180); first line `# ` + note.
- Exported file goes through `pychron::mark_as_user_file(path)`.
- `build_ideogram` output unchanged: every existing `Ideogram.*` test passes untouched.
- No Qt in `libs/processing`.

## Review Focus

1. Negative F̄ (blank-dominated group): R negative, σ_R uses |R|, never NaN. Test `RValues.NegativeFKeepsAPositiveError` (Task 1).
2. Group whose included analyses all have zero F error: `weighted_mean` fails, so the group has no F̄; its row and column are empty and the other cells survive. Test `RValues.ZeroErrorsLeaveTheGroupWithoutF` (Task 1).
3. Group with one analysis: F̄ = that F, σ = its error (msem with MSWD 0 is sem). Test `RValues.OneAnalysisGroup` (Task 1).
4. Group name with a comma or quote in CSV: quoted, row width equals header width. Test `RValues.CsvQuotesNames` (Task 2).
5. Several graphs, only some with ≥2 groups: dock shows tables for those graphs only; a dataset where no graph has 2 groups hides the dock. Step in `ideogram_r_values_panel` (Task 3).

---

### Task 1: Shared group mean and `r_value_matrices`

**Files:**
- Modify: `libs/processing/src/figure_common.hpp`, `libs/processing/src/figure_common.cpp` (add helper)
- Modify: `libs/processing/src/ideogram.cpp:186-197` (call helper)
- Create: `libs/processing/include/pychron/processing/r_values.hpp`, `libs/processing/src/r_values.cpp`
- Test: `tests/processing/test_r_values.cpp` (new; GLOB picks it up)

**Interfaces:**
- Produces, in `detail` (`figure_common.hpp`):
  `Result<reduction::Mean> group_mean(std::span<const double> values, std::span<const double> errors, const Options& figure_options);`
  reads `mean` and `error_kind` exactly as `build_ideogram` does now (`parse_mean_error_kind(...).value_or(Msem)`, `weighted` → `weighted_mean`, else `arithmetic_mean`).
- Produces, in `r_values.hpp` (namespace `pychron::processing`): `GroupF`, `RCell`, `RValueMatrix`, `kRValueNote`, `r_value_matrices` exactly as spec §3.

- [ ] **Step 1: Write failing tests** in `tests/processing/test_r_values.cpp`. Build datasets as `test_arar_figures.cpp` does (`make_step(i, ar39, ar36, f, identifier)`, `reduce_analysis`), setting `item.path.group` and `d.group_names`. Read each analysis's F and error through `Quantity::parse("F")->eval(...)` and compute expected values by hand in the test with `r::weighted_mean` over those values.
  - `TwoGroupsWeightedSem`: groups F≈10 and F≈12, `error_kind = sem`. `cells[0][1]->r == F̄1/F̄0` (EXPECT_NEAR 1e-12 relative), `r_err` per formula; `cells[0][0]` and `cells[1][1]` empty.
  - `Reciprocal`: `cells[0][1]->r * cells[1][0]->r` near 1 (1e-12); `r_err/r` equal both ways.
  - `MsemExpandsWhenMswdAboveOne`: scattered group (MSWD > 1): `groups[g].f->error == sem * sqrt(mswd)`; tight group (MSWD ≤ 1): `error == sem`.
  - `ArithmeticMeanFollowed`: `mean = arithmetic` → `groups[g].f->value` equals plain average.
  - `ExcludedAnalysisLeftOut`: `exclusion.user = true` on the outlier; F̄ equals mean without it.
  - `GroupWithoutF`: group whose analyses' `arar` is absent (or F eval fails): `!groups[1].f`, `!groups[1].error.empty()`, row 1 and column 1 empty, other cells present (three groups).
  - `UsesFWhileIdeogramPlotsAge`: options `quantity = age` (default): F̄ in F units (≈10, not Ma); and `build_ideogram` of same dataset still shows age mean (indicator x ≈ `age_of_f`).
  - `TwoGraphs`: `item.path.graph` 0/1, `d.graph_names`: two matrices, `graph_name` set, groups per graph.
  - Review Focus 1–3: `NegativeFKeepsAPositiveError` (one group F < 0: `r < 0`, `r_err > 0`, finite), `ZeroErrorsLeaveTheGroupWithoutF` (set errors so `weighted_mean` fails; fabricate via a group whose F errors are zero — if the fixture cannot give zero F error, construct the dataset with a stub `ReducedAnalysis` whose `arar->f.f` has error 0), `OneAnalysisGroup`.
  - `ZeroFDenominatorLeavesCellEmpty`: F̄_i == 0 → `cells[i][j]` empty for all j, column i present.

- [ ] **Step 2: Run, expect compile failure** (`r_values.hpp` missing):
  `cmake --build --preset dev --target pychron_processing_tests`

- [ ] **Step 3: Add `group_mean` to `figure_common`; switch `ideogram.cpp:186-197` to it.** J-error addition stays in `ideogram.cpp` (age only).

- [ ] **Step 4: Implement `r_value_matrices`** in `r_values.cpp`: per `d.graphs()`, per `groups_of_graph(graph)`: collect F value/error of included items whose eval succeeds and both are finite; `group_mean` → `GroupF` (on failure `error = to_string(result.error())` or "no included analysis has F"). Then cells per spec §3. Fail only if `Quantity::parse("F")` fails.

- [ ] **Step 5: Run**
  `build/dev/tests/processing/pychron_processing_tests --gtest_filter='RValues.*:Ideogram.*'`
  Expected: all pass; `Ideogram.*` unchanged.

- [ ] **Step 6: Static analysis** `python3 tools/quality_check.py` → exit 0; `python3 tools/compiler_sweep.py --changed` → no warnings.

- [ ] **Step 7: Commit** `feat(processing): R-value matrix of ideogram group F`

### Task 2: Cell text, TSV and CSV

**Files:**
- Modify: `libs/processing/include/pychron/processing/r_values.hpp`, `libs/processing/src/r_values.cpp`
- Modify: `docs/superpowers/specs/2026-10-21-ideogram-r-values-design.md` §3/§5 (add `r_value_text`, `r_values_tsv` signatures; spec and code never disagree)
- Test: `tests/processing/test_r_values.cpp`

**Interfaces:**
- Consumes: Task 1 types.
- Produces:
  - `std::string r_value_text(double value, double error_1s, int nsigma);` → `"%.6f ± %.6f"` of value and `nsigma * error_1s` (UTF-8 `±`).
  - `std::string r_values_tsv(const std::vector<RValueMatrix>& matrices, int nsigma);` — first line `# ` + note; per matrix with ≥2 groups: line `graph_name`, header row (empty cell, then each group's `name` + ` (` + `r_value_text(F̄)` + `)`; blank when no F̄), one row per group (same row header, then cells via `r_value_text`, empty string for empty cell); tab-separated, `\n` line ends, blank line between matrices.
  - `std::string r_values_csv(const std::vector<RValueMatrix>& matrices, const Options& ideogram_options);` — format per Global Constraints; one row per ordered pair i ≠ j where `cells[i][j]` present; numerator = group j, denominator = group i; `mean`, `error_kind` columns are the option strings.

- [ ] **Step 1: Failing tests**
  - `CellText`: `r_value_text(1.2345678, 0.001, 2) == "1.234568 ± 0.002000"`.
  - `TsvLayout`: two-group matrix → lines: note, graph name, header with 3 fields, 2 rows with 3 fields; diagonal field empty.
  - `CsvRowsAndRoundTrip`: three groups, all F̄ present → 1 note line + header + 6 rows; every row 11 fields; `std::stod` of `r` field == `cells[i][j]->r` exactly.
  - `CsvQuotesNames` (Review Focus 4): group name `a,"b"` → field `"a,""b"""`; row width still 11.
  - `CsvSkipsEmptyCells`: group without F̄ → its pairs absent.

- [ ] **Step 2: Run, expect link/compile failure.**
- [ ] **Step 3: Implement** with `std::to_chars` (shortest form) for CSV numbers, `csv_quote` from `report.hpp` for text fields.
- [ ] **Step 4: Run** `build/dev/tests/processing/pychron_processing_tests --gtest_filter='RValues.*'` → pass.
- [ ] **Step 5: Update spec §3/§5** with the two added signatures.
- [ ] **Step 6: Static analysis** as Task 1.
- [ ] **Step 7: Commit** `feat(processing): R-value text, TSV and CSV`

### Task 3: Option, panel and dock in the ideogram window

**Files:**
- Modify: `libs/processing/src/ideogram.cpp` (schema: after `fill_curve` add `boolean("show_rvalues", "R-values table", "Display", false)`)
- Create: `apps/pychron-ui/src/r_values_panel.hpp`, `apps/pychron-ui/src/r_values_panel.cpp`
- Modify: `apps/pychron-ui/CMakeLists.txt` (both files to source and header lists, beside `figure_window`)
- Modify: `apps/pychron-ui/src/figure_window.hpp`, `apps/pychron-ui/src/figure_window.cpp`
- Test: `tests/ui/test_data_windows.cpp`

**Interfaces:**
- Consumes: `r_value_matrices`, `r_value_text`, `r_values_tsv`, `r_values_csv`, `kRValueNote`.
- Produces:
  - `class RValuesPanel : public QWidget` with `void set_matrices(std::vector<processing::RValueMatrix> m, const processing::Options& ideogram_options);`, `bool has_tables() const;` (any matrix with ≥2 groups), `QList<QTableWidget*> tables() const;`, `QString note_text() const;`, `bool export_csv(const QString& path) const;` (writes `r_values_csv`, then `mark_as_user_file`; false on write failure), `void copy() const;` (`r_values_tsv` to `QGuiApplication::clipboard()`).
  - `FigureWindow::r_values_dock() const noexcept -> QDockWidget*`, `FigureWindow::r_values_panel() const noexcept -> RValuesPanel*` (test accessors; null for kinds other than `ideogram`).

- [ ] **Step 1: Failing UI test** `ideogram_r_values_panel` in `TestDataWindows`, on the `make_aliquots()` source as `graph_by_puts_groups_on_graphs` uses:
  - Default options → `r_values_dock()->isHidden()`.
  - `set_figure_options` with `show_rvalues = true`, wait a run → dock visible; one table, `rowCount() == columnCount() == 4` (four identifier groups); `item(0,1)->text()` contains `" ± "`; diagonal empty; `note_text()` equals `kRValueNote`.
  - `export_csv(tmp/r.csv)` true; file's first line starts with `# Uncertainties`; second line is the header.
  - Review Focus 5: `set_graph_key(same_as_group())` → every graph has 1 group → dock hidden; `set_graph_key("sample")` with group key `aliquot` → 4 tables of 2×2.
  - Time-series window → `r_values_dock() == nullptr`.
- [ ] **Step 2: Run, expect compile failure**
  `cmake --build --preset dev-ui --target pychron_ui_tests && build/dev-ui/tests/ui/pychron_ui_tests ideogram_r_values_panel`
- [ ] **Step 3: Add option to schema.** Run `build/dev/tests/processing/pychron_processing_tests` in full: a test that lists ideogram fields or round-trips default TOML may need the new field; update its expectation, never drop the field.
- [ ] **Step 4: Implement `RValuesPanel`**: `QVBoxLayout`: toolbar row (`Copy`, `Export CSV...` → `QFileDialog::getSaveFileName`, default `r_values.csv`, warning box on false), one `QLabel` (graph name) + `QTableWidget` per matrix with ≥2 groups (headers: name + newline + `F̄ ` + `r_value_text`; cells read-only, tooltip `R = <to_chars r>, 1σ = <to_chars r_err>` or `GroupF::error` when the cause of the empty cell), note `QLabel` with word wrap at bottom.
- [ ] **Step 5: Wire `FigureWindow`**: for `kind_ == "ideogram"` only, bottom `QDockWidget(tr("R-values"))` holding the panel, added with `Qt::BottomDockWidgetArea` beside (not tabified with) the Analyses dock, so both show at once. At end of `on_result` (scene path, after `view_->set_scene`): if `show_rvalues` and `dataset_`, `set_matrices(r_value_matrices(*dataset_, options))`; dock visible iff option set and `has_tables()`. On error result, hide dock.
- [ ] **Step 6: Run** UI test above → pass; `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`; `python3 tools/quality_check.py` → exit 0.
- [ ] **Step 7: Commit** `feat(ui): R-values table under the ideogram`

### Task 4: Docs and full run

**Files:**
- Modify: user guide page that documents ideogram options (find with `git grep -l "show_mswd\|MSWD probability" docs`); add a short "R-values" section: definition, error formula, independence note, Copy/Export CSV columns.

- [ ] **Step 1: Write the docs section.**
- [ ] **Step 2: Full build and tests** `cmake --build --preset dev-ui && ctest --preset dev-ui` → all pass (covers `dev` too).
- [ ] **Step 3: Commit** `docs: R-values in the ideogram guide`

## Open questions

None.
