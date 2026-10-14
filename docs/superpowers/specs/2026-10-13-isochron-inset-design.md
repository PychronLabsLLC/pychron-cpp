# Inverse isochron: an inset on the intercept

Status: agreed in outline 2026-10-13 (owner: wanted, legacy's arrangement;
section 8). Drafted by an unattended run from task T070. Nothing built.

## 1. Problem

Inverse isochron of radiogenic samples: points crowd near the x axis (high
39Ar/40Ar, low 36Ar/40Ar), and the figure's y range is set by the trapped
intercept far up the axis. What the eye wants to judge, how the points sit
about the line, is a few pixels tall. Zooming shows it but loses the
intercept and is not in a saved figure.

## 2. Legacy (examined 2026-10-13, `~/Programming/pychron`)

`pipeline/plot/plotter/isochron.py:438-507`, `_add_inset`: two overlays in
a corner of the isochron's plot.

- Points inset: the regression's points, small markers, optional error
  ellipse (`inset_show_error_ellipse`), in the group's colour.
- Line inset: the regression line over the inset's x range (20 points).
- Options: `inset_location`, `inset_width`, `inset_height`,
  `inset_marker_size`, `inset_label_font`, `inset_show_axes_titles`,
  `inset_x_bounds`, `inset_y_bounds`.
- Default bounds when unset: x from `-0.1 * (x_intercept - min_x)` to
  `1.1 * x_intercept`; y from 0 to `max(1.1 * nominal_intercept, 1.1 *
  y_intercept)`. So legacy's default inset is the whole line from one axis
  to the other: the overview. The main plot was then left to the points.
- With several groups: one inset a group, axes of all but the first hidden,
  drawn over each other.

## 3. What exists here

- `build_isochron_scene` (`libs/processing/src/isochron.cpp`): one `Graph`
  a group set, one `Panel`, layers (points, ellipses, line, envelope,
  nominal line, text). Axis limits `x.min`, `x.max`, `y.min`, `y.max`;
  default from 0 to past the largest point and past the intercept (`:244-247`).
- `Scene` has no notion of an inset: `Panel` is a row of a graph
  (`scene.hpp:147`); `Layer` variant has no nested plot.
- `SceneView` draws each panel as a `QCPAxisRect` in a grid layout.
  QCustomPlot can place an axis rect inside another's area
  (`QCPLayoutInset` of the outer rect's `insetLayout()`), with own axes.

## 4. Goal

An inverse isochron can carry a small second view of the same data and line
in a corner, with limits of its own, drawn and saved with the figure.

## 5. Design

### 5.1 Which view is the inset

Legacy's arrangement (owner, 2026-10-13). With the inset on: main plot is
the close view, the box round the points padded 10 %, so the scatter about
the line shows; inset is the overview, the whole line from axis to axis with
both intercepts. Typed main limits (`x.min` ...) win as always. With the
inset off the figure is as today (whole line).

`inset.view = points` gives the other way round (main plot whole line, inset
on the points) for whoever wants it.

### 5.2 Scene

```cpp
struct Inset {                 // a small view inside a panel
  Corner corner = Corner::TopRight;
  double width = 0.35, height = 0.35;   // fraction of the panel
  Axis x, y;                   // its own limits and titles
  std::vector<Layer> layers;   // drawn in it; same layer types
};
struct Panel { ...; std::optional<Inset> inset; };
```

Additive: a scene without one draws as today. Generic, not isochron's own:
any figure may fill it later.

### 5.3 Options (`figure.inverse_isochron`, section "Inset")

| Key | Default |
|---|---|
| `inset.show` | off |
| `inset.location` | `top_right` (corners) |
| `inset.width`, `inset.height` | 35, 35 (% of panel; 15 to 60) |
| `inset.view` | `whole` (axis to axis, legacy; main plot then fits the points) or `points` (box round the points; main plot as today) |
| `inset.x_min`, `inset.x_max`, `inset.y_min`, `inset.y_max` | unset: from `inset.view` |
| `inset.show_ellipses` | follows `ellipse` of the figure |
| `inset.show_nominal_intercept` | off |
| `inset.show_titles` | off (tick labels only, at `font.tick` less 1) |

Absent from a preset saved before: defaults, figure as before. Same keys
under `isochron.` in the spectrum + isochron figure.

### 5.4 Contents

Same points (excluded ones as the figure's `excluded_style` says), same
fitted line clipped to inset's x range, envelope if the figure shows it,
ellipses per 5.3. No statistics text, no legend. Several groups in one
graph: all in one inset, group colours (not legacy's stacked insets).
Fewer than 3 points or no fit: no inset, no warning.

### 5.5 Drawing (`SceneView`)

Inset as an axis rect in the panel's inset layout, opaque background
(`plot_background`), thin border. Main plot's text layer in the same corner
moves to the next free corner (statistics default `top_left`, inset
`top_right`: no clash by default). Clicks in the inset select and omit
points as in the main plot; zoom and pan act on the main plot only. Saved
PNG and PDF carry it (same plot).

## 6. Rules

- Inset never changes the regression or any number: same dataset, a second
  view.
- Scene stays the single description: view computes nothing.
- A scene with no inset is byte for byte what it is today (tests that
  compare scenes unchanged).

## 7. Out of scope

- Insets on other figures (ideogram, spectrum): model allows, nobody asked.
- Dragging or resizing the inset with the mouse.
- More than one inset a panel.
- Legacy's `inset_marker_size`, `inset_label_font` as separate settings:
  derived from the figure's.

## 8. Owner, 2026-10-13

Decided: wanted; legacy's arrangement by default (5.1).

Kept as drafted, not asked (say if wrong): several groups in one graph
share one inset, in their colours (legacy stacked an inset a group).

## 9. Tests

- `tests/processing/test_arar_figures.cpp`: off by default, scene equal to
  today's; on: inset present, main plot on the points; limits by `whole`
  and by `points` against hand numbers; typed limits win; line clipped to range; fewer than 3
  points none; an older preset loads.
- `tests/ui/test_scene_view.cpp`: an inset draws a second axis rect inside
  the panel's, in its corner, at its size; a click on an inset point emits
  `point_clicked`; wheel over the inset zooms the main plot; saved PNG has
  it (pixel at inset's corner is its background).
