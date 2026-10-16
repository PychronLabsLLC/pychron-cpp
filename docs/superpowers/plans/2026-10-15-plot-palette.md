# One plot palette, one plot background: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Live plots and figures share one default palette (the scene's 12 colours) and one default plot background (white); the live plots' colours become adjustable on a new Preferences page "Plots".

**Architecture:** `libs/processing` keeps the palette and exposes it as `default_palette()`. `apps/pychron-ui` gets `plot_style.{hpp,cpp}`: a `PlotStyle` value (series colours, plot background) built from `Preferences`, held process-wide, and a `PlotStyleHub` signal fired when it changes. `apply_application_preferences` (already called at start and on every Apply by both main windows) sets it; each live view listens to the hub and recolours itself. A colour that came from a detector's config is marked as such and never recoloured.

**Tech Stack:** C++20, Qt 6 (Widgets, QSettings, Qt Test), QCustomPlot, GoogleTest (`tests/processing`).

**Spec:** `docs/superpowers/specs/2026-10-13-plot-palette-design.md`

## Global Constraints

- Default palette: the 12 of `libs/processing/src/scene.cpp` `kPalette`, in that order, not reordered: `#1f77b4 #d62728 #2ca02c #ff7f0e #9467bd #8c564b #e377c2 #7f7f7f #bcbd22 #17becf #000080 #000000`.
- Default plot background `#ffffff`, for live plots and figures alike.
- `libs/processing` stays Qt-free.
- QSettings keys: `preferences/plot_background` (`#rrggbb`), `preferences/plot_series/<n>` for n = 1..12 (`#rrggbb`). Only values that differ from the default are written. A stored value that does not parse is ignored (default used), not an error.
- Live palette has 12 entries always; reset gives `default_palette()` exactly.
- A detector colour from the spectrometer config wins over the palette, and a palette change never overwrites it.
- Figures do not follow the Plots page: `figure_common.cpp`, options and presets unchanged.
- Grid and axis colours stay `theme().grid` / `theme().outline`; no contrast correction for a dark background.
- No colour literal for a series or a plot background outside `scene.cpp` (palette) and `plot_style.cpp` (white). `tests/ui/test_theme.cpp` `no_widget_source_names_a_colour_by_value` enforces the UI half.
- UI text spells "color" (`test_preferences` `no_text_of_the_dialog_says_colour`).
- Read `libs/processing/AGENTS.md` before Task 1 and `apps/pychron-ui/AGENTS.md` before Task 2.
- Checks per task (root `AGENTS.md`): `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --changed` (`--build-dir build/dev-ui` for `apps/pychron-ui`, `tests/ui`); build the changed test targets only and run them directly, filtered. Full `cmake --build --preset dev-ui`, `ctest --preset dev-ui` and whole-tree sweep only in Task 7.

## Review Focus

- A detector with a config colour, then a palette edit on the Plots page: its trace, its intensities-table swatch and its evolution panel keep the config colour (Task 5 tests `a_config_color_survives_a_palette_change` in strip chart, evolutions).
- Peak hopping (two isotopes on one detector) in the evolutions view, then a palette change: the two lines must still differ after recolouring (Task 5 `palette_change_keeps_peak_hop_lines_apart`).
- A QSettings file from an older build (no plot keys) and one with a hand-edited bad value (`"pink"`, `"#12345"`, `plot_series/13`, `plot_series/0`): loads as the default, no crash (Task 3 `bad_plot_values_read_as_default`).
- A user picks a colour equal to the default: nothing written, so a later change of the default reaches them (Task 3 `a_default_color_is_not_written`).
- Restore Defaults vs Reset colors on the Plots page, then Cancel: the open plots do not change until OK/Apply (Task 4 `reset_changes_fields_only_until_applied`).

---

### Task 1: `default_palette()` in `libs/processing`

**Files:**
- Modify: `libs/processing/include/pychron/processing/scene.hpp:24-25`
- Modify: `libs/processing/src/scene.cpp:21-26,61-64`
- Create: `tests/processing/test_scene.cpp` (picked up by the glob in `tests/processing/CMakeLists.txt`)

