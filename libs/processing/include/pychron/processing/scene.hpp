#pragma once

// The scene model (design section 8.1): everything a figure needs to be drawn,
// computed by a figure unit, rendered by the UI without further computation.
// Panels are listed top to bottom.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pychron::processing {

struct Color {
  std::uint8_t r = 0, g = 0, b = 0, a = 255;
  friend bool operator==(const Color&, const Color&) = default;
};

// "#rrggbb", "#rrggbbaa" or a palette name; nullopt if malformed.
std::optional<Color> parse_color(std::string_view text);
std::string color_hex(const Color& c);  // "#rrggbb" or "#rrggbbaa" when a != 255
// The default group palette: 12 distinguishable colours.
Color palette_color(int index);

enum class MarkerShape { Circle, Square, Diamond, Triangle, Cross, Plus, Star };
std::string_view to_string(MarkerShape s) noexcept;
std::optional<MarkerShape> parse_marker(std::string_view text) noexcept;

struct MarkerStyle {
  MarkerShape shape = MarkerShape::Circle;
  double size = 5.0;
  Color color;
  bool filled = true;
};

enum class LineDash { Solid, Dash, Dot, DashDot };

struct LineStyle {
  Color color;
  double width = 1.0;
  LineDash dash = LineDash::Solid;
};

struct PointRef {
  std::string analysis;  // uuid
  friend bool operator==(const PointRef&, const PointRef&) = default;
};

// Scatter with optional error bars (errors already scaled by nsigma).
struct PointLayer {
  std::vector<double> x, y, x_err, y_err;  // errors empty or one per point
  std::vector<PointRef> refs;
  std::vector<bool> excluded;
  std::vector<std::string> tooltips;
  MarkerStyle marker, excluded_marker;
  bool show_excluded = true;
  std::string label;  // legend; empty: not in the legend
  int group = 0;
};

struct LineLayer {
  std::vector<double> x, y;
  LineStyle style;
  std::string label;
  int group = 0;
};

struct BandLayer {  // fit envelope
  std::vector<double> x, low, high;
  Color fill;
  int group = 0;
};

enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight };
std::string_view to_string(Corner c) noexcept;
std::optional<Corner> parse_corner(std::string_view text) noexcept;

struct TextLayer {
  std::vector<std::string> lines;
  Corner corner = Corner::TopLeft;
  int stack = 0;  // order among texts in the same corner
  Color color;
  double font_size = 9.0;
  int group = 0;
};

struct GuideLayer {
  bool horizontal = true;
  double value = 0.0;
  LineStyle style;
  std::string label;
};

using Layer = std::variant<PointLayer, LineLayer, BandLayer, TextLayer, GuideLayer>;

enum class AxisScale { Linear, Log };
enum class AxisFormat { Number, Time, Category };

struct Axis {
  std::string title;
  AxisScale scale = AxisScale::Linear;
  AxisFormat format = AxisFormat::Number;
  std::string time_format;  // strftime-like; empty: automatic
  std::optional<double> min, max;
  bool visible = true;
  std::vector<std::string> categories;  // Category: label of value i
};

struct Panel {
  std::string id;        // stable per panel row ("p0", ...)
  std::string quantity;  // canonical quantity text
  double height = 1.0;   // relative
  Axis y;
  std::vector<Layer> layers;
};

struct Graph {
  std::string title;
  Axis x;
  std::vector<Panel> panels;  // top to bottom; x shared
};

struct Fonts {
  std::string family;  // empty: application default
  double title = 12, axis_title = 10, tick = 9, annotation = 9;
};

struct SceneStyle {
  Color background{255, 255, 255, 255};
  Color plot_background{255, 255, 255, 255};
  bool grid = true;
  Fonts fonts;
  bool legend = true;
  Corner legend_corner = Corner::TopRight;
  int panel_spacing = 4;
};

struct Scene {
  std::string kind;  // "time_series"
  std::vector<Graph> graphs;
  int columns = 1;
  SceneStyle style;
  std::vector<std::string> warnings;

  std::size_t point_count() const;  // over every PointLayer
};

using ScenePtr = std::shared_ptr<const Scene>;

}  // namespace pychron::processing
