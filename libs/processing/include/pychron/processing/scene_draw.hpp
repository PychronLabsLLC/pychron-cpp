#pragma once

// A scene as a list of paths and texts on a page (data publishing design,
// section 5.6): what the SVG and PDF writers serialize, so both place the
// same primitives. Painted in list order; lengths in points, origin top left,
// y down. Layers map as the figure view (SceneView) paints them: per panel
// the plot background, grid, the layers in scene order, the corner texts,
// the legend (first panel), the axes, tick labels and titles. Insets are not
// drawn, as the view draws none. Non-finite values, and values <= 0 on a log
// axis, are left out.

#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "pychron/processing/scene.hpp"
#include "pychron/processing/scene_layout.hpp"

namespace pychron::processing {

struct Pt {
  double x = 0, y = 0;
};

struct PathOp {
  enum class Kind { Move, Line, Curve, Close };
  Kind kind = Kind::Move;
  std::array<Pt, 3> p{};  // Move, Line: p[0]; Curve: p[0], p[1] controls, p[2] end
};

enum class DrawRole { Frame, Grid, Axis, Data, Marker, ErrorBar, Step, Ellipse, Band, Guide, Span, Legend, TextBox };

struct DrawPath {
  std::vector<PathOp> ops;
  std::optional<Color> stroke, fill;
  double width = 1.0;
  LineDash dash = LineDash::Solid;
  int clip = -1;  // index into DrawList::clips; -1: none
  DrawRole role = DrawRole::Data;
};

enum class TextAnchor { Start, Middle, End };
enum class TextRole { Title, AxisTitle, Tick, Annotation, StepLabel, SpanLabel, Legend, Empty };

struct DrawText {
  Pt at;             // the anchor on the baseline
  std::string text;  // one line, UTF-8
  double size = 9.0;
  bool bold = false;
  TextAnchor anchor = TextAnchor::Start;
  bool vertical = false;  // rotated -90 degrees about `at` (a y title)
  Color color;
  int clip = -1;
  TextRole role = TextRole::Annotation;
};

using DrawItem = std::variant<DrawPath, DrawText>;

struct DrawList {
  double width = 0, height = 0;
  Color background{255, 255, 255, 255};
  std::string font_family;  // the scene's; empty: Helvetica
  std::vector<Box> clips;
  std::vector<DrawItem> items;  // painting order
};

DrawList draw_scene(const Scene& scene, const FigureSize& size = {});

// A coordinate as both writers print it: fixed with at most 2 decimals,
// trailing zeros and point dropped, "-0" printed "0" (std::to_chars, so the
// same on every compiler). A non-finite value prints "0"; the draw list holds
// none.
std::string format_coord(double v);

// Dash pattern of a line style in points: Dash 6 3, Dot 1 2, DashDot 6 2 1 2;
// empty for Solid.
std::vector<double> dash_pattern(LineDash dash);

}  // namespace pychron::processing