**Interfaces:**
- Produces: `std::span<const pychron::processing::Color> pychron::processing::default_palette();` 12 entries. `Color palette_color(int index)` unchanged in signature: `default_palette()[((index % 12) + 12) % 12]`.

- [x] **Step 1: Write the failing tests** in `tests/processing/test_scene.cpp`:

```cpp
TEST(Palette, DefaultHasTwelveInOrder) {
  const auto p = pp::default_palette();
  ASSERT_EQ(p.size(), 12u);
  EXPECT_EQ(pp::color_hex(p[0]), "#1f77b4");
  EXPECT_EQ(pp::color_hex(p[6]), "#e377c2");
  EXPECT_EQ(pp::color_hex(p[10]), "#000080");
  EXPECT_EQ(pp::color_hex(p[11]), "#000000");
}
TEST(Palette, PaletteColorCyclesTheDefault) {
  const auto p = pp::default_palette();
  for (int i = 0; i < 12; ++i) EXPECT_EQ(pp::palette_color(i), p[static_cast<std::size_t>(i)]);
  EXPECT_EQ(pp::palette_color(12), p[0]);
  EXPECT_EQ(pp::palette_color(25), p[1]);
}
TEST(Palette, NegativeIndexWrapsFromTheEnd) {
  EXPECT_EQ(pp::palette_color(-1), pp::default_palette()[11]);
  EXPECT_EQ(pp::palette_color(-13), pp::default_palette()[11]);
}
```

- [x] **Step 2: Run, expect a compile failure** (`default_palette` undeclared):
`cmake --build --preset dev --target pychron_processing_tests`

- [x] **Step 3: Declare `default_palette()` in `scene.hpp`** (add `#include <span>`), define it in `scene.cpp` over `kPalette`; `palette_color` reads it.

- [x] **Step 4: Run, expect PASS:**
`build/dev/tests/processing/pychron_processing_tests --gtest_filter='Palette.*:TimeSeries.*:FluxView.*'`

- [x] **Step 5: Checks:** `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --changed`.

- [x] **Step 6: Commit:** `feat(processing): default_palette() exposes the 12 group colors`

---

### Task 2: `PlotStyle` with defaults; every live plot reads it (screen change of spec 4.1)

Lands alone: with nothing configured, live plots turn white and series 7, 8 change cyan → pink, black → grey.

**Files:**
- Create: `apps/pychron-ui/src/plot_style.hpp`, `apps/pychron-ui/src/plot_style.cpp`; add `src/plot_style.cpp` beside `src/theme.cpp` in `apps/pychron-ui/CMakeLists.txt:71`
- Modify: `apps/pychron-ui/src/theme.hpp:4,54,106` (remove `plot_bg`, `series`; fix the comment naming `theme().plot_bg`), `apps/pychron-ui/src/theme.cpp:46,91-92`
- Modify: `apps/pychron-ui/src/strip_chart_model.{hpp,cpp}` (`palette_color` reads `plot_style()`; header comment "12 colours" → "the live palette, cycled")
- Modify (background → `plot_style().plot_background`): `strip_chart_view.cpp:45`, `evolutions_view.cpp:252`, `peak_center_view.cpp:64`, `history_chart.cpp:48`, `pattern_maker_window.cpp:102`
- Modify (series): `history_chart.cpp:52` (`plot_style().series[0]`), `holder_view.cpp:66-72` (`plot_series_color(i++)`; comment "the live palette"), `evolutions_view.cpp:32` (`kPaletteSize` removed; loop to `plot_style().series.size()`)
- Modify: `tests/ui/test_theme.cpp:307` (skip `plot_style.cpp` as well as `theme.cpp`; comment above says why: white is the plot default, spec 5)
- Test: `tests/ui/test_plot_style.cpp` (new), `tests/ui/test_strip_chart_model.cpp:262-268`

**Interfaces:**
- Consumes: `processing::default_palette()` (Task 1).
- Produces, in `plot_style.hpp`, namespace `pychron::ui`:

```cpp
struct PlotStyle {                 // live plots only
  std::vector<QColor> series;      // never empty; default: default_palette()
  QColor plot_background;          // default #ffffff
  bool operator==(const PlotStyle&) const = default;
};
PlotStyle default_plot_style();
const PlotStyle& plot_style();                // current; GUI thread
QColor plot_series_color(std::size_t slot);  // plot_style().series[slot % size]
```

