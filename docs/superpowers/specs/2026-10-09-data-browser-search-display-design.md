# Data browser: date range, irradiation, list filtering, row colour, time breaks: design

Date: 2026-10-09. Status: approved 2026-10-09; implemented, except the
measurement of section 5 (needs a copy of a lab's store).

Extends section 11.2 of `2026-10-02-data-browsing-visualization-design.md`.
First of two specs. Second (age cache and age filter, `derived_value`
writers) is separate and builds on this one; nothing here depends on it.

## 1. Problem

Data browser (`apps/pychron-ui` `DataBrowserWindow`) finds runs worse than
legacy run browser:

- Date: five presets relative to newest analysis (`last_hours`). No way to
  say "between these two dates". `BrowseQuery::from` / `to` exist, store
  applies them, UI never sets them.
- Irradiation: `Facet::Irradiation`, `Facet::Level` exist and are tested at
  store and processing; UI has no list for them. Column hidden.
- Sample and Identifier lists hold hundreds of values, 130 px tall, no way
  to narrow them.
- Row colour fixed: tint by analysis type from theme, red for bad tag. Legacy
  let lab pick colours per type.
- Nothing shows where instrument stopped and started again. Legacy coloured
  a row when time since previous analysis exceeded a limit.

## 2. Goal

Five additions, all in browser's filter column, table and Preferences:

1. Low post / high post date range.
2. Irradiation and Level filter lists.
3. Text filter on every filter list.
4. "Colour by" choice for rows; per-type colours editable.
5. Separator row where gap between consecutive analyses exceeds a limit.

No migration. No new SQL. No change to `IAnalysisSource`, `BrowseQuery`,
`IStore`. Store-side behaviour of every filter used here already covered by
`tests/persistence/test_browse.cpp` (`Filters`, `FacetsIgnoreTheirOwnFilter`).

## 3. Design

### 3.1 Date range

- `dates_` combo keeps its entries, gains last entry **Range…** (item data
  `-1.0`).
- Below combo, row `range_` (hidden unless Range… is current):
  `[x] From [QDateTimeEdit]`, `[x] To [QDateTimeEdit]`. Checkbox per end;
  unchecked end is open. Both unchecked = any date.
- Edits: UTC (`setTimeZone(QTimeZone::utc())`), display format
  `yyyy-MM-dd hh:mm`, calendar popup on. Labels say "(UTC)", same as Date
  column.
- First time Range… is chosen, edits are set from rows then shown: From =
  oldest shown timestamp, To = newest shown; both checked. Empty table:
  both unchecked, edits at `2000-01-01 00:00`. No wall clock is read.
- `query()`: preset with hours > 0 sets `last_hours` as now. Range… sets
  `from` = From edit's seconds when checked, `to` = To edit's seconds + 59
  when checked (whole minute included; store bound is inclusive). Never both
  `last_hours` and `from`/`to`.
- From later than To: no query. Status line: `From is after To`; table and
  lists left as they were.
- Change of either edit or checkbox reloads. Edits use
  `editingFinished`, not `dateTimeChanged`: one query per date entered, not
  one per keystroke.

### 3.2 Irradiation and Level lists

- `kFacets` gains `{Facet::Irradiation, "Irradiation"}`,
  `{Facet::Level, "Level"}`, after Identifier. `query()` fills
  `q.irradiations`, `q.levels`.
- Level list narrows to checked irradiations with no extra code: a facet is
  computed from every other filter.
- Level is matched by name. With no irradiation checked, checking level `A`
  means level A of every irradiation. Accepted; user guide says so.
- Irradiation column shown by default (still `irradiation + " " + level`).
- Six lists no longer fit window height: filter column goes in a
  `QScrollArea` (vertical only, frameless, widget resizable).
- Cost: `update_facets` makes 6 facet statements per reload, was 4.
  Section 5 measures it.

### 3.3 Text filter on filter lists

New widget `FacetBox : QGroupBox` (`apps/pychron-ui/src/facet_box.{hpp,cpp}`),
replaces inline group box + list code in `DataBrowserWindow`.

    class FacetBox : public QGroupBox {
     public:
      explicit FacetBox(const QString& title, QWidget* parent = nullptr);
      void set_values(const QStringList& values);   // keeps checks, reapplies filter
      QStringList checked() const;
      void clear_checked();
      QListWidget* list() const noexcept;
      QLineEdit* filter() const noexcept;
      QToolButton* clear_button() const noexcept;
     signals:
      void changed();                                // a check changed, by user
    };

- Layout: header row `[filter QLineEdit, placeholder "Filter"] [clear ×]`,
  then list (max height 130 as now).
- Filter: case-insensitive substring of item text. Non-matching items hidden
  (`setHidden`). No query, no reload: acts on values already fetched.
- A checked item is never hidden, whatever filter says.
- `set_values`: values not in new set but checked are kept at end (present
  behaviour: "keep a checked value visible even when nothing matches").
  Filter text survives `set_values` and is reapplied. `set_values` emits
  nothing.
- Clear button: enabled only when something checked; unchecks all, emits
  `changed()` once.
- Title shows count when any checked: `Sample (3)`.
- `DataBrowserWindow::facet_list(Facet)` keeps returning the `QListWidget`
  (existing tests use it); new `facet_box(Facet)` returns the box.

### 3.4 Row colour

Pure, widget-free, in `apps/pychron-ui/src/row_colors.{hpp,cpp}`:

    enum class ColorBy { AnalysisType, Tag, Spectrometer, IrradiationLevel, None };

    struct TypeColors {                 // one entry per class of analysis type
      QColor unknown, blank, air, cocktail, detector_ic, other;
    };
    TypeColors default_type_colors(const Theme&);   // unknown, other invalid = no tint

    // Key a row is coloured by under `by`; empty = no tint.
    std::string color_key(ColorBy by, const processing::AnalysisSummary&);
    // Tint for one row. `keys`: sorted distinct keys of rows shown.
    QColor row_color(ColorBy by, const processing::AnalysisSummary&, const TypeColors&,
                     const std::vector<std::string>& keys, const Theme&);

Rules:

- In every mode but Tag, including None: tag other than `ok` and non-empty
  gives `theme().error_bg`. Checked first.
- AnalysisType: class of type as now (`unknown`; `blank*`; `air`;
  `cocktail`; `detector_ic`; anything else `other`), colour from
  `TypeColors`. Invalid colour = no tint.
- Tag: `ok` and empty: no tint. `invalid`: `error_bg`. Any other tag: key =
  tag, category colour (below), so that `omit`, `skip` and a lab's own tags
  can be told apart.
- Spectrometer: key = `mass_spectrometer`. IrradiationLevel: key =
  `irradiation + " " + level`; no irradiation = no tint.
- A key takes category colour `theme().row_category[i % N]`, `i` = index of
  key in `keys`. New `Theme` member
  `std::array<QColor, 8> row_category`, pale tints readable under table
  text in each theme, none equal to `error_bg`.
- Colour of a key can change when a page or filter changes set of keys
  shown. Accepted: colour separates neighbours, it does not name a value.

Model: `AnalysisTableModel::set_coloring(ColorBy, TypeColors)`; keeps sorted
distinct keys, rebuilt on `set_rows` / `append_rows` / `set_coloring`;
emits `dataChanged` for `BackgroundRole` over all rows when colours of
shown rows may have changed. `data(BackgroundRole)` calls `row_color`.

UI:

- Filter column, under "Hide invalid": `Colour by` combo, five entries in
  enum order. Choice kept in QSettings `data_browser/color_by`
  (`analysis_type`, `tag`, `spectrometer`, `irradiation_level`, `none`;
  other text = AnalysisType). Browser itself touches no settings: it has
  `set_color_by` and signal `color_by_changed`; `DataWorkspace` reads the
  key when it makes the browser and writes it on the signal, through the
  settings factory its main window gives it (`DataWorkspace::set_settings`).
- Preferences, Data group: six colour buttons (Unknown, Blank, Air,
  Cocktail, Detector IC, Other), each opens `QColorDialog`, each with "no
  colour" state; `Reset colours` restores `default_type_colors(theme())`.
- `Preferences` gains `std::map<std::string, std::string> browser_type_colors`
  (class name to `#rrggbb`, or empty text for no colour), holding only the
  classes that differ from the theme's, so a class given back to the theme
  follows it. Keys
  `preferences/browser_type_colors/<class>`. Saved values untrusted: a class
  not among the six is ignored, a value that is not `#rrggbb` or empty falls
  back to default for that class. Absent key = default.
- QSettings, not line's local file: display only, `elctl` has no use for it.

### 3.5 Time breaks

Table is not sortable: rows are always newest first. Breaks rely on that.

Pure helper, `libs/processing` `time_breaks.hpp` / `src/time_breaks.cpp`:

    struct TimeBreak {
      std::size_t above;        // index in `rows`; separator is drawn above this row
      double gap_seconds;       // > threshold
      std::size_t session_runs; // analyses of same spectrometer, newer than the gap,
                                // back to previous gap or to first row
      double session_start;     // timestamp of oldest of those
      double session_end;       // timestamp of newest of those
    };
    // `rows` newest first (ties in timestamp: any order). threshold_seconds <= 0: none.
    std::vector<TimeBreak> time_breaks(std::span<const AnalysisSummary> rows, double threshold_seconds);
    std::string gap_text(double seconds);   // "45 min", "14 h 20 min", "3 d 4 h"

Rules:

- Computed per `mass_spectrometer`: for two analyses of one spectrometer
  that are consecutive in time among `rows`, newer `B` and older `A`, a
  break exists when `B.timestamp - A.timestamp > threshold`. Analyses of
  other spectrometers lying between them neither make nor hide a break.
- Separator sits directly above `A` (`above` = index of `A`): above last
  run before the gap. Result sorted by `above`; at most one break per row.
- A break is found only when `A` is in `rows`. Hence appending older rows
  never adds, moves or removes a separator among rows already shown. Model
  relies on this (below); helper's test asserts it.
- Gap is between analyses **shown**. A filter that hides runs widens gaps:
  with one sample checked, days between its runs show as breaks though
  instrument ran throughout. Legacy behaved same. User guide says so;
  section 6 lists true session detection as out of scope.

Model (`AnalysisTableModel`):

- Keeps `rows_` (analyses) and `display_`: `std::vector<Entry>`, entry =
  analysis index or break index. `rowCount()` = `display_.size()`.
- `set_gap_threshold(double seconds)`: rebuild, model reset.
- `set_rows`: rebuild, reset. `append_rows`: compute breaks over all rows,
  append entries for new rows and new breaks with one
  `beginInsertRows(tail)`; display rows already shown untouched (rule
  above).
- Separator row: `flags()` = `Qt::NoItemFlags` (not selectable, not
  current). Column 0 `DisplayRole`:
  `no analyses for 14 h 20 min`, prefixed `<spectrometer>: ` when rows shown
  have more than one spectrometer. `ToolTipRole`:
  `<n> runs above, <start> to <end> UTC`. `BackgroundRole`
  `theme().header_bg`, `TextAlignmentRole` centre, `FontRole` italic.
  `UserRole` (uuid) empty.
- Duration text: `45 min`, `14 h 20 min`, `3 d 4 h` (two largest units,
  minutes dropped from one day up).
- API change, so that no caller can index a separator by mistake:
  - `rows()` unchanged (analyses only).
  - `row(int)` removed. New `const AnalysisSummary* analysis_at(int display_row) const`
    (null for separator), `bool is_break(int display_row) const`,
    `int analysis_count() const`, `int display_row_of(std::size_t analysis_index) const`,
    `QList<int> break_rows() const`.

Browser:

- After every reset or insert, `table_->setSpan(r, 0, 1, ColumnCount)` for
  each separator row (`clearSpans()` first on reset).
- Status line counts analyses: `analysis_count()`, not `rowCount()`.
- `selected_uuids()`, figure and export "all shown", `recall_current()`:
  analyses only. Select-all (Ctrl+A) cannot select a separator (no flags).
- `recall_step(±1)` moves to next display row in that direction that is an
  analysis.
- `select_rows(QList<int>)` (tests) takes analysis indices, maps through
  `display_row_of`.

Preferences: Data group, `Time break after:` `QDoubleSpinBox`, hours, range
0 to 720, step 0.5, default 6, special value text `Off` at 0.
`Preferences::browser_gap_hours`, key `preferences/browser_gap_hours`,
out-of-range saved value = default. `DataWorkspace::apply_preferences(const Preferences&)` replaces
`set_page_size` and hands page size, gap hours and type colours to the
browser. A browser made without a workspace (tests) has no threshold until
`set_gap_hours` is called.

## 4. Files

| File | Change |
|---|---|
| `libs/processing/include/pychron/processing/time_breaks.hpp`, `src/time_breaks.cpp` | new, 3.5 helper |
| `apps/pychron-ui/src/facet_box.{hpp,cpp}` | new, 3.3 |
| `apps/pychron-ui/src/row_colors.{hpp,cpp}` | new, 3.4 |
| `apps/pychron-ui/src/analysis_table_model.{hpp,cpp}` | colouring, display rows, separators |
| `apps/pychron-ui/src/data_browser_window.{hpp,cpp}` | range row, two facets, `FacetBox`, scroll area, colour combo, spans, analysis-only selection |
| `apps/pychron-ui/src/theme.{hpp,cpp}` | `row_category` |
| `apps/pychron-ui/src/preferences.{hpp,cpp}`, `preferences_dialog.{hpp,cpp}` | type colours, gap hours |
| `apps/pychron-ui/src/data_workspace.cpp` (+ whoever applies page size) | pass gap hours, type colours |
| `docs/user/06-data-analysis.md` | "The data browser" section |
| `docs/superpowers/specs/2026-10-02-data-browsing-visualization-design.md` | 11.2 points here |

## 5. Tests

- `tests/processing/test_time_breaks.cpp`: no rows; one row; gap equal to
  threshold is no break, just over is; two spectrometers interleaved, gap in
  one only; threshold 0 and negative; equal timestamps; `session_runs`,
  `session_start`, `session_end`; breaks of first `k` rows are exactly the
  breaks of all rows whose `above < k`, for every `k` (append rule).
- `tests/ui/test_row_colors.cpp` (or slots in `test_data_windows.cpp`): bad
  tag red in the four modes that are not Tag; Tag mode: `invalid` red, `omit`
  a category colour, `ok` none; each type class; category index stable and
  wraps past 8; no irradiation no tint; custom `TypeColors`; invalid colour.
- `tests/ui/test_data_windows.cpp`, new slots:
  - range: From/To set `from`/`to` in `query()` and not `last_hours`; To
    includes its minute; open ends; From after To leaves table and shows
    message; preset after range clears `from`/`to`.
  - irradiation checked narrows Level list and table; `make_source` gains
    irradiation and level on its rows.
  - `FacetBox`: filter hides, checked item stays, filter survives reload,
    clear unchecks and reloads once, title count.
  - colour combo changes `BackgroundRole`; choice restored from QSettings.
  - separators: present at gap, absent at threshold 0; `NoItemFlags`; span;
    status counts analyses; `selected_uuids()` after select-all has no empty
    entry and `analysis_count()` entries; `recall_step` skips; break across
    a page boundary appears with Load more and display rows before it are
    unchanged (compare before and after).
- `tests/ui/test_preferences.cpp`: gap hours and type colours round trip;
  bad saved values fall back.
- Existing slots using `model()->row(r)` move to `analysis_at`.
- Measurement (AGENTS.md, "How to know"), on a copy of a lab's store, in
  commit message of the task that adds the two facets: time of `reload()`
  before and after (4 against 6 facet statements), and `EXPLAIN QUERY PLAN`
  of the Irradiation and Level facet statements. A `SCAN` of `analysis`
  there is reported to developer before anything is added to schema: an
  index is a migration and is not part of this spec.

## 6. Out of scope

- Age filter, Age column, `derived_value` writers: second spec.
- True session detection (a break only where instrument did not run, filter
  or not): needs gaps computed store-side over unfiltered analyses, or an
  experiment/queue id on the browse row. Not here.
- "Today" preset: range covers it.
- Sortable table.
- Moving browse and facet queries off GUI thread; debounce of search box.
- Lists for project, PI, material, load, repository, extract device
  (facets exist; add when asked for).
- Colours per theme for user's type colours: one set, used in every theme.
