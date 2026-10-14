# One plot palette, one plot background: design

Status: draft, 2026-10-13 (nightly). Not agreed. Open points: section 8.
Task T050 (from T016). Owner 2026-10-13: share one palette and one
background, white by default; all colours adjustable; live plots get
preferences in Preferences window.

## 1. Problem

Two palettes, two plot backgrounds, no reason for difference.

| | Live plots | Figures |
|---|---|---|
| Who | `StripChartView` (scan, cryo, heaters), `EvolutionsView`, intensities table, holder view's project colours | every `processing::Scene` (ideogram, spectrum, isochron, series, flux, blanks / IC, isotope evolution refit, recall) |
| Series colours | `theme().series`, 8 (`apps/pychron-ui/src/theme.cpp:91-92`), through `StripChartModel::palette_color` (`strip_chart_model.cpp:22`) | `kPalette`, 12 (`libs/processing/src/scene.cpp:22-26`), through `palette_color(int)` (`scene.cpp:61`) |
| Plot background | `theme().plot_bg` `#fafad2` (`theme.cpp:46`; `strip_chart_view.cpp:45`, `evolutions_view.cpp:81`, `pattern_maker_window.cpp:102`) | `plot_background` option, default `#ffffff` (`figure_common.cpp:53-54`, fallback `:81-82`) |
| User can change | detector colour only, from spectrometer config (`DetectorSeries::color`) | background, plot background, per-group colour, in options editor; saved in presets |

First 6 colours already equal (`#1f77b4 #d62728 #2ca02c #ff7f0e #9467bd
#8c564b`). Theme's 7 and 8: `#17becf`, `#000000`. Scene's 7 to 12: `#e377c2
#7f7f7f #bcbd22 #17becf #000080 #000000`.

## 2. Goals

- One default palette: scene's 12. One default plot background: white.
- Live plots: palette and plot background adjustable, page "Plots" in
  Preferences.
- Figures: unchanged in what they offer (already adjustable per figure and
  per preset); their defaults follow the same values.
- Nothing saved before breaks: a preset, a detector colour in a config, a
  QSettings key written by an older build all still load.

## 3. Out of scope

- Dark theme. One theme exists; not added here.
- Per-plot options for live plots (zoom, save image, tooltips): T048.
- Row colours of data browser (`row_category`, Preferences > Data): other
  purpose (row tint), stay.
- Canvas source colours (`theme().sources`), syntax colours.
- Font of live plots.

## 4. Design

### 4.1 One palette, owned by `libs/processing`

- `palette_color(int)` and `kPalette` stay where they are: `libs/processing`
  has no Qt and UI already depends on it (`SceneView`,
  `apps/pychron-ui/src/scene_view.cpp:33` converts `Color` to `QColor`).
- New in `scene.hpp`: `std::span<const Color> default_palette();` (the 12).
  `palette_color(i)` is `default_palette()[i mod 12]`.
- `Theme::series` removed. `StripChartModel::palette_color(i)` reads the
  live-plot palette of 4.2 instead; its callers do not change
  (`evolutions_view.cpp:136-139`, `experiment_window.cpp:46`,
  `heater_dock.cpp:18,45`, `cryo_dock.cpp:17,44`,
  `spectrometer_bridge.cpp:108`, `intensities_model.cpp:18`).
  `evolutions_view.cpp:30` `kPaletteSize = 8` becomes the palette's size.
- `holder_view.cpp:72` (project colours) reads same palette.
- Effect on screen with nothing set: series 1 to 6 keep their colour; series
  7 changes cyan to pink, series 8 black to grey. A detector with a colour
  in spectrometer config keeps it (config wins, as today).

### 4.2 Live plot style (`apps/pychron-ui`)

```cpp
struct PlotStyle {                 // live plots only
  std::vector<QColor> series;      // never empty; default: default_palette()
  QColor plot_background;          // default #ffffff
};
const PlotStyle& plot_style();     // current; GUI thread
```

- New `plot_style.{hpp,cpp}`. Built from `Preferences` at start and again
  when Preferences is applied.
- `Theme::plot_bg` removed; `strip_chart_view.cpp:45`,
  `evolutions_view.cpp:81`, `pattern_maker_window.cpp:102` read
  `plot_style().plot_background`.