- [x] **Step 1: Write the failing tests** in `tests/ui/test_plot_style.cpp` (Qt Test, class `TestPlotStyle`):
  - `default_series_are_the_figure_palette`: `default_plot_style().series.size() == 12`; for each i, `series[i] == QColor(c.r, c.g, c.b)` of `processing::default_palette()[i]`.
  - `default_background_is_white`: `default_plot_style().plot_background == QColor(QStringLiteral("#ffffff"))`.
  - `current_style_starts_as_default`: `plot_style() == default_plot_style()`.
  - `series_color_cycles_twelve`: `plot_series_color(12) == plot_series_color(0)`, `plot_series_color(6).name() == "#e377c2"`.
  - `a_live_plot_and_a_figure_draw_series_n_alike` (spec 5, rule 1): for n in 0..11, `StripChartModel::palette_color(n)` equals the `QColor` of `processing::palette_color(n)`.
  - `strip_chart_background_is_the_plot_style`: a `StripChartView` over a one-detector model; `plot()->axisRect()->backgroundBrush().color() == plot_style().plot_background`. (If `StripChartView` has no plot accessor, add `QCustomPlot* plot() const noexcept` "for tests", as other views have.)
- [x] **Step 2: Change `test_strip_chart_model.cpp:267`** to `QCOMPARE(StripChartModel::palette_color(12), StripChartModel::palette_color(0));` and add `QVERIFY(StripChartModel::palette_color(8) != StripChartModel::palette_color(0));`.
- [ ] **Step 3: Run, expect build failure** (`plot_style.hpp` missing): `cmake --build --preset dev-ui --target pychron_ui_test_plot_style`
- [x] **Step 4: Implement `plot_style.{hpp,cpp}`** (white is the one colour literal here) and rewire every file listed above; remove `Theme::series`, `Theme::plot_bg`.
- [x] **Step 5: Run, expect PASS:**
`cmake --build --preset dev-ui --target pychron_ui_test_plot_style pychron_ui_test_strip_chart_model pychron_ui_test_theme pychron_ui_test_evolutions_view pychron_ui_test_peak_center_view pychron_ui_test_history_chart pychron_ui_test_pattern_maker pychron_ui_test_laser_views pychron_ui_test_spectrometer_window`, then run each binary under `build/dev-ui/tests/ui/` with `QT_QPA_PLATFORM=offscreen`. A test that named `theme().series` or 8 colours is changed to the 12, not removed (spec 6).
- [x] **Step 6: Checks:** `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`.
- [x] **Step 7: Commit:** `feat(ui): live plots use the figure palette on white`

---

### Task 3: Preferences keys, load / store, applied at start and on Apply

**Files:**
- Modify: `apps/pychron-ui/src/preferences.hpp` (two fields), `apps/pychron-ui/src/preferences.cpp` (keys, load, save, `apply_application_preferences`)
- Modify: `apps/pychron-ui/src/plot_style.{hpp,cpp}`
- Test: `tests/ui/test_preferences.cpp`, `tests/ui/test_plot_style.cpp`

**Interfaces:**
- Consumes: Task 2's `PlotStyle`, `default_plot_style()`, `plot_style()`.
- Produces:
  - In `Preferences`:
    ```cpp
    // Live plots (plot_style.hpp), where they differ from the default:
    // "#rrggbb"; empty plot_background: the default.
    std::string plot_background;
    std::map<int, std::string> plot_series_colors;  // 1..12 -> "#rrggbb"
    ```
  - In `plot_style.hpp`:
    ```cpp
    PlotStyle plot_style_from(const Preferences& preferences);       // defaults where nothing (valid) is kept
    void store_plot_style(const PlotStyle& style, Preferences& preferences);  // only what differs from default
    void set_plot_style(PlotStyle style);  // empty or short series: filled from the default up to 12
    ```
  - `apply_application_preferences(p)` calls `set_plot_style(plot_style_from(p))`.

