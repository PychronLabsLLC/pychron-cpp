# Data browser: date range, irradiation, list filtering, row colour, time breaks: plan

Spec: `docs/superpowers/specs/2026-10-09-data-browser-search-display-design.md`.

Each task is written test first, built with the `dev-ui` preset, checked with
`python3 tools/quality_check.py`, and committed on its own. No task touches
the schema or a SQL statement.

## Task 1: the time-break helper

- Add `libs/processing/include/pychron/processing/time_breaks.hpp` and
  `libs/processing/src/time_breaks.cpp` (spec 3.5): `TimeBreak` and
  `time_breaks(std::span<const AnalysisSummary>, double threshold_seconds)`,
  plus `gap_text(double seconds)` for the duration wording, so that the
  wording is tested without a widget.
- Tests: `tests/processing/test_time_breaks.cpp`, the cases listed in spec
  section 5, including the append rule (the breaks of the first `k` rows are
  the breaks of all rows whose `above < k`).
- Commit: `feat(processing): find the time breaks in a list of analyses`.

## Task 2: row colours

- Add `apps/pychron-ui/src/row_colors.{hpp,cpp}` (spec 3.4): `ColorBy`,
  `TypeColors`, `default_type_colors`, `color_key`, `row_color`, and the
  text form of `ColorBy` used in QSettings.
- `Theme::row_category` (eight tints) in `theme.{hpp,cpp}`.
- `AnalysisTableModel::set_coloring(ColorBy, TypeColors)`; `BackgroundRole`
  calls `row_color`.
- Tests: `tests/ui/test_row_colors.cpp`.
- Commit: `feat(ui): analysis rows coloured by type, tag, spectrometer or level`.

## Task 3: separator rows in the table

- `AnalysisTableModel`: display rows, `set_gap_threshold`, `analysis_at`,
  `is_break`, `analysis_count`, `display_row_of`; `row(int)` removed.
- `DataBrowserWindow`: spans, status counts analyses, selection, figure,
  export and recall act on analyses only, `recall_step` skips separators,
  `set_gap_hours`.
- Tests: new slots in `tests/ui/test_data_windows.cpp` (spec section 5,
  "separators"); existing slots move from `row(r)` to `analysis_at(r)`.
- Commit: `feat(ui): the data browser marks time breaks between analyses`.

## Task 4: list filters, irradiation and level lists

- Add `apps/pychron-ui/src/facet_box.{hpp,cpp}` (spec 3.3) and use it in
  `DataBrowserWindow`; add the Irradiation and Level lists; put the filter
  column in a scroll area; show the Irradiation column.
- `make_source` in `tests/ui/test_data_windows.cpp` gives its rows an
  irradiation and a level.
- Tests: slots for `FacetBox` and for irradiation narrowing level.
- Commit: `feat(ui): the data browser filters by irradiation and level, and its lists can be searched`.

## Task 5: date range

- `DataBrowserWindow`: the Range… entry, the two UTC date-time edits with
  their checkboxes, `query()` (spec 3.1), the "From is after To" message.
- Tests: slot `browser_date_range`.
- Commit: `feat(ui): a date range in the data browser`.

## Task 6: preferences and the Colour by choice

- `Preferences::browser_gap_hours`, `Preferences::browser_type_colors`;
  load, save and validation; the Data page's fields and Reset colours.
- `DataWorkspace::apply_preferences(const Preferences&)` replaces
  `set_page_size` at its two callers and hands page size, gap hours and type
  colours to the browser.
- The `Colour by` combo in the browser, kept in QSettings
  `data_browser/color_by`.
- Tests: `tests/ui/test_preferences.cpp` (round trip, bad values), a slot
  in `test_data_windows.cpp` for the combo.
- Commit: `feat(ui): colour by and time-break preferences for the data browser`.

## Task 7: documents

- `docs/user/06-data-analysis.md`, "The data browser": the new controls,
  that Level is matched by name, that time breaks are between the analyses
  shown.
- Section 11.2 of the 2026-10-02 spec points at the new spec; the new spec's
  status becomes "implemented".
- Commit: `docs: the data browser's new filters, colours and time breaks`.

## Owed after the tasks

The measurement of spec section 5 (time of `reload()` with four and with six
facet statements, and the plans of the Irradiation and Level facet
statements) needs a copy of a lab's store. It is run when one is at hand and
its numbers are reported before the branch lands.
