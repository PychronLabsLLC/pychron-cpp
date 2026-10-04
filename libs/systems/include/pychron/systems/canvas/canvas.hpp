#pragma once

// Qt-free model of `canvas.toml` (spec section 6): pure presentation and
// connectivity. No state lives here; valve state and gauge readings arrive via
// the SignalBus. Domain objects are referenced by name from
// `extraction_line.toml`.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/config/system_config.hpp"

namespace pychron::canvas {

using config::Located;

struct Point {
  double x = 0;
  double y = 0;
  friend bool operator==(const Point&, const Point&) = default;
};

struct Size {
  double width = 0;
  double height = 0;
  friend bool operator==(const Size&, const Size&) = default;
};

// How an open valve is filled: the usual state green, or the colour of the
// shared region it joins (a lone open valve joining nothing stays green).
enum class OpenValveColor { Green, Inherit };

struct CanvasSection : Located {
  Point origin{0, 0};
  Size size{1000, 700};
  std::int64_t connection_width = 5;
  OpenValveColor open_valve_color = OpenValveColor::Green;
};

// Every switchable element shares one struct; `kind` records which array it
// was declared in (`[[valve]]`, `[[manual_valve]]`, `[[rough_valve]]`,
// `[[switch]]`).
enum class ValveKind { Valve, Manual, Rough, Switch };

struct ValveElement : Located {
  std::string name;
  ValveKind kind = ValveKind::Valve;
  Point pos;
};

// A gauge readout placed on the plumbing; its value arrives as PressureSample.
struct GaugeElement : Located {
  std::string name;
  Point pos;
};

// What a stage is, for the glyph drawn inside its box; None draws the name only.
enum class StageSymbol { None, Spectrometer, Quadrupole, Laser, Turbo, Getter, IonPump };

struct StageElement : Located {
  std::string name;
  Point pos;
  Size size{50, 50};
  std::optional<double> volume;  // cc; used by later volume logic
  bool fill = false;
  // The label in the box: unset = the name; "" = no label.
  std::optional<std::string> display_name;
  bool use_symbol = false;
  StageSymbol symbol = StageSymbol::None;
};

struct PipetteElement : Located {
  std::string name;
  Point pos;
  Size size{50, 50};
  std::string vlabel;
  std::optional<std::string> display_name;  // as a stage's; unset = vlabel, else the name
};

enum class Orientation { Auto, Horizontal, Vertical };

// start_offset / end_offset move where the pipe meets its element, in pixels
// from the element's centre (so a pipe can join a wide volume off-centre).
struct Connection : Located {
  std::string start;
  std::string end;
  Orientation orientation = Orientation::Auto;
  Point start_offset;
  Point end_offset;
};

enum class Corner { UpperLeft, UpperRight, LowerLeft, LowerRight };

struct Elbow : Located {
  std::string start;
  std::string end;
  Corner corner = Corner::UpperLeft;
  Point start_offset;
  Point end_offset;
};

// Junctions: every listed endpoint is joined to every other with no valve
// in between.
struct Tee : Located {
  std::string left;
  std::string right;
  std::string mid;
};

struct Cross : Located {
  std::string left;
  std::string right;
  std::string top;
  std::string bottom;
};

struct Label : Located {
  std::string text;
  Point pos;
  std::string font;
};

struct Image : Located {
  std::string path;
  Point pos;
};

struct Legend : Located {
  Point pos;
};

struct Canvas {
  std::string source_file;
  CanvasSection canvas;
  std::map<std::string, std::string> colors;
  std::vector<ValveElement> valves;  // all ValveKinds, in declaration order per kind
  std::vector<GaugeElement> gauges;
  std::vector<StageElement> stages;
  std::vector<PipetteElement> pipettes;
  std::vector<Connection> connections;
  std::vector<Elbow> elbows;
  std::vector<Tee> tees;
  std::vector<Cross> crosses;
  std::vector<Label> labels;
  std::vector<Image> images;
  std::optional<Legend> legend;
};

}  // namespace pychron::canvas