- [ ] **Step 1: Write the failing tests** in `test_preferences.cpp`:
  - `plot_colors_round_trip`: `p.plot_background = "#202020"; p.plot_series_colors = {{1, "#abcdef"}, {12, "#123456"}};` save, load, `QCOMPARE(load, p)`; then erase key 12, save, load equal again (key `preferences/plot_series/12` gone).
  - `a_default_color_is_not_written`: `PlotStyle s = default_plot_style(); s.series[2] = QColor("#010203"); Preferences p; store_plot_style(s, p);` → `p.plot_series_colors == {{3, "#010203"}}`, `p.plot_background.empty()`; after `save_preferences`, `settings()->contains("preferences/plot_background") == false`, `childKeys` of group `preferences/plot_series` is `{"3"}`.
  - `bad_plot_values_read_as_default`: set `preferences/plot_background` = `"pink"`, `preferences/plot_series/1` = `"#12345"`, `/0` and `/13` = `"#000000"`, `/2` = `"#ABCDEF"`; load → `plot_background.empty()`, `plot_series_colors == {{2, "#abcdef"}}` (upper case read lower, as `browser_type_colors`).
  - `nothing_saved_reads_as_the_defaults` (existing) still passes: defaults of the two new fields are empty.
- [ ] **Step 2: Write the failing tests** in `test_plot_style.cpp`:
  - `style_from_preferences_overrides_only_named_slots`: `plot_series_colors = {{7, "#00ff00"}}` → `series[6] == "#00ff00"`, `series[7]` default, size 12.
  - `store_then_read_is_identity` for a style with background and 3 series changed.
  - `applying_preferences_sets_the_current_style`: `apply_application_preferences(p)` with background `#202020` → `plot_style().plot_background.name() == "#202020"`; `apply_application_preferences(Preferences{})` → `plot_style() == default_plot_style()` (also the `cleanup()`).
- [ ] **Step 3: Run, expect build failure.** `cmake --build --preset dev-ui --target pychron_ui_test_preferences pychron_ui_test_plot_style`
- [ ] **Step 4: Implement.** Keys `preferences/plot_background`, `preferences/plot_series/<n>`; save removes the `preferences/plot_series` group and `plot_background` key before writing what is set (as `kTypeColors`). Valid text: `#rrggbb` (reuse `is_type_color_text`, non-empty), lower-cased; slot outside 1..12 ignored.
- [ ] **Step 5: Run both binaries, expect PASS.**
- [ ] **Step 6: Checks** (as Task 2 Step 6).
- [ ] **Step 7: Commit:** `feat(ui): plot colors kept in preferences`

---

### Task 4: Preferences page "Plots"