- Grid and axis colours stay theme's (`grid`, `outline`): readable on white
  and on light yellow. A plot background darker than mid grey is accepted
  as given; no contrast correction (section 8, point 3).
- Apply: the main window and the data window each have
  `apply_preferences(const Preferences&)` (`main_window.hpp:155`,
  `data_main_window.hpp:37`, `data_workspace.hpp:52`), called when the
  dialog is applied. Same path: each window hands the new style to its live
  plots, which set background and series pens again and replot. Which other
  windows hold a live plot and have no such method yet (experiment,
  spectrometer) is for the plan to list. Rule: a colour that came from config or from the user for one
  detector is not overwritten by a palette change.
- New views that draw live (`PeakCenterView`, gauge history, rise rate
  marks) take series colours and background from `plot_style()`, never
  literals. Stated in `apps/pychron-ui/AGENTS.md`.

### 4.3 Preferences page "Plots"

- After "Data" (`preferences_dialog.cpp:153`). QSettings, as Appearance and
  Data are: plot colours are this workstation's, `elctl` has no use for
  them, so the local-override-file rule of `apps/pychron-ui/AGENTS.md` does
  not apply.
- Controls: "Plot background" colour button; "Series colors" list of 12
  swatches, each a colour button; "Reset colors" (both back to defaults).
  Label text "color", as T079 / T081.
- Keys: `preferences/plot_background` (`#rrggbb`),
  `preferences/plot_series/<n>` for n = 1..12 (`#rrggbb`). Only values that
  differ from default are written, as `browser_type_colors` does
  (`type_color_overrides`, `row_colors.hpp:50`): a later change of the
  default reaches every user who did not choose.
- A stored value that does not parse is ignored (default used), not an
  error.
- Note under controls: "Used by the live plots (scan, evolutions, cryostat,
  heaters). A figure has its own colors in its options."

### 4.4 Figures

- Defaults unchanged in value: `background` and `plot_background`
  `#ffffff`, groups from the 12.
- Decision needed (section 8, point 1): whether a figure whose options
  never set them follows the Plots page too. Draft says no: a figure's
  look is its preset's, a preset travels between workstations and with a
  publication, and QSettings of one machine must not change it. "Shared"
  then means same defaults from one definition, each side adjustable in
  its own place.

## 5. Rules

- With nothing set anywhere, a live plot and a figure draw group or series
  n in same colour, on white.
- A preset saved before this change loads and draws as before.
- A detector colour in spectrometer config wins over palette.
- Palette has 12 entries always; reset gives `default_palette()` exactly.
- No colour literal for a series or a plot background outside
  `scene.cpp` (palette) and `plot_style.cpp` (white).

## 6. Tests

- `tests/processing`: `default_palette()` has 12, `palette_color` cycles it,
  negative index as today.
- `tests/ui` `test_preferences`: plot keys round trip; only changed entries
  written; bad text ignored; reset removes keys.
- `tests/ui` strip chart and evolutions: background and series pens are
  `plot_style()`'s; change through Preferences reaches an open view;
  a config colour survives a palette change.
- Existing tests that name `theme().series` or 8 colours: changed to the 12,
  not removed.

## 7. Steps (for the plan)

1. `default_palette()`; `Theme::series` and `StripChartModel::palette_color`
   read it; `plot_bg` white. Screen changes as 4.1 says. Lands alone.
2. `PlotStyle`, Preferences keys and their load / store.
3. Plots page.
4. Open views follow an applied change.
5. Docs (`docs/user`), `apps/pychron-ui/AGENTS.md` rule.

## 8. Open points for owner

1. Figures follow Plots page when their options are untouched, or keep own
   defaults (draft: keep; 4.4)?
2. Series 7 and 8 of live plots change colour (cyan to pink, black to
   grey). Acceptable, or reorder the 12 so that first 8 are today's 8?
   Reordering changes figures' groups 7 to 12 instead.
3. Dark plot background chosen by user: leave axis and grid as theme's
   (draft), or pick contrasting ones?
4. Pattern maker's canvas uses `plot_bg` too
   (`pattern_maker_window.cpp:102`): white with the rest (draft), or its own
   colour?
5. Holder view's project colours: same palette (draft), or leave on a
   separate 8?
