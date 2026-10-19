#pragma once

// The layout of a scene (data publishing design, section 5.6): what a
// renderer needs to place a scene on a page without Qt. The axis ranges are
// the ones the figure view (apps/pychron-ui SceneView) takes, so both show a
// scene over the same limits; ticks, text widths and margins are what
// QCustomPlot does for the view and are used by the file writers
// (scene_svg.hpp, scene_pdf.hpp) alone. Lengths are in points, the origin is
// the top left of the page and y runs down.

#include <string>
#include <string_view>
#include <vector>

#include "pychron/processing/scene.hpp"

namespace pychron::processing {

struct Range {
  double min = 0.0, max = 1.0;
  friend bool operator==(const Range&, const Range&) = default;
};

// The x range of a graph: the axis's min and max when both are set, else the
// extent of its panels' data (the x +- x_err of the points shown, lines,
// bands, ellipses), with no padding; (0, 1) when there is none.
Range graph_x_range(const Graph& graph);

// The y range of a panel: the extent of its data (points y +- y_err, lines,
// band low and high, ellipses; steps, guides, spans and texts are not data).
// Linear: padded by 8% of the size, or max(|max| * 0.05, 1e-12) when the size
// is 0. Log: values <= 0 ignored, min = max(min, max * 1e-6), then min / 1.2
// and max * 1.2. No data: (0, 1), log (0.1, 10). The axis's min and max then
// each override their side.
Range panel_y_range(const Panel& panel);

struct Tick {
  double value = 0.0;
  std::string label;  // may hold '\n' (a time format's %n)
};

// About `target` ticks inside r, never more than 4 * target + 2: linear 1, 2
// or 5 times a power of ten; log one per decade ("0.001" .. "10000", else
// "1e5"), every n-th when there are many; Time (unix seconds, UTC) a step
// from one second to a year, labelled by axis.time_format (%Y %y %m %d %b %H
// %M %S %n %%; empty: "%Y-%m-%d%n%H:%M"); Category the integers, labelled
// categories[i]. No ticks for a range that is not finite or not increasing.
std::vector<Tick> axis_ticks(const Axis& axis, Range r, int target = 5);

// The advance width in points of UTF-8 text in Helvetica (bold:
// Helvetica-Bold), from the standard 14 font metrics for 32..126; ± 584,
// ° 400, µ 556, σ 603 per 1000 em; any other code point 556.
double text_width(std::string_view utf8, double size, bool bold = false);

// The next code point of UTF-8 text from byte i, i moved past it; a
// malformed byte is U+FFFD and one byte. Shared by the file writers.
char32_t next_code_point(std::string_view utf8, std::size_t& i);

// UTC time of unix seconds by a strftime subset (%Y %y %m %d %b %H %M %S %n
// %%), the time axis's labels.
std::string format_utc(double seconds, const std::string& format);

struct Box {
  double x = 0, y = 0, w = 0, h = 0;
};

// The size of one graph's cell of the page.
struct FigureSize {
  double graph_width = 432.0, graph_height = 324.0;  // 6 x 4.5 in
};

struct PanelLayout {
  std::string id;
  Box plot;  // the data area
  Range x, y;
  bool log_y = false;
  std::vector<Tick> x_ticks, y_ticks;
  bool x_tick_labels = false;  // the bottom panel of its graph only
};

struct GraphLayout {
  Box cell;
  std::string title;                // empty: none
  std::vector<PanelLayout> panels;  // top to bottom
};

struct SceneLayout {
  double width = 0, height = 0;
  std::vector<GraphLayout> graphs;  // the scene's, in order
};

// Graphs in max(1, columns) columns, row-major, style.graph_spacing apart.
// In a cell: the title on top when set; the panels share one left margin
// (the widest y tick label, the y title, a gap) and one right margin; the
// bottom margin (x tick labels and x title) is the last panel's; panel heights
// are proportional to max(0.05, height), style.panel_spacing apart.
SceneLayout layout_scene(const Scene& scene, const FigureSize& size = {});

// Page position of a data value in a panel; log y when log_y (y > 0).
double map_x(const PanelLayout& panel, double x);
double map_y(const PanelLayout& panel, double y);

}  // namespace pychron::processing