**Files:**
- Modify: `apps/pychron-ui/src/preferences_dialog.{hpp,cpp}` (page after Data, line 153; `values()`, `set_values()`; header comment's page list)
- Test: `tests/ui/test_preferences.cpp` (new cases; page counts and rows after Data move by one: lines 198, 237-238, 327, 347, 392, 456, 461-463, 697)

**Interfaces:**
- Consumes: `store_plot_style`, `plot_style_from`, `default_plot_style` (Task 3); `ColorField`.
- Produces:
  - `ColorField::set_can_be_none(bool)`: false hides `none()` and `color()` is never invalid. Plot fields use it.
  - `PreferencesDialog` accessors for tests: `ColorField* plot_background() const noexcept;`, `ColorField* plot_series(std::size_t slot) const;` (0..11, null past the end), `QPushButton* reset_plot_colors() const noexcept;`.

- [ ] **Step 1: Write the failing tests** in `test_preferences.cpp`:
  - `plots_page_follows_data`: `pages()->item(2)->text() == "Plots"`; without spectrometer `count() == 4`.
  - `plots_page_shows_and_gives_back_the_colors`: dialog over `Preferences` with `plot_series_colors = {{4, "#445566"}}`, `plot_background = "#eeeeee"`; `plot_series(3)->color().name() == "#445566"`, `plot_background()->color().name() == "#eeeeee"`, `!plot_series(0)->none()->isVisible()`; `plot_series(12) == nullptr`; set `plot_series(0)` to `#000001`, `values().preferences.plot_series_colors == {{1, "#000001"}, {4, "#445566"}}`.
  - `reset_plot_colors_gives_the_default_palette`: after reset, every `plot_series(i)->color() == default_plot_style().series[i]`, background white, `values().preferences.plot_series_colors.empty()`, `plot_background.empty()`.
  - `reset_changes_fields_only_until_applied`: open via `MainWindow` preferences path with a background set and applied; press Reset colors, then Cancel; `plot_style().plot_background` still the applied one.
  - `restore_defaults` case (existing `dialog_shows_the_values_and_restores_defaults`) extended: plot fields back to default.
  - Note text present on page: `"Used by the live plots (scan, evolutions, cryostat, heaters). A figure has its own colors in its options."`
- [ ] **Step 2: Run, expect FAIL** (`pychron_ui_test_preferences`).
- [ ] **Step 3: Implement the page**: `QFormLayout`; row "Plot background:" ColorField; label "Series colors:" then 12 rows "1:" .. "12:" each a ColorField; "Reset colors" button (`reset_plot_colors_`, separate from the Data page's `reset_colors_`); then `preferences_note(...)` with the text above. Update the page counts in existing tests.
- [ ] **Step 4: Run `pychron_ui_test_preferences` and `pychron_ui_test_earthbank_preferences`, expect PASS.**
- [ ] **Step 5: Checks** (as Task 2 Step 6).
- [ ] **Step 6: Commit:** `feat(ui): Plots page in Preferences`

---

### Task 5: Open views follow an applied change; config colours stay

Departs from spec 4.2 wording ("each window hands the new style to its live plots"): the hand-over is a signal, so no window needs a new method and a view opened later reads `plot_style()` anyway. `MainWindow` / `DataMainWindow::apply_preferences` remain the path (they call `apply_application_preferences`). Spec 4.2 is changed in this task to say so and to list the views below (it left the list to the plan).

**Files:**
- Modify: `apps/pychron-ui/src/plot_style.{hpp,cpp}` (hub)
- Modify: `strip_chart_model.{hpp,cpp}` (`DetectorSeries::palette_slot`, `restyle()`), `strip_chart_view.{hpp,cpp}`, `intensities_model.{hpp,cpp}`, `spectrometer_bridge.cpp:105-109`, `heater_dock.cpp:18,45`, `cryo_dock.cpp:17,44`
- Modify: `evolutions_view.{hpp,cpp}`, `experiment_window.cpp:41-50,64`
- Modify: `peak_center_view.{hpp,cpp}`, `history_chart.{hpp,cpp}`, `pattern_maker_window.cpp`, `holder_view.{hpp,cpp}`
- Modify: `docs/superpowers/specs/2026-10-13-plot-palette-design.md` section 4.2 (compressed, as the spec is)
- Test: `tests/ui/test_strip_chart_model.cpp`, `test_evolutions_view.cpp`, `test_peak_center_view.cpp`, `test_history_chart.cpp`, `test_cryo_dock.cpp`, `test_heater_dock.cpp`, `test_spectrometer_window.cpp`

**Interfaces:**
- Consumes: `set_plot_style`, `plot_series_color` (Tasks 2, 3).
- Produces:
  - `class PlotStyleHub : public QObject { Q_OBJECT public: static PlotStyleHub& instance(); signals: void changed(); };` `set_plot_style` emits `changed()` only when the style differs.
  - `DetectorSeries::palette_slot` (`std::optional<std::size_t>`): set means the colour is `plot_series_color(*palette_slot)` and follows the palette; unset means the colour is the config's and is never changed. Set by `StripChartModel` / `IntensitiesModel` constructors for an invalid colour (slot = index), by `SpectrometerBridge::detectors()` where the config gives none, and by heater / cryo docks.
  - `void StripChartModel::restyle();` `void IntensitiesModel::restyle();` (emits `dataChanged` on the colour column): recolour slotted series.
  - `EvolutionsView(std::map<std::string, QColor> colors = {}, std::map<std::string, std::size_t> palette_slots = {}, QWidget* parent = nullptr)`: `colors` only config colours; `palette_slots` detector → index for detectors without one (`experiment_window.cpp` `detector_colors` split into the two maps). `Line` gains `std::optional<std::size_t> palette_slot`; the first-unused-colour search records the slot it picked.
  - Each view connects to `PlotStyleHub::changed` in its constructor and, on it, sets the axis-rect background(s), re-pens slotted graphs and replots: `StripChartView`, `EvolutionsView`, `PeakCenterView`, `HistoryChart`; `PatternMakerWindow`'s canvas and `HolderView` call `update()`; heater / cryo docks refresh their name labels' colour.

- [ ] **Step 1: Write the failing tests** (each restores `set_plot_style(default_plot_style())` in `cleanup()`):
  - `test_strip_chart_model`: `restyle_recolors_palette_series_only`: model over `{{"H1", QColor("#123456")}, {"AX", {}}}`; `set_plot_style` with `series[1] = "#00ff00"`; `restyle()`; `detectors()[0].color.name() == "#123456"`, `detectors()[1].color.name() == "#00ff00"`.
  - `test_spectrometer_window`: `a_config_color_survives_a_palette_change`: a fixture detector given `color = "#123456"` in config, another without; `apply_application_preferences` with every series slot changed; the configured trace pen and intensities swatch still `#123456`, the other is the new slot colour; `plot()->axisRect()->backgroundBrush().color()` the new background.
  - `test_evolutions_view`: `palette_change_recolors_open_panels` (background and unconfigured line pen follow); `a_config_color_survives_a_palette_change`; `palette_change_keeps_peak_hop_lines_apart` (two isotopes on one detector stay different colours after the change).
  - `test_peak_center_view`, `test_history_chart`: background and series pen follow a `set_plot_style`.
  - `test_cryo_dock`, `test_heater_dock`: name label colour follows (`styleSheet()` contains the new `name()`).
- [ ] **Step 2: Run, expect FAIL** (build the seven targets; run each).
- [ ] **Step 3: Implement** the hub, slots and restyle per the Interfaces block. Every view's background and series pens come from `plot_style()`; no literal.
- [ ] **Step 4: Run the seven binaries plus `pychron_ui_test_experiment_window`, `pychron_ui_test_spectrometer_bridge`, `pychron_ui_test_laser_views`, `pychron_ui_test_pattern_maker`; expect PASS.**
- [ ] **Step 5: Amend spec 4.2** ("Apply" bullet): style set by `apply_application_preferences`; views listen to `PlotStyleHub::changed`; list of views as above; config-colour rule unchanged.
- [ ] **Step 6: Checks** (as Task 2 Step 6).
- [ ] **Step 7: Commit:** `feat(ui): open live plots follow a change of plot colors`

---

### Task 6: Docs and the component rule

**Files:**
- Modify: `docs/user/01-getting-started.md` section 9 (table: rows for Plots / **Plot background**, **Series colors**, **Reset colors**; one line that figures keep their own colors in their options and presets)
- Modify: `apps/pychron-ui/AGENTS.md` (compressed): new live view takes series colours and background from `plot_style()` and listens to `PlotStyleHub::changed`, never a literal or `theme()`; a colour from config carries no `palette_slot`
- Modify: `docs/user/03-spectrometer.md` only if it states the light yellow background or 8 colours (`grep -n "yellow\|8 colo" docs/user`)

- [ ] **Step 1: Edit the three files.** Docs only: no build.
- [ ] **Step 2: Commit:** `docs(ui): Plots preferences page and the live palette rule`

---

### Task 7: Whole-branch verification

- [ ] **Step 1:** Rebase on `origin/develop` if it moved.
- [ ] **Step 2:** `python3 tools/quality_check.py` → exit 0.
- [ ] **Step 3:** `python3 tools/compiler_sweep.py --build-dir build/dev-ui` (whole tree) → no warnings.
- [ ] **Step 4:** `cmake --build --preset dev-ui` then `ctest --preset dev-ui` (timeout 3600000 ms) → all pass. A failure is root-caused, never skipped.
- [ ] **Step 5:** `grep -rn "plot_bg\|theme().series\|kPaletteSize" apps tests` → nothing.
- [ ] **Step 6:** Whole-branch review on Sonnet (root `AGENTS.md`, Delegated reviews) against the spec; fix findings in their own commits.

## Open questions

None.
