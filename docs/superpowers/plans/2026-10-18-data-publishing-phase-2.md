# Data publishing phase 2: figures in a release — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `elctl publish build --with-figures` writes an ideogram, age spectra and an inverse isochron per sample as SVG and PDF into the release directory, listed in the manifest with hashes, byte-identical across builds, checked by `verify`.

**Architecture:** A Qt-free layout of a `processing::Scene` (axis ranges, ticks, panel geometry, Helvetica metrics) in `libs/processing` turns a scene into a draw list of paths and texts in points. Two thin serializers write that list as SVG text and as an uncompressed one-page PDF 1.4. `SceneView` takes its axis ranges from the same layout code, and a UI test compares what it places with the draw list. `libs/publish` builds the three figures per sample from the release's dataset and writes them through the build's `Writer`.

**Tech Stack:** C++20, GoogleTest, QtTest (UI parity test only). No new dependency: the PDF and SVG writers are hand-written, Helvetica widths are a compiled-in table of the standard 14 font metrics.

**Spec:** `docs/superpowers/specs/2026-10-18-data-publishing-and-hosted-sharing-design.md` (sections 5.1, 5.4, 5.6, 5.7, 5.8, 6.1, 6.2, 12, 13 phase 2).

## Global Constraints

- Qt stays out of `libs/`: every new file under `libs/processing` and `libs/publish` is Qt-free.
- Figure path: `figures/<sample>/<kind>.pdf` and `<kind>.svg`; kind `ideogram`, `spectrum`, `isochron`; `spectrum-<aliquot>.{pdf,svg}` when the sample has more than one step-heated aliquot. `<sample>` is `sample_dir_name` (phase 1, already file-safe and unique).
- SVG: text, deterministic, fonts named not embedded.
- PDF: PDF 1.4, one page, vector paths, standard 14 fonts (Helvetica), `CreationDate` = release `created_utc`, no `/ID`, no compression, xref table correct.
- Same Scene twice gives the same bytes; every figure file is in `manifest.files` with its sha256 and media type (`image/svg+xml`, `application/pdf`, already in `media_type_of`).
- Every file written goes through `pychron::mark_as_user_file` (the build's `Writer` does this).
- Stored formats are compiler-independent: a committed SVG and PDF fixture is compared byte for byte by a test, so Windows and Linux CI check the bytes a Mac wrote. Numbers are written with `std::to_chars`, never iostreams or `printf` with a locale.
- `UI keeps QPdfWriter for interactive export`: `SceneView::save_pdf` and `save_png` are not changed.
- Commits: Conventional Commits, scope `processing`, `ui`, `publish`, `elctl` or `docs`.

## Review Focus

- A scene with no graphs, or a panel with no data, must still give a valid SVG and a PDF that parses (Task 4, Task 5 tests `EmptySceneIsAValidDocument`).
- NaN or infinite values in a layer (a failed reduction) must be skipped, never written as `nan`/`inf` into SVG or PDF (Task 2 test `NonFiniteValuesAreSkipped`).
- Sample names, titles and labels with `<`, `&`, `"`, `(`, `)`, `\` and non-ASCII (`±`, `σ`, `é`) must be escaped in SVG and encoded in PDF; a character Helvetica cannot show becomes `?`, `σ` uses the Symbol font (Task 4 `EscapesMarkup`, Task 5 `EncodesTextForTheStandardFonts`).
- A sample for which a figure cannot be built (one analysis, no ages, an isochron of fewer than three points) must not fail the release: the figure is skipped with a warning naming the sample and kind (Task 6 `ASampleWithoutEnoughPointsSkipsTheIsochronWithAWarning`).
- All values equal (zero-width range) and a log axis with non-positive values must give a finite range and a bounded number of ticks (Task 1 `ZeroWidthRangeIsPaddedAsTheView`, `LogRangeIgnoresNonPositiveValues`, `TicksAreBounded`).

---

## File structure

| File | Responsibility |
|---|---|
| `libs/processing/include/pychron/processing/scene_layout.hpp`, `src/scene_layout.cpp` | Axis ranges (moved from `SceneView::reset_view`), ticks (linear, log, time, category), Helvetica text widths, geometry of graphs and panels |
| `libs/processing/include/pychron/processing/scene_draw.hpp`, `src/scene_draw.cpp` | Scene + layout -> `DrawList` (paths, texts, clip boxes) in points, y down; coordinate formatting shared by the writers |
| `libs/processing/include/pychron/processing/scene_svg.hpp`, `src/scene_svg.cpp` | `DrawList` -> SVG text |
| `libs/processing/include/pychron/processing/scene_pdf.hpp`, `src/scene_pdf.cpp` | `DrawList` -> PDF 1.4 bytes; save to a path |
| `tests/processing/test_scene_layout.cpp`, `test_scene_draw.cpp`, `test_scene_svg.cpp`, `test_scene_pdf.cpp` | One suite per file above |
| `tests/processing/fixtures/scene/figure.svg`, `figure.pdf` | Committed stored-format fixtures |
| `apps/pychron-ui/src/scene_view.cpp` | `reset_view` uses the shared ranges |
| `tests/ui/test_scene_view.cpp` | Parity of SceneView with the draw list |
| `libs/publish/include/pychron/publish/figures.hpp`, `src/figures.cpp` | The figures of one sample: options from the release, file names, bytes |
| `libs/publish/src/build.cpp`, `src/records.cpp`, `include/pychron/publish/build.hpp`, `manifest.hpp`, `src/manifest.cpp` | Figures written in the build; formats in records; `has_figures` |
| `tests/publish/test_figures.cpp`, `test_build.cpp`, `test_elctl_publish.cpp` | Build and command tests |
| `apps/elctl/src/publish.cpp` | `--with-figures` on build, `verify` rebuilds with figures when the manifest has them |
| `docs/release_format.md`, `docs/user/08-elctl-reference.md`, the spec | Docs and spec amendments |

---

### Task 1: Scene layout: ranges, ticks, metrics, geometry

**Files:**
- Create: `libs/processing/include/pychron/processing/scene_layout.hpp`, `libs/processing/src/scene_layout.cpp`
- Test: `tests/processing/test_scene_layout.cpp`

**Interfaces:**
- Consumes: `scene.hpp` (`Scene`, `Graph`, `Panel`, `Axis`, layers).
- Produces (namespace `pychron::processing`):

```cpp
struct Range { double min = 0.0, max = 1.0; friend bool operator==(const Range&, const Range&) = default; };
// x of a graph: Axis min and max when both are set, else the extent of its
// panels' data (PointLayer x +- x_err of shown points, LineLayer, BandLayer,
// EllipseLayer x +- scale*sx), no padding; (0, 1) when there is none.
Range graph_x_range(const Graph& graph);
// y of a panel, as SceneView::reset_view did: extent of PointLayer y +- y_err,
// LineLayer, BandLayer low/high, EllipseLayer y +- scale*sy (StepLayer,
// GuideLayer, SpanLayer and TextLayer are not data); linear: pad 8% of the
// size (size 0: max(|max| * 0.05, 1e-12)); log: values <= 0 ignored,
// min = max(min, max * 1e-6), then min / 1.2, max * 1.2; no data: (0, 1),
// log (0.1, 10); then Axis min and max each override its side.
Range panel_y_range(const Panel& panel);
struct Tick { double value = 0.0; std::string label; };
// About `target` ticks inside r: linear 1-2-5 steps; log one per decade
// (labels "0.01" .. "1000", else "1e5"); Time (unix seconds) steps from
// 1 s to 1 year, labels by axis.time_format (strftime subset %Y %y %m %d %b
// %H %M %S %n %%, default "%Y-%m-%d%n%H:%M"); Category integers with
// categories[i]. Never more than 4 * target + 2 ticks.
std::vector<Tick> axis_ticks(const Axis& axis, Range r, int target = 5);
// Advance width in points of UTF-8 text in Helvetica (bold: Helvetica-Bold),
// standard 14 font metrics for 32..126; ± 584, ° 400, µ 556, σ 603, any
// other code point 556.
double text_width(std::string_view utf8, double size, bool bold = false);
struct Box { double x = 0, y = 0, w = 0, h = 0; };  // points, origin top left, y down
struct FigureSize { double graph_width = 432.0, graph_height = 324.0; };  // one grid cell
struct PanelLayout {
  std::string id;
  Box plot;
  Range x, y;
  bool log_y = false;
  std::vector<Tick> x_ticks, y_ticks;
  bool x_tick_labels = false;  // bottom panel only
};
struct GraphLayout {
  Box cell;
  std::string title;  // empty: none
  std::vector<PanelLayout> panels;  // top to bottom
};
struct SceneLayout { double width = 0, height = 0; std::vector<GraphLayout> graphs; };
SceneLayout layout_scene(const Scene& scene, const FigureSize& size = {});
double map_x(const PanelLayout& p, double x);  // points; linear
double map_y(const PanelLayout& p, double y);  // points; log when log_y (y > 0)
```

Geometry: graphs in `max(1, columns)` columns, row-major, cells of `FigureSize`, `graph_spacing` between cells. In a cell: title (bold, `fonts.title`) on top when set; panels share one left margin (widest y tick label + y title line + 8) and one right margin (12); bottom margin on the last panel only (tick line + x title line + 6); panel heights proportional to `max(0.05, height)` with `panel_spacing` between them.

- [ ] **Step 1: Write the failing tests** in `tests/processing/test_scene_layout.cpp`:
  - `SceneLayout.XRangeIsTheAxisLimitsWhenBothAreSet` (x.min 0, x.max 100 -> `Range{0, 100}` whatever the data).
  - `SceneLayout.XRangeIsTheDataWithoutPadding` (points x {1, 3} with x_err {0.5, 0.5} -> `{0.5, 3.5}`).
  - `SceneLayout.YRangeIsPaddedEightPercent` (y {10, 20}, no errors -> `{9.2, 20.8}`, `EXPECT_DOUBLE_EQ`).
  - `SceneLayout.ZeroWidthRangeIsPaddedAsTheView` (y {5, 5} -> `{4.75, 5.25}`).
  - `SceneLayout.AxisMinOverridesOneSide` (y.min 0 on y {10, 20} -> min 0, max 20.8).
  - `SceneLayout.LogRangeIgnoresNonPositiveValues` (log y {-1, 0, 1, 100} -> `{1 / 1.2, 120}`).
  - `SceneLayout.StepsGuidesAndSpansAreNotData` (a panel with only a StepLayer -> `{0, 1}`).
  - `SceneLayout.LinearTicksAreOneTwoFive` (range 0..10, target 5 -> values 0, 2, 4, 6, 8, 10, labels "0" .. "10"; range 0.0021..0.0049 -> labels "0.0025" "0.003" .. step 0.0005).
  - `SceneLayout.LogTicksPerDecade` (0.5..2000 -> 1, 10, 100, 1000).
  - `SceneLayout.TimeTicksUseTheFormat` (range 0..86400 with format "%H:%M" -> first label "00:00", every value a multiple of the step).
  - `SceneLayout.TicksAreBounded` (range 0..1e-300, 0..1e300, and NaN inputs -> at most 22 ticks, no infinite loop).
  - `SceneLayout.HelveticaWidths` (`text_width("0", 10)` == 5.56; `text_width("i", 10)` == 2.22; bold "m" at 10 == 8.89; "σ" at 10 == 6.03).
  - `SceneLayout.TwoColumnsMakeAGrid` (3 graphs, columns 2 -> width 2*432 + 5, height 2*324 + 5; graph 2 cell at x 0, y 329).
  - `SceneLayout.PanelsShareTheLeftMarginAndStackByHeight` (heights 1 and 3 -> plot heights in ratio 1:3 within 1e-9 after spacing; both plot.x equal; only the last has `x_tick_labels`).
- [ ] **Step 2: Run, expect compile failure.** `cmake --build --preset dev --target pychron_processing_tests` fails on the missing header.
- [ ] **Step 3: Implement `scene_layout.hpp/.cpp`.** The Helvetica and Helvetica-Bold width tables for 32..126 are the AFM widths of the standard 14 fonts, as a `constexpr std::array<std::uint16_t, 95>` each. Ticks: step = 1, 2 or 5 times a power of ten nearest to size / target, computed with `std::pow(10.0, k)` for integer `k`; the label has as many decimals as the step needs (fixed), scientific (`to_chars` scientific, shortest) when |value| >= 1e6 or step < 1e-5. Non-finite or empty ranges give no ticks.
- [ ] **Step 4: Run** `build/dev/tests/processing/pychron_processing_tests --gtest_filter='SceneLayout.*'`: all pass. `python3 tools/quality_check.py`: exit 0.
- [ ] **Step 5: Commit** `feat(processing): a Qt-free layout of a scene: ranges, ticks and panel geometry`.

### Task 2: Draw list

**Files:**
- Create: `libs/processing/include/pychron/processing/scene_draw.hpp`, `libs/processing/src/scene_draw.cpp`
- Test: `tests/processing/test_scene_draw.cpp`

**Interfaces:**
- Consumes: Task 1 (`layout_scene`, `map_x`, `map_y`, `text_width`, `FigureSize`, `Box`).
- Produces:

```cpp
struct Pt { double x = 0, y = 0; };
struct PathOp { enum class Kind { Move, Line, Curve, Close } kind = Kind::Move; Pt p[3]{}; };  // Curve: p[0], p[1] controls, p[2] end
enum class DrawRole { Frame, Grid, Axis, Data, Marker, ErrorBar, Step, Ellipse, Band, Guide, Span, Legend, TextBox };
struct DrawPath {
  std::vector<PathOp> ops;
  std::optional<Color> stroke, fill;
  double width = 1.0;
  LineDash dash = LineDash::Solid;
  int clip = -1;  // index into DrawList::clips; -1 none
  DrawRole role = DrawRole::Data;
};
enum class TextAnchor { Start, Middle, End };
enum class TextRole { Title, AxisTitle, Tick, Annotation, StepLabel, SpanLabel, Legend, Empty };
struct DrawText {
  Pt at;  // baseline anchor
  std::string text;  // one line, UTF-8
  double size = 9.0;
  bool bold = false;
  TextAnchor anchor = TextAnchor::Start;
  bool vertical = false;  // rotated -90 degrees about `at` (y titles)
  Color color;
  int clip = -1;
  TextRole role = TextRole::Annotation;
};
using DrawItem = std::variant<DrawPath, DrawText>;
struct DrawList {
  double width = 0, height = 0;
  Color background;
  std::string font_family;  // Scene::style.fonts.family; empty: Helvetica
  std::vector<Box> clips;
  std::vector<DrawItem> items;  // painting order
};
DrawList draw_scene(const Scene& scene, const FigureSize& size = {});
// A coordinate as both writers print it: fixed, at most 2 decimals, trailing
// zeros and a trailing point dropped, "-0" printed "0"; std::to_chars.
std::string format_coord(double v);
```

Painting order per panel, as `SceneView` paints: plot background, grid (when `style.grid`), then the layers in scene order (band, line, points with error bars, steps with labels, ellipses, guides, spans with labels; insets are not drawn, as `SceneView` does not draw them), corner text boxes (all `TextLayer` lines of one corner in one box, white at alpha 200, padding 4 x 2, at the corner ratios 0.01/0.99 by 0.02/0.98), legend on the first panel (one swatch and label per labelled, shown `PointLayer`; none when empty), axes (left and bottom lines, ticks 5 pt inward), tick labels, axis titles, graph title. Data items are clipped to the plot box. An empty scene gives one centred `TextRole::Empty` text "No data". Markers are paths of size `marker.size` points (circle: four Bézier arcs, kappa 0.5522847498; star: ten-point polygon from literal constants, no trig). Ellipses: the unit circle's four Bézier arcs through the affine map `(u, v) -> (x + s*sx*u, y + s*sy*(rho*u + sqrt(1 - rho^2)*v))`, rho clamped to ±0.999999: only `sqrt`, so the bytes do not depend on a libm. Excluded steps: `excluded_fill`, dashed outline; with `dim_others` a step that is neither excluded nor highlighted has its fill alpha divided by 3 (as `SceneView`). Excluded ellipses: dashed outline, colour (150, 150, 150), never filled. Dashes: Dash [6 3], Dot [1 2], DashDot [6 2 1 2].

- [ ] **Step 1: Write the failing tests**:
  - `SceneDraw.EmptySceneSaysNoData` (one `DrawText` with role Empty, text "No data").
  - `SceneDraw.OneMarkerAndTwoErrorBarsPerShownPoint` (3 points with x_err and y_err, one excluded with `show_excluded` false -> 2 Marker paths, 4 ErrorBar paths).
  - `SceneDraw.PointsMapIntoThePlotBox` (x.min 0, x.max 10, point at 5 -> marker centre x == plot.x + plot.w / 2 within 1e-9).
  - `SceneDraw.NonFiniteValuesAreSkipped` (points {1, NaN, inf}, line with a NaN -> 1 marker; the line breaks into two subpaths; no `format_coord` output of the list contains "nan" or "inf").
  - `SceneDraw.StepsAreBoxesWithLabels` (3 steps, labels {"A", "", "C"} -> 3 Step paths, 2 StepLabel texts; dim_others with step 1 highlighted -> step 0 fill alpha == original / 3).
  - `SceneDraw.EllipseIsFourCurves` (one ellipse -> one path of Move + 4 Curve + Close).
  - `SceneDraw.CornerTextIsOneBoxPerCorner` (two TextLayers TopLeft, one BottomRight -> 2 TextBox paths, 3 or more Annotation texts, in corner order).
  - `SceneDraw.LegendListsLabelledPointLayers` (two labelled, one unlabelled -> 2 Legend texts; `style.legend` false -> 0).
  - `SceneDraw.OnlyTheBottomPanelHasTickLabels`.
  - `SceneDraw.FormatCoord` (`1.005` -> "1" or "1.01" per to_chars rounding of 1.005, assert equals `std::to_chars` fixed 2 result stripped; `-0.001` -> "0"; `2.50` -> "2.5"; `100.0` -> "100").
- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Implement** `scene_draw.hpp/.cpp`.
- [ ] **Step 4: Run** `--gtest_filter='SceneDraw.*:SceneLayout.*'`: pass; quality_check exit 0.
- [ ] **Step 5: Commit** `feat(processing): a scene as a draw list of paths and texts`.

### Task 3: SceneView shares the ranges; parity test

**Files:**
- Modify: `apps/pychron-ui/src/scene_view.cpp` (`SceneView::reset_view`)
- Test: `tests/ui/test_scene_view.cpp`

**Interfaces:**
- Consumes: `graph_x_range`, `panel_y_range`, `draw_scene` (Tasks 1, 2).
- Produces: nothing new.

`reset_view` sets each rect's y to `pp::panel_y_range(panel)` and x to `pp::graph_x_range(graph)`; `RectInfo` keeps a pointer-free copy of the two ranges computed in `rebuild` (store `pp::Range x_range, y_range` in `RectInfo`, drop `x_min`..`y_max` and `log` when nothing else reads them).

- [ ] **Step 1: Write the failing test** `TestSceneView::placesWhatTheDrawListPlaces`: a scene of two panels (points with errors, a line, a band, two labelled step boxes, an ellipse, corner texts). After `set_scene`: each rect's axis ranges equal `graph_x_range` / `panel_y_range` (`QCOMPARE` on lower and upper); per panel, the number of `Marker` paths in `draw_scene` equals the hit points of the view (use `points_in(view.rect())` count), `Step` paths equal the QCPItemRects minus spans, and the Annotation texts of the draw list, in order, equal `view.texts(panel)`.
- [ ] **Step 2: Run** `cmake --build --preset dev-ui --target pychron_ui_tests` and the scene view test binary: the range comparison fails for the ellipse panel (polygon vs exact extent) or passes trivially; either way it must pass only after Step 3.
- [ ] **Step 3: Change `reset_view`** to the shared ranges.
- [ ] **Step 4: Run** the whole UI scene view, figure window and data browser suites (`--gtest_filter`-equivalent QtTest names: run the `pychron_ui_tests` binaries that include `test_scene_view.cpp` and `test_figure_window.cpp`): pass; quality_check exit 0.
- [ ] **Step 5: Commit** `refactor(ui): the figure view takes its axis ranges from the scene layout`.

### Task 4: SVG writer

**Files:**
- Create: `libs/processing/include/pychron/processing/scene_svg.hpp`, `libs/processing/src/scene_svg.cpp`
- Create: `tests/processing/fixtures/scene/figure.svg`
- Modify: `tests/processing/CMakeLists.txt` (define `PYCHRON_PROCESSING_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"`)
- Test: `tests/processing/test_scene_svg.cpp`, shared scene in `tests/processing/scene_fixture.hpp`

**Interfaces:**
- Consumes: `DrawList`, `draw_scene`, `format_coord` (Task 2).
- Produces: `std::string scene_svg(const Scene& scene, const FigureSize& size = {});` and `std::string draw_list_svg(const DrawList& list);`; `tests/processing/scene_fixture.hpp` with `inline Scene fixture_scene()` (two graphs side by side: an ideogram-like panel stack with band, line, points and corner text; a spectrum-like panel with steps, labels, guide and span; an isochron-like panel with ellipses; text with `±`, `σ`, `<&>` and `é`), used by Task 5.

Format: `<?xml version="1.0" encoding="UTF-8"?>\n`, `<svg xmlns="http://www.w3.org/2000/svg" width="Wpt" height="Hpt" viewBox="0 0 W H" font-family="<family or Helvetica>, Helvetica, Arial, sans-serif">`, background `rect`, `<defs>` of `clipPath id="c<i>"`, one element per item (`path d="M.. L.. C.. Z"` with `fill`/`stroke`/`stroke-width`/`stroke-dasharray`/`fill-opacity`/`stroke-opacity`/`clip-path`; `text` with `x`, `y`, `font-size`, `font-weight="bold"`, `text-anchor`, `transform="rotate(-90 x y)"`), one item per line, `\n` line ends, attributes in a fixed order. Text and attribute values escape `& < > "`.

- [ ] **Step 1: Write the failing tests**:
  - `SceneSvg.EmptySceneIsAValidDocument` (starts with the XML declaration, has one `<svg` and one `</svg>`, contains "No data").
  - `SceneSvg.SameSceneSameBytes` (two calls equal).
  - `SceneSvg.OneElementPerItem` (count of `<path` + `<text` == items of `draw_scene` for the fixture scene).
  - `SceneSvg.EscapesMarkup` (a title `a<b & "c"` appears as `a&lt;b &amp; &quot;c&quot;`; no raw `<b`).
  - `SceneSvg.NamesTheFontFamily` (default `font-family="Helvetica, Helvetica, Arial, sans-serif"` is not doubled: assert exact `font-family="Helvetica, Arial, sans-serif"` when family is empty, and `"Times, Helvetica, Arial, sans-serif"` when it is "Times").
  - `SceneSvg.MatchesTheCommittedFixture` (output for `fixture_scene()` equals `fixtures/scene/figure.svg` byte for byte; when env `PYCHRON_WRITE_FIXTURES=1` the test writes the file instead and fails with "fixture written"; message on mismatch names the first differing offset).
- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Implement**; generate the fixture with `PYCHRON_WRITE_FIXTURES=1`, read it (open in a browser via the built-in browser pane to see it is a sane figure), commit it.
- [ ] **Step 4: Run** `--gtest_filter='SceneSvg.*'` without the variable: pass; quality_check exit 0.
- [ ] **Step 5: Commit** `feat(processing): a scene written as SVG`.

### Task 5: PDF writer

**Files:**
- Create: `libs/processing/include/pychron/processing/scene_pdf.hpp`, `libs/processing/src/scene_pdf.cpp`
- Create: `tests/processing/fixtures/scene/figure.pdf`
- Test: `tests/processing/test_scene_pdf.cpp`

**Interfaces:**
- Consumes: Task 2, `tests/processing/scene_fixture.hpp` (Task 4).
- Produces:

```cpp
struct PdfInfo {
  std::int64_t created_unix = 0;  // CreationDate, UTC seconds
  std::string title;              // /Title; may be empty
};
std::string scene_pdf(const Scene& scene, const PdfInfo& info, const FigureSize& size = {});
std::string draw_list_pdf(const DrawList& list, const PdfInfo& info);
// Writes scene_pdf to `path` and marks it a user file; Io on failure.
Result<void> save_scene_pdf(const Scene& scene, const PdfInfo& info, const std::filesystem::path& path,
                            const FigureSize& size = {});
```

Layout of the file: header `%PDF-1.4\n%\xE2\xE3\xCF\xD3\n`; objects 1 Catalog, 2 Pages, 3 Page (`/MediaBox [0 0 W H]`, resources: `/Font << /F1 5 0 R /F2 6 0 R /F3 7 0 R >>`, `/ExtGState` with one `/A<n>` per distinct alpha used, `n` = alpha 0..254, sorted), 4 content stream (uncompressed, `/Length` exact), 5 Helvetica, 6 Helvetica-Bold (both `/Encoding /WinAnsiEncoding`), 7 Symbol, 8 Info (`/Producer (pychron-cpp)`, `/CreationDate (D:YYYYMMDDHHmmSSZ)`, `/Title` when set), then the ExtGState objects; xref with one 20-byte entry per object, trailer `/Size /Root 1 0 R /Info 8 0 R`, no `/ID`; `startxref`, `%%EOF\n`. y is flipped per coordinate (`H - y`). Clip: `q x y w h re W n ... Q`. Text: `BT /F1 size Tf r g b rg 1 0 0 1 x y Tm (...) Tj ET`; anchor Middle/End shift x by `text_width` (Task 1); vertical uses `0 1 -1 0 x y Tm`. Text runs: a code point in WinAnsi goes to F1/F2 as its byte, lowercase Greek α..ω to F3 as the Symbol letter (`σ` -> `s`, `μ` -> `m`, ...), anything else `?`; `(`, `)`, `\` escaped.

- [ ] **Step 1: Write the failing tests**, with a reader of about 50 lines in the test file (`struct PdfCheck { bool ok; std::string why; }; PdfCheck check_pdf(std::string_view)`: finds `startxref`, reads the offset, parses `xref` (first and count, 20-byte entries), checks each in-use offset starts `"<n> 0 obj"`, `/Size` equals count, every stream's `/Length` equals the bytes between `stream\n` and `\nendstream`):
  - `ScenePdf.EmptySceneIsAValidDocument` (`check_pdf(scene_pdf(Scene{}, {}))` ok).
  - `ScenePdf.XrefOffsetsAreRight` (fixture scene: `check_pdf` ok, `why` printed on failure).
  - `ScenePdf.SameSceneSameBytes`; and no `/ID` in the trailer.
  - `ScenePdf.CreationDateIsTheGivenTime` (`created_unix` 1760000000 -> contains `/CreationDate (D:20251009085320Z)`).
  - `ScenePdf.EncodesTextForTheStandardFonts` (title `(a\b) ± σ é 中` -> the content stream holds `(\(a\\b\) \261 ` under `/F1` or `/F2`, `(s)` under `/F3`, `\351` for é, `?` for 中).
  - `ScenePdf.AlphaUsesOneGraphicsStatePerValue` (a band fill alpha 60 and a step fill alpha 120 -> `/A60` and `/A120` in the resources, each once).
  - `ScenePdf.SameTextsAsTheSvg` (number of `Tj` runs that start a text item equals `<text` count of `scene_svg` for the fixture: count `BT` operators).
  - `ScenePdf.MatchesTheCommittedFixture` (as in Task 4, `figure.pdf`, `created_unix` 1760000000, title "fixture").
  - `ScenePdf.SaveWritesTheBytes` (file in the test scratch directory equals `scene_pdf`).
- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Implement**; write the fixture; open it in Preview via `open` to check it shows the figure; commit it.
- [ ] **Step 4: Run** `--gtest_filter='ScenePdf.*:SceneSvg.*'`: pass; quality_check exit 0.
- [ ] **Step 5: Commit** `feat(processing): a scene written as a one-page PDF`.

### Task 6: The figures of a sample

**Files:**
- Create: `libs/publish/include/pychron/publish/figures.hpp`, `libs/publish/src/figures.cpp`
- Test: `tests/publish/test_figures.cpp` (uses `tests/processing/fixtures.hpp`: `make_unknown`, `make_step`)

**Interfaces:**
- Consumes: `build_ideogram`, `build_spectrum`, `build_isochron` and their schemas (`arar_figures.hpp`), `scene_svg`, `scene_pdf` (Tasks 4, 5), `ReleaseOptions`, `file_safe` (phase 1).
- Produces:

```cpp
struct FigureFile {
  std::string path;   // relative to the release directory
  std::string bytes;
};
// Figure options from the release: the schema's defaults, with
// error_bar_nsigma = sigma, and for the spectrum plateau.method, plateau.nsteps,
// plateau.gas_fraction from the release options.
processing::Options figure_options(const processing::SchemaPtr& schema, const ReleaseOptions& options);
// The figures of one sample (its items of the release dataset): an ideogram;
// a spectrum per step-heated aliquot (items with increment >= 0, by
// identifier and aliquot), file spectrum.* when there is one, else
// spectrum-<identifier>-<aliquot two digits>.* made file-safe; an inverse
// isochron when at least three items are included. SVG and PDF of each, PDF
// CreationDate = created_unix. A figure that cannot be built is left out and
// a warning "<sample>: no <kind>: <why>" is added.
std::vector<FigureFile> sample_figures(const processing::Dataset& sample, const std::string& sample_dir,
                                       const ReleaseOptions& options, std::int64_t created_unix,
                                       std::vector<std::string>& warnings);
```

Paths: `figures/<sample_dir>/<kind>.svg` then `.pdf`, kinds in the order ideogram, spectrum(s), isochron.

- [ ] **Step 1: Write the failing tests** (datasets reduced as `tests/processing/test_arar_figures.cpp` does):
  - `ReleaseFigures.FigureOptionsFollowTheRelease` (sigma 1, plateau mahon, nsteps 4, gas 60 -> the spectrum options' values equal these).
  - `ReleaseFigures.OneAliquotOfStepsGivesIdeogramSpectrumAndIsochron` (five steps of one aliquot -> paths `figures/S/ideogram.svg`, `.pdf`, `spectrum.svg`, `.pdf`, `isochron.svg`, `.pdf` in that order; every svg starts with `<?xml`, every pdf with `%PDF-1.4`).
  - `ReleaseFigures.TwoAliquotsNameTheirSpectra` (aliquots 1 and 2 of `66012` -> `spectrum-66012-01.svg` and `spectrum-66012-02.svg`).
  - `ReleaseFigures.ASampleWithoutEnoughPointsSkipsTheIsochronWithAWarning` (two unknowns -> no isochron file, no spectrum, ideogram present; no warning for the missing spectrum; a warning only when a builder returned an error).
  - `ReleaseFigures.SameInputsSameBytes`.
- [ ] **Step 2: Run, expect compile failure** (`cmake --build --preset dev --target pychron_publish_tests`).
- [ ] **Step 3: Implement** `figures.hpp/.cpp`.
- [ ] **Step 4: Run** `build/dev/tests/publish/pychron_publish_tests --gtest_filter='ReleaseFigures.*'`: pass; quality_check exit 0.
- [ ] **Step 5: Commit** `feat(publish): the figures of a sample in a release`.

### Task 7: Figures in the build, manifest and records

**Files:**
- Modify: `libs/publish/src/build.cpp`, `libs/publish/include/pychron/publish/build.hpp` (comment of `with_figures`; `IProgress` stage list gains `figures`), `libs/publish/include/pychron/publish/manifest.hpp`, `libs/publish/src/manifest.cpp`, `libs/publish/src/records.cpp`
- Test: `tests/publish/test_build.cpp`, `tests/publish/test_records.cpp`

**Interfaces:**
- Consumes: `sample_figures` (Task 6).
- Produces: `bool has_figures(const Manifest& manifest);` (true when any `files[].path` starts with `figures/`), used by Task 8.

In `build_release`, after the per-sample reports and before Parquet, when `options.with_figures`: stage `figures`; per kept sample (the same loop and `dataset_of_sample` as the reports), `sample_figures(sub, sample_dir_name(...), *opts, release.created.micros / 1000000, result.warnings)`, each file through `writer.write`. The phase 1 warning about `--with-figures` is removed. Records: a sample record's `formats` gains `"SVG"`, `"PDF"` when it has figure files; the release record's formats gain them when `has_figures(m)`; the enclosure links already cover `figures/<sample>/`.

- [ ] **Step 1: Write the failing tests**:
  - `BuildTest.WithFiguresWritesSvgAndPdfPerSampleInTheManifest` (fixture release, `with_figures` -> for each sample dir in `tables/`, `figures/<dir>/ideogram.svg` and `.pdf` exist, are in `manifest.files` with matching sha256 and media types `image/svg+xml`, `application/pdf`; `verify_files` ok; no warning mentions "phase 2").
  - `BuildTest.FiguresAreByteIdenticalAcrossTwoBuilds` (two builds with figures into two directories: every `figures/` entry has equal sha256).
  - `BuildTest.WithoutFiguresThereIsNoFiguresDirectory` (default options -> no `figures/` path and `has_figures` false).
  - `BuildTest.ProgressReportsTheFiguresStage` (a recording `IProgress` sees `figures` between `files` and `parquet` only with figures).
  - `Records.SampleWithFiguresListsSvgAndPdfFormats` in `test_records.cpp`, using a manifest with a `figures/<dir>/ideogram.svg` file.
- [ ] **Step 2: Run, expect failure** of these cases.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `--gtest_filter='Engines/BuildTest.*:Records*.*:ReleaseFigures.*'`: pass (PostgreSQL cases too when `PYCHRON_TEST_PG_URL` is set); quality_check exit 0.
- [ ] **Step 5: Commit** `feat(publish): a release build writes its figures with --with-figures`.

### Task 8: `elctl publish build --with-figures` and verify

**Files:**
- Modify: `apps/elctl/src/publish.cpp` (help text, `verify` sets `options.with_figures = pb::has_figures(*manifest)`)
- Test: `tests/publish/test_elctl_publish.cpp`

**Interfaces:**
- Consumes: `has_figures` (Task 7).

- [ ] **Step 1: Write the failing tests**:
  - `ElctlPublishTest.BuildWithFiguresAndVerifyAgainstTheStore` (`publish build --with-figures` exits 0 and prints no "ignored"; `publish verify <dir> --db <url>` exits 0 and says "the rebuild agrees").
  - `ElctlPublishTest.VerifyNamesAChangedFigure` (append a byte to `figures/<dir>/ideogram.svg` -> `verify <dir>` exits 1, stderr contains `changed figures/` and the file name).
- [ ] **Step 2: Run, expect failure** (the rebuild without figures reports the figure files missing).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `--gtest_filter='Engines/ElctlPublishTest.*'`: pass; quality_check exit 0.
- [ ] **Step 5: Commit** `feat(elctl): publish build --with-figures, and verify rebuilds the figures`.

### Task 9: Docs and spec

**Files:**
- Modify: `docs/release_format.md` (layout lists `figures/`; a "Figures" section: kinds, names, SVG and PDF properties, determinism, which options), `docs/user/08-elctl-reference.md` (`--with-figures`), the spec.

Spec amendments (compressed, as the spec is written):
- 5.6: signatures as built (`scene_svg(const Scene&, const FigureSize&)`, `scene_pdf(const Scene&, const PdfInfo&, const FigureSize&)` returning bytes, `save_scene_pdf(scene, info, path, size)`); `scene_layout.hpp` holds what `SceneView` computed itself (axis ranges) plus what QCustomPlot did for it (ticks, margins), the writers' only; `scene_draw.hpp` the draw list both writers serialize; insets not drawn (`SceneView` draws none); spectrum per aliquot named `spectrum-<identifier>-<aliquot two digits>`; isochron only with three or more included analyses; figure options are schema defaults plus `sigma` and plateau options of `release.options` (no per-release figure options yet); time series not a release figure; parity test is on ranges, counts and annotation texts.
- 13: phase 2 done, with date and plan path.

- [ ] **Step 1: Write the docs and amendments.**
- [ ] **Step 2: Check** every path and name in them against the code (`grep` the identifiers).
- [ ] **Step 3: Commit** `docs(publish): figures in the release format, elctl and the spec`.

### Landing

- [ ] Rebase on `origin/develop`; `cmake --build --preset dev-ui` and `ctest --preset dev-ui` (Task 3 touches `apps/pychron-ui`; it covers `dev`); PostgreSQL suites rerun at `-j2` if the server refuses connections under full parallelism.
- [ ] `python3 tools/quality_check.py` exit 0; `python3 tools/compiler_sweep.py` clean on the changed files.
- [ ] Sonnet review of the whole branch against this plan and the spec; fix rounds as their own commits.
- [ ] Merge into `develop`, push `feat/publish-figures:develop`; remove the worktree's branch after landing.

## Open questions

None.
