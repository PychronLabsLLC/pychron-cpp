# Isochron Inset Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An inverse isochron can carry a small second view of the same points and line in a corner: by default the whole line from axis to axis, while the main plot closes in on the points, as legacy pychron drew it.

**Architecture:** The scene gains an optional `Inset` on a `Panel` (corner, size, its own axes, its own layers). The isochron builder fills it from `inset.*` options, computing the two views' limits; nothing in the regression changes. `SceneView` draws an inset as a second `QCPAxisRect` in the panel's inset layout, takes clicks on its points, and passes the wheel through to the main plot.

**Tech Stack:** C++20, Qt 6 Widgets, QCustomPlot 2.1.1, GoogleTest (`libs/processing`), QtTest (`tests/ui`).

**Spec:** `docs/superpowers/specs/2026-10-13-isochron-inset-design.md` (agreed in outline 2026-10-13, legacy's arrangement). Section numbers below are its.

## What the code says

Read from `develop` `358eb184`.

- `Panel` (`libs/processing/include/pychron/processing/scene.hpp:147-153`) is `id`, `quantity`, `height`, `y`, `layers`. `Layer` is a variant of eight layer types (`:125-126`). `Corner` and `parse_corner` exist (`:76-78`). Nothing compares whole scenes with `==`; tests assert on the scene's parts.
- `build_isochron_scene` (`libs/processing/src/isochron.cpp:84-252`): one graph a graph key, one panel `id = "isochron"`. Per group it pushes an `EllipseLayer` (unless `ellipse = none`), a `PointLayer`, and, with 3 or more fitted points, a `BandLayer` (when `show_envelope`) and a `LineLayer` sampled 50 times from x = 0 to `x_end = max(xmax, x_intercept) * 1.1`. Then the nominal intercept (a `PointLayer` at x = 0) and one `TextLayer` in `statistics_location` (default `top_left`, `:80`). `xmax` and `ymax` grow over every drawn point plus its ellipse, the line and the nominal point; there is no minimum kept. Defaults: x 0 to `xmax * 1.1`, y 0 to `ymax * 1.15` (`:244-247`).
- Point layers carry the group's label (`:146`), so a figure with labelled groups shows a legend, whose corner is `legend_location`, default `top_right` (`figure_common.cpp`): the same as the inset's default corner. Spec 5.5 speaks of the statistics text only. Task 3 moves the legend too; see "Questions" at the end.
- The combined spectrum + isochron figure takes the isochron's own fields under the prefix `isochron.` (`spectrum_isochron.cpp:23`, `:61`) and builds the isochron from those options (`:110`): new `inset.*` fields reach it as `isochron.inset.*` with no change there.
- `SceneView::rebuild` (`apps/pychron-ui/src/scene_view.cpp:181-458`) draws every layer of a panel inline in one loop (`:276-446`) into that panel's `QCPAxisRect`, recording hit points, boxes, texts and spans in a `RectInfo`, and keeps one `RectInfo` a panel in `rects_`. `panel_count()`, `point_position(uuid, panel)`, `reset_view()`, `set_scene`'s kept ranges and `hit()` all walk `rects_` by panel index; an inset must not be one of them. The legend already sits in the rect's `insetLayout()` (`:268-270`).
- QCustomPlot 2.1.1: `QCPLayoutInset::addElement(element, QRectF)` places an element by fractions of the rect. `QCustomPlot::wheelEvent` offers the wheel to the topmost layerable at the pointer and stops when it is accepted; `QCPAxisRect::wheelEvent` accepts it even with `setRangeZoom(0)`. So a wheel over an inset never reaches the main plot unless `SceneView` hands it on.
- `tests/ui/test_scene_view.cpp` drives `SceneView` with hand-made scenes; `tests/processing/test_arar_figures.cpp` has the `Isochron` and `SpectrumIsochron` suites.
- Branch `feat/use-these-limits` (T071, not on `develop`) adds `SceneView::limits_at`, which walks `rects_`: keeping insets out of `rects_` keeps it right. Whichever lands second rebases.

## Global Constraints

- An inset changes no number: the same fit, the same points.
- With `inset.show` off (the default) the isochron's scene is what it is today, layer for layer and limit for limit. Task 3 has a test that says so.
- The view computes nothing: limits, corners and layers of the inset come in the scene.
- An older preset loads and draws as before.
- Qt code: a parent owns its child. `const` by default.
- Before each commit that changes C++: `python3 tools/quality_check.py --build-dir build/dev-ui` (exit 0), the tests the task names. UI tasks build with the `dev-ui` preset. Before the branch lands: full `cmake --build --preset dev-ui`, `ctest --preset dev-ui`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui`.
- Commits: Conventional Commits, scope `processing` or `ui`.

## File Structure

| File | Responsibility |
|---|---|
| `libs/processing/include/pychron/processing/scene.hpp` | `Inset`; `Panel::inset`. |
| `libs/processing/src/isochron.cpp` | `inset.*` fields; the two views' limits; the inset's layers; moving a text or legend out of the inset's corner. |
| `apps/pychron-ui/src/scene_view.hpp`, `.cpp` | Layer drawing as a function of its own; insets drawn, hit, and passing the wheel on. |
| `tests/processing/test_arar_figures.cpp` | Options, limits, layers, the scene unchanged when off, the combined figure. |
| `tests/ui/test_scene_view.cpp` | Placement, clicks, wheel, saved image. |
| `docs/user/06-data-analysis.md` | The isochron's Inset options. |

---

### Task 1: `Inset` in the scene

**Files:** `scene.hpp`, `tests/processing/test_arar_figures.cpp`.

- [x] Failing test `Isochron.APanelHasNoInsetUnlessAsked`: the default isochron scene's panel has `!inset`.
- [x] Add, as spec 5.2 gives it, beside `Panel`:

```cpp
// A small second view inside a panel, in one of its corners, with limits of
// its own; drawn over the panel. Its layers are of the panel's kinds.
struct Inset {
  Corner corner = Corner::TopRight;
  double width = 0.35, height = 0.35;  // fractions of the panel
  Axis x, y;
  std::vector<Layer> layers;
};
```

  and `std::optional<Inset> inset;` last in `Panel`. `Scene::point_count()` counts the panel's layers only (an inset repeats the same analyses): say so in its comment.
- [x] `build/dev-ui/tests/processing/pychron_processing_tests --gtest_filter='Isochron.*'`; commit `feat(processing): a scene's panel can hold an inset`.

### Task 2: The options

**Files:** `isochron.cpp`, `test_arar_figures.cpp`.

- [x] Failing tests:
  - `Isochron.InsetOptionsAndTheirDefaults`: the schema has, in section "Inset", `inset.show` (false), `inset.location` (corner choices, `top_right`), `inset.width` and `inset.height` (35, 15 to 60), `inset.view` (`whole`, `points`; `whole`), `inset.x_min`, `inset.x_max`, `inset.y_min`, `inset.y_max` (optional), `inset.show_ellipses` (true: drawn when the figure draws them), `inset.show_nominal_intercept` (false), `inset.show_titles` (false).
  - `Isochron.APresetWithoutInsetKeysLoads`: `options_from_toml` of a preset text with none of them gives the defaults and no warning.
  - `SpectrumIsochron.TheIsochronsInsetOptionsArePrefixed`: the combined schema has `isochron.inset.show` and the rest.
- [x] Add the fields after `y.max` (`isochron.cpp:44`). Use the corner list the common fields use (`kCorners`, `figure_common`). No schema version bump: fields only add.
- [x] Commit `feat(processing): the inverse isochron's inset options`.

  Built: `inset.width` and `inset.height` are whole percents of the plot (integer fields); every `inset.*` field but `inset.show` is enabled only when it is set (`isochron.inset.show` in the combined figure). The corner list is `figure_common`'s, now `corner_choices()`.

### Task 3: The isochron fills the inset

**Files:** `isochron.cpp`, `test_arar_figures.cpp`.

The builder keeps, besides `xmax`/`ymax`, the box of the drawn points (min and max of x and y over every point drawn, each widened by its ellipse's `scale * sigma`, excluded points included when they are drawn). Two views follow (spec 5.1):

- *whole*: today's limits (x 0 to `xmax * 1.1`, y 0 to `ymax * 1.15`, with the line and the nominal point in `xmax`/`ymax`);
- *points*: the box padded 10 % of its size on each side (a box of zero width or height padded by 10 % of the value, or 1e-6 when that is 0).

With `inset.show` and `inset.view = whole`: the main panel takes *points* and the inset *whole*; with `inset.view = points`: the main panel *whole* and the inset *points*. Typed `x.min`... win on the main panel, typed `inset.x_min`... on the inset, each limit alone.

- [ ] Failing tests (a hand-made dataset of 5 points on a known line, as `Isochron.EllipsesFitAndResults` builds one):
  - `Isochron.OffTheSceneIsAsBefore`: with every `inset.*` at its default, the scene's limits, layer kinds and order, and each layer's data equal those of the same options with the inset keys absent; compare against numbers written down from today's builder before the code changes (run the test on `develop` first and keep its output).
  - `Isochron.TheInsetShowsTheWholeLineAndTheMainPlotThePoints`: limits of both against hand numbers.
  - `Isochron.TheOtherWayRound`: `inset.view = points`.
  - `Isochron.TypedLimitsWinEachInItsOwnView`.
  - `Isochron.TheInsetsLayers`: per group the ellipses (only when the figure draws ellipses and `inset.show_ellipses`), the points (marker 0.6 of `marker_size`, at least 2), the band when `show_envelope`, the line re-sampled (20 points) over the inset's x range; the nominal point only with `inset.show_nominal_intercept`; no text layer; axes titles only with `inset.show_titles`. Several groups: all in the one inset, each in its colour.
  - `Isochron.NoFitNoInset`: fewer than 3 fitted points in every group, or no fit, gives no inset and no warning; the main panel then keeps *whole*.
  - `Isochron.TheTextAndTheLegendLeaveTheInsetsCorner`: statistics or legend in the inset's corner move to the next free corner in the order top left, top right, bottom right, bottom left; with nothing free they stay.
  - `SpectrumIsochron.HasTheInsetWhenAskedFor`: `isochron.inset.show = true` gives the isochron graph's panel an inset.
- [ ] Implement. The legend's corner is the scene's (`scene.style.legend_corner`): the builder sets it for this scene only when it moves. Keep the 50-sample main line as it is.
- [ ] Commit `feat(processing): the inverse isochron draws an inset of the whole line or of the points`.

### Task 4: `SceneView` draws layers through one function

**Files:** `scene_view.hpp`, `scene_view.cpp`.

No change of behaviour; it makes Task 5 small.

- [ ] Move the body of the layer loop (`scene_view.cpp:276-446`) into `void SceneView::draw_layers(const pp::Scene& s, const std::vector<pp::Layer>& layers, QCPAxisRect* rect, RectInfo& info, QCPLegend* legend)`; `rebuild` calls it for each panel.
- [ ] `ctest --preset dev-ui -R '^ui\.test_(scene_view|data_windows|flux_window)$'` unchanged; commit `refactor(ui): SceneView draws a panel's layers in one function`.

### Task 5: `SceneView` draws the inset

**Files:** `scene_view.hpp`, `scene_view.cpp`, `tests/ui/test_scene_view.cpp`.

- [ ] Failing tests with a hand-made scene of one panel with an inset (`TopRight`, 0.35 by 0.35):
  - `an_inset_is_a_second_plot_in_its_corner`: the plot has two axis rects; `panel_count()` is 1; the inset's rect lies inside the panel's, against its top and right edges (within a few pixels of margin), and is 35 % of its width and height (within 2 px); its ranges are the inset's limits.
  - `a_click_on_an_inset_point_selects_it`: `point_clicked` with that point's uuid, also where a main-plot point lies under the inset.
  - `the_wheel_over_the_inset_zooms_the_main_plot`: a wheel event at the inset's centre changes the main rect's ranges and leaves the inset's alone.
  - `a_saved_image_has_the_inset`: `save_png` to a temporary file; the pixel just inside the inset's corner is the inset's background, opaque.
  - `a_rebuild_keeps_no_old_inset`: setting a scene without an inset after one with leaves one axis rect.
- [ ] Implement: for a panel with an inset, a `QCPAxisRect` added to the panel rect's `insetLayout()` with `addElement(rect, QRectF)` from the corner and size, `setRangeDrag(0)` and `setRangeZoom(0)`, background `plot_background`, all four axes shown as a thin box with ticks on bottom and left only, tick labels at `fonts.tick - 1`, titles when the scene gives them, ranges from the inset's axes. Layers through `draw_layers` into a `RectInfo` kept in a separate `insets_` vector (not `rects_`). `hit()` looks in `insets_` first: an inset is drawn over its panel. In `eventFilter`, `QEvent::Wheel` over an inset: zoom the panel's rect about the pointer as `QCPAxisRect::wheelEvent` would (its `rangeZoomFactor` to the power of the steps, `scaleRange` about `pixelToCoord` of the pointer, on both axes) and return true. Double-click and reset act on the main plot as today.
- [ ] `ctest --preset dev-ui -R '^ui\.test_scene_view$'`; commit `feat(ui): a figure's panel draws its inset`.

### Task 6: Documents

- [ ] `docs/user/06-data-analysis.md`, "Inverse isochron" (`:477`): the Inset options, what each view shows, that the inset is drawn in saved images and that the wheel over it zooms the main plot.
- [ ] Commit `docs: the inverse isochron's inset`.

## Before landing

Full `cmake --build --preset dev-ui`, `ctest --preset dev-ui`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui`, `python3 tools/quality_check.py --build-dir build/dev-ui`. Look at the figure in the running app with a real step-heating isochron (both views, a group legend, a saved PDF).

## Questions for the owner (none blocks Tasks 1, 2, 4)

1. The legend's default corner is the inset's (`top_right`). Draft: the legend moves to the next free corner when the inset is shown there (Task 3). Or should the inset's default corner be another one?
2. Marker size in the inset, 0.6 of the figure's (at least 2): legacy had its own setting (`inset_marker_size`), which spec section 7 leaves out. Enough?
3. The main plot on the points pads the box by 10 %. Legacy left the main plot to its own autoscale. Right amount?
