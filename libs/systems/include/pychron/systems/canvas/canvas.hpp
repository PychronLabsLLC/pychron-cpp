#pragma once

// Qt-free model of `canvas.toml` (spec section 6): pure presentation and
// connectivity. No state lives here; valve state and gauge readings arrive via
// the SignalBus. Domain objects are referenced by name from
// `extraction_line.toml`.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
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
  // The label on the valve's face. Unset: the name, except on a manual
  // valve, whose face is blank (its handle marks it; the tooltip names it).
  std::optional<std::string> display_name;

  const std::string& label() const {
    static const std::string none;
    return display_name ? *display_name : kind == ValveKind::Manual ? none : name;
  }
};

// A gauge readout placed on the plumbing; its value arrives as PressureSample.
struct GaugeElement : Located {
  std::string name;
  Point pos;
};

// What a stage is, for the glyph drawn inside its box; None draws the name only.
enum class StageSymbol { None, Spectrometer, Quadrupole, Laser, Turbo, Getter, IonPump };

// What a stage is to the gas in it. A source colours every volume connected
// to it; when a region holds several, the one with the highest precedence
// wins (legacy pychron's rule). The order here breaks a tie.
enum class SourceKind { None, Pump, Pipette, Laser, Tank, Spectrometer, Getter };

// Pump 120, tank 110, pipette and laser 100, spectrometer 80, getter 70,
// none 0. Legacy pychron's numbers but for the tank (90 there): what is open
// to a tank is that tank's gas, so it beats the pipette it fills.
int default_precedence(SourceKind kind) noexcept;

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
  // Unset: what the symbol says (a turbo or an ion pump is a pump, ...); a
  // stage with neither is a plain volume.
  std::optional<SourceKind> kind;
  std::optional<int> precedence;     // unset: the kind's; 0: colours nothing
  std::optional<std::string> color;  // "#rrggbb"; unset: the theme's for the kind
  // For a pipette: the tank whose gas it holds, and whose colour it wears.
  // Unset: the one tank across a valve from it, if there is exactly one;
  // "": none.
  std::optional<std::string> tank;
};

SourceKind source_kind(const StageElement& stage) noexcept;

struct PipetteElement : Located {
  std::string name;
  Point pos;
  Size size{50, 50};
  std::string vlabel;
  std::optional<std::string> display_name;  // as a stage's; unset = vlabel, else the name
  std::optional<int> precedence;            // unset: a pipette's (100)
  std::optional<std::string> color;         // "#rrggbb"
  std::optional<std::string> tank;          // as a stage's
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

// An element that colours the region it is connected to.
struct Source {
  std::string name;
  SourceKind kind = SourceKind::None;
  int precedence = 0;
  std::optional<std::string> color;  // the element's own; unset: the theme's for `kind`
  // Its place among the canvas's sources of this kind, from 0, in the order
  // the canvas lists them: each tank is drawn in a colour of its own.
  int ordinal = 0;
  // A pipette's tank (a name in the same map), empty if it has none: the
  // pipette holds that tank's gas whether or not the valve between them is
  // open, so it is drawn in the tank's colour unless it has its own.
  std::string tank;
};

// The canvas's stages and pipettes with a precedence above 0, by name.
std::map<std::string, Source, std::less<>> sources(const Canvas& canvas);

// The source that colours a region holding `volumes`: the highest
// precedence, then the kind's place in SourceKind, then the name. Null when
// the region has none. Depends on nothing but who is connected.
const Source* dominant(const std::map<std::string, Source, std::less<>>& sources,
                       const std::set<std::string>& volumes);

}  // namespace pychron::canvas
