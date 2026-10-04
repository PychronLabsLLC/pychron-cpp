#include "pychron/systems/canvas/loader.hpp"

#include <array>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace pychron::canvas {
namespace {

using config::Diagnostic;
using config::SourceLoc;
using Keys = std::set<std::string, std::less<>>;

std::string_view type_name(toml::node_type t) {
  switch (t) {
    case toml::node_type::none: return "nothing";
    case toml::node_type::table: return "table";
    case toml::node_type::array: return "array";
    case toml::node_type::string: return "string";
    case toml::node_type::integer: return "integer";
    case toml::node_type::floating_point: return "float";
    case toml::node_type::boolean: return "boolean";
    case toml::node_type::date: return "date";
    case toml::node_type::time: return "time";
    case toml::node_type::date_time: return "date-time";
  }
  return "unknown";
}

constexpr std::array kOrientations{
    std::pair<std::string_view, Orientation>{"auto", Orientation::Auto},
    std::pair<std::string_view, Orientation>{"h", Orientation::Horizontal},
    std::pair<std::string_view, Orientation>{"v", Orientation::Vertical},
};

constexpr std::array kCorners{
    std::pair<std::string_view, Corner>{"ul", Corner::UpperLeft},
    std::pair<std::string_view, Corner>{"ur", Corner::UpperRight},
    std::pair<std::string_view, Corner>{"ll", Corner::LowerLeft},
    std::pair<std::string_view, Corner>{"lr", Corner::LowerRight},
};

constexpr std::array kStageSymbols{
    std::pair<std::string_view, StageSymbol>{"spectrometer", StageSymbol::Spectrometer},
    std::pair<std::string_view, StageSymbol>{"quadrupole", StageSymbol::Quadrupole},
    std::pair<std::string_view, StageSymbol>{"laser", StageSymbol::Laser},
    std::pair<std::string_view, StageSymbol>{"turbo", StageSymbol::Turbo},
    std::pair<std::string_view, StageSymbol>{"getter", StageSymbol::Getter},
    std::pair<std::string_view, StageSymbol>{"ion_pump", StageSymbol::IonPump},
};

constexpr std::array kOpenValveColors{
    std::pair<std::string_view, OpenValveColor>{"green", OpenValveColor::Green},
    std::pair<std::string_view, OpenValveColor>{"inherit", OpenValveColor::Inherit},
};

constexpr std::array kValveSections{
    std::pair<std::string_view, ValveKind>{"valve", ValveKind::Valve},
    std::pair<std::string_view, ValveKind>{"manual_valve", ValveKind::Manual},
    std::pair<std::string_view, ValveKind>{"rough_valve", ValveKind::Rough},
    std::pair<std::string_view, ValveKind>{"switch", ValveKind::Switch},
};

const Keys kSections{"canvas", "colors",     "valve", "manual_valve", "rough_valve", "switch",
                     "gauge",  "stage",      "pipette", "connection", "elbow",       "tee",
                     "cross",  "label",      "image",   "legend"};

class CanvasBuilder {
 public:
  CanvasBuilder(std::string file, std::vector<Diagnostic>& out) : file_(std::move(file)), out_(out) {}

  Canvas build(const toml::table& root) {
    Canvas c;
    c.source_file = file_;
    for (auto&& [k, v] : root) {
      if (!kSections.contains(k.str())) error(loc(v), std::string(k.str()), "unknown section");
    }

    if (const auto* n = root.get("canvas")) {
      if (const auto* t = as_table(*n, "canvas")) parse_canvas_section(*t, c.canvas);
    } else {
      c.canvas.loc = SourceLoc{file_, 1, 1};
      c.canvas.path = "canvas";
    }
    if (const auto* n = root.get("colors")) {
      if (const auto* t = as_table(*n, "colors")) parse_colors(*t, c.colors);
    }
    if (const auto* n = root.get("legend")) {
      if (const auto* t = as_table(*n, "legend")) c.legend = parse_legend(*t);
    }

    for (const auto& [section, kind] : kValveSections) {
      each(root, section, [&](const std::string& path, const toml::table& t) {
        c.valves.push_back(parse_valve(path, t, kind));
      });
    }
    each(root, "gauge", [&](const std::string& p, const toml::table& t) { c.gauges.push_back(parse_gauge(p, t)); });
    each(root, "stage", [&](const std::string& p, const toml::table& t) { c.stages.push_back(parse_stage(p, t)); });
    each(root, "pipette",
         [&](const std::string& p, const toml::table& t) { c.pipettes.push_back(parse_pipette(p, t)); });
    each(root, "connection",
         [&](const std::string& p, const toml::table& t) { c.connections.push_back(parse_connection(p, t)); });
    each(root, "elbow", [&](const std::string& p, const toml::table& t) { c.elbows.push_back(parse_elbow(p, t)); });
    each(root, "tee", [&](const std::string& p, const toml::table& t) { c.tees.push_back(parse_tee(p, t)); });
    each(root, "cross", [&](const std::string& p, const toml::table& t) { c.crosses.push_back(parse_cross(p, t)); });
    each(root, "label", [&](const std::string& p, const toml::table& t) { c.labels.push_back(parse_label(p, t)); });
    each(root, "image", [&](const std::string& p, const toml::table& t) { c.images.push_back(parse_image(p, t)); });
    return c;
  }

 private:
  // ---- primitives ---------------------------------------------------------

  SourceLoc loc(const toml::node& n) const {
    const auto& src = n.source();
    return SourceLoc{src.path ? std::string(*src.path) : file_, src.begin.line, src.begin.column};
  }

  void error(SourceLoc where, std::string field, std::string message) {
    out_.push_back({std::move(where), std::move(field), std::move(message)});
  }

  static std::string field(const Located& e, std::string_view key) { return e.path + "." + std::string(key); }

  void begin(Located& e, const toml::node& n, std::string path) {
    e.loc = loc(n);
    e.path = std::move(path);
  }

  const toml::node* find(const toml::table& t, Located& e, std::string_view key, bool required) {
    const auto* n = t.get(key);
    if (n == nullptr) {
      if (required) error(e.loc, field(e, key), "missing required field");
      return nullptr;
    }
    e.field_locs[std::string(key)] = loc(*n);
    return n;
  }

  void type_error(const Located& e, std::string_view key, const toml::node& n, std::string_view expected) {
    error(loc(n), field(e, key), "expected " + std::string(expected) + ", got " + std::string(type_name(n.type())));
  }

  void read(const toml::table& t, Located& e, std::string_view key, std::string& out, bool required) {
    const auto* n = find(t, e, key, required);
    if (n == nullptr) return;
    if (const auto* s = n->as_string()) {
      out = s->get();
      if (required && out.empty()) error(loc(*n), field(e, key), "must not be empty");
      return;
    }
    type_error(e, key, *n, "string");
  }

  // Present, even when empty, is different from absent.
  void read(const toml::table& t, Located& e, std::string_view key, std::optional<std::string>& out) {
    if (!t.contains(key)) return;
    std::string s;
    read(t, e, key, s, false);
    out = std::move(s);
  }

  void read(const toml::table& t, Located& e, std::string_view key, bool& out) {
    const auto* n = find(t, e, key, false);
    if (n == nullptr) return;
    if (const auto* b = n->as_boolean()) {
      out = b->get();
      return;
    }
    type_error(e, key, *n, "boolean");
  }

  void read(const toml::table& t, Located& e, std::string_view key, std::int64_t& out, std::int64_t min) {
    const auto* n = find(t, e, key, false);
    if (n == nullptr) return;
    const auto* i = n->as_integer();
    if (i == nullptr) return type_error(e, key, *n, "integer");
    if (i->get() < min) {
      error(loc(*n), field(e, key), "value " + std::to_string(i->get()) + " out of range, must be >= " + std::to_string(min));
      return;
    }
    out = i->get();
  }

  static std::optional<double> number(const toml::node& n) {
    if (const auto* f = n.as_floating_point()) return f->get();
    if (const auto* i = n.as_integer()) return static_cast<double>(i->get());
    return std::nullopt;
  }

  void read(const toml::table& t, Located& e, std::string_view key, std::optional<double>& out) {
    const auto* n = find(t, e, key, false);
    if (n == nullptr) return;
    if (auto v = number(*n)) {
      out = v;
      return;
    }
    type_error(e, key, *n, "number");
  }

  // `[x, y]`; with `positive`, both components must be > 0.
  bool read_pair(const toml::table& t, Located& e, std::string_view key, double& a, double& b, bool required,
                 bool positive) {
    const auto* n = find(t, e, key, required);
    if (n == nullptr) return false;
    const auto* arr = n->as_array();
    if (arr == nullptr) {
      type_error(e, key, *n, "array");
      return false;
    }
    std::optional<double> x, y;
    if (arr->size() == 2) {
      x = number(*arr->get(0));
      y = number(*arr->get(1));
    }
    if (!x || !y) {
      error(loc(*n), field(e, key), "expected an array of 2 numbers");
      return false;
    }
    if (positive && (*x <= 0 || *y <= 0)) {
      error(loc(*n), field(e, key), "components must be positive");
      return false;
    }
    a = *x;
    b = *y;
    return true;
  }

  void read(const toml::table& t, Located& e, std::string_view key, Point& out, bool required) {
    read_pair(t, e, key, out.x, out.y, required, false);
  }

  void read(const toml::table& t, Located& e, std::string_view key, Size& out) {
    read_pair(t, e, key, out.width, out.height, false, true);
  }

  template <class E, std::size_t N>
  void read_enum(const toml::table& t, Located& e, std::string_view key, E& out,
                 const std::array<std::pair<std::string_view, E>, N>& table) {
    std::string s;
    read(t, e, key, s, false);
    if (s.empty()) return;
    for (const auto& [name, value] : table) {
      if (name == s) {
        out = value;
        return;
      }
    }
    std::string allowed;
    for (const auto& [name, value] : table) allowed += (allowed.empty() ? "" : " | ") + std::string(name);
    error(e.where(std::string(key)), field(e, key), "invalid value '" + s + "' (expected " + allowed + ")");
  }

  void reject_unknown(const toml::table& t, const Located& e, const Keys& allowed) {
    for (auto&& [k, v] : t) {
      if (!allowed.contains(k.str())) error(loc(v), field(e, k.str()), "unknown field");
    }
  }

  const toml::table* as_table(const toml::node& n, const std::string& path) {
    if (const auto* t = n.as_table()) return t;
    error(loc(n), path, "expected table, got " + std::string(type_name(n.type())));
    return nullptr;
  }

  // Calls `fn("<section>[i]", table)` for each entry of a `[[section]]` array.
  void each(const toml::table& root, std::string_view section,
            const std::function<void(const std::string&, const toml::table&)>& fn) {
    const auto* n = root.get(section);
    if (n == nullptr) return;
    const auto* arr = n->as_array();
    if (arr == nullptr) {
      error(loc(*n), std::string(section),
            "expected array of tables [[" + std::string(section) + "]], got " + std::string(type_name(n->type())));
      return;
    }
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const auto path = std::string(section) + "[" + std::to_string(i) + "]";
      if (const auto* t = as_table(*arr->get(i), path)) fn(path, *t);
    }
  }

  // ---- sections -----------------------------------------------------------

  void parse_canvas_section(const toml::table& t, CanvasSection& s) {
    begin(s, t, "canvas");
    reject_unknown(t, s, Keys{"origin", "size", "connection_width", "open_valve_color"});
    read(t, s, "origin", s.origin, false);
    read(t, s, "size", s.size);
    read(t, s, "connection_width", s.connection_width, 1);
    read_enum(t, s, "open_valve_color", s.open_valve_color, kOpenValveColors);
  }

  void parse_colors(const toml::table& t, std::map<std::string, std::string>& colors) {
    for (auto&& [k, v] : t) {
      if (const auto* s = v.as_string()) {
        colors[std::string(k.str())] = s->get();
      } else {
        error(loc(v), "colors." + std::string(k.str()), "expected string, got " + std::string(type_name(v.type())));
      }
    }
  }

  Legend parse_legend(const toml::table& t) {
    Legend l;
    begin(l, t, "legend");
    reject_unknown(t, l, Keys{"pos"});
    read(t, l, "pos", l.pos, true);
    return l;
  }

  ValveElement parse_valve(const std::string& path, const toml::table& t, ValveKind kind) {
    ValveElement v;
    begin(v, t, path);
    v.kind = kind;
    reject_unknown(t, v, Keys{"name", "pos"});
    read(t, v, "name", v.name, true);
    read(t, v, "pos", v.pos, true);
    return v;
  }

  GaugeElement parse_gauge(const std::string& path, const toml::table& t) {
    GaugeElement g;
    begin(g, t, path);
    reject_unknown(t, g, Keys{"name", "pos"});
    read(t, g, "name", g.name, true);
    read(t, g, "pos", g.pos, true);
    return g;
  }

  StageElement parse_stage(const std::string& path, const toml::table& t) {
    StageElement s;
    begin(s, t, path);
    reject_unknown(t, s, Keys{"name", "pos", "size", "volume", "fill", "display_name", "use_symbol", "symbol"});
    read(t, s, "name", s.name, true);
    read(t, s, "pos", s.pos, true);
    read(t, s, "size", s.size);
    read(t, s, "volume", s.volume);
    read(t, s, "fill", s.fill);
    read(t, s, "display_name", s.display_name);
    read(t, s, "use_symbol", s.use_symbol);
    read_enum(t, s, "symbol", s.symbol, kStageSymbols);
    return s;
  }

  PipetteElement parse_pipette(const std::string& path, const toml::table& t) {
    PipetteElement p;
    begin(p, t, path);
    reject_unknown(t, p, Keys{"name", "pos", "size", "vlabel", "display_name"});
    read(t, p, "name", p.name, true);
    read(t, p, "pos", p.pos, true);
    read(t, p, "size", p.size);
    read(t, p, "vlabel", p.vlabel, false);
    read(t, p, "display_name", p.display_name);
    return p;
  }

  Connection parse_connection(const std::string& path, const toml::table& t) {
    Connection c;
    begin(c, t, path);
    reject_unknown(t, c, Keys{"start", "end", "orientation", "start_offset", "end_offset"});
    read(t, c, "start", c.start, true);
    read(t, c, "end", c.end, true);
    read(t, c, "start_offset", c.start_offset, false);
    read(t, c, "end_offset", c.end_offset, false);
    read_enum(t, c, "orientation", c.orientation, kOrientations);
    return c;
  }

  Elbow parse_elbow(const std::string& path, const toml::table& t) {
    Elbow e;
    begin(e, t, path);
    reject_unknown(t, e, Keys{"start", "end", "corner", "start_offset", "end_offset"});
    read(t, e, "start", e.start, true);
    read(t, e, "end", e.end, true);
    read(t, e, "start_offset", e.start_offset, false);
    read(t, e, "end_offset", e.end_offset, false);
    read_enum(t, e, "corner", e.corner, kCorners);
    return e;
  }

  Tee parse_tee(const std::string& path, const toml::table& t) {
    Tee e;
    begin(e, t, path);
    reject_unknown(t, e, Keys{"left", "right", "mid"});
    read(t, e, "left", e.left, true);
    read(t, e, "right", e.right, true);
    read(t, e, "mid", e.mid, true);
    return e;
  }

  Cross parse_cross(const std::string& path, const toml::table& t) {
    Cross e;
    begin(e, t, path);
    reject_unknown(t, e, Keys{"left", "right", "top", "bottom"});
    read(t, e, "left", e.left, true);
    read(t, e, "right", e.right, true);
    read(t, e, "top", e.top, true);
    read(t, e, "bottom", e.bottom, true);
    return e;
  }

  Label parse_label(const std::string& path, const toml::table& t) {
    Label l;
    begin(l, t, path);
    reject_unknown(t, l, Keys{"text", "pos", "font"});
    read(t, l, "text", l.text, true);
    read(t, l, "pos", l.pos, true);
    read(t, l, "font", l.font, false);
    return l;
  }

  Image parse_image(const std::string& path, const toml::table& t) {
    Image i;
    begin(i, t, path);
    reject_unknown(t, i, Keys{"path", "pos"});
    read(t, i, "path", i.path, true);
    read(t, i, "pos", i.pos, true);
    return i;
  }

  std::string file_;
  std::vector<Diagnostic>& out_;
};

// Name uniqueness and endpoint resolution on a structurally clean canvas.
std::vector<Diagnostic> validate(const Canvas& c) {
  std::vector<Diagnostic> out;
  enum class Role { Plumbing, Switch };
  std::map<std::string, Role, std::less<>> names;

  auto declare = [&](const Located& e, const std::string& name, Role role) {
    if (!names.try_emplace(name, role).second) {
      out.push_back({e.where("name"), e.path + ".name", "duplicate element name '" + name + "'"});
    }
  };
  for (const auto& v : c.valves) declare(v, v.name, v.kind == ValveKind::Switch ? Role::Switch : Role::Plumbing);
  for (const auto& g : c.gauges) declare(g, g.name, Role::Plumbing);
  for (const auto& s : c.stages) declare(s, s.name, Role::Plumbing);
  for (const auto& p : c.pipettes) declare(p, p.name, Role::Plumbing);

  auto endpoint = [&](const Located& e, const char* key, const std::string& name) {
    auto it = names.find(name);
    if (it == names.end()) {
      out.push_back({e.where(key), e.path + "." + key, "unknown element '" + name + "'"});
    } else if (it->second == Role::Switch) {
      out.push_back({e.where(key), e.path + "." + key, "switch '" + name + "' is not plumbing and cannot be connected"});
    }
  };
  auto pair = [&](const Located& e, const std::string& start, const std::string& end) {
    endpoint(e, "start", start);
    endpoint(e, "end", end);
    if (start == end) out.push_back({e.where("end"), e.path + ".end", "connects '" + start + "' to itself"});
  };
  for (const auto& x : c.connections) pair(x, x.start, x.end);
  for (const auto& x : c.elbows) pair(x, x.start, x.end);
  for (const auto& x : c.tees) {
    endpoint(x, "left", x.left);
    endpoint(x, "right", x.right);
    endpoint(x, "mid", x.mid);
  }
  for (const auto& x : c.crosses) {
    endpoint(x, "left", x.left);
    endpoint(x, "right", x.right);
    endpoint(x, "top", x.top);
    endpoint(x, "bottom", x.bottom);
  }
  return out;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

CanvasLoadReport load_canvas_report_from_string(std::string_view text, std::string_view name) {
  CanvasLoadReport report;
  auto parsed = toml::parse(text, name);
  if (!parsed) {
    const auto& err = parsed.error();
    const auto& src = err.source();
    report.diagnostics.push_back({SourceLoc{std::string(name), src.begin.line, src.begin.column}, "toml",
                                  "syntax error: " + std::string(err.description())});
    return report;
  }
  auto canvas = CanvasBuilder(std::string(name), report.diagnostics).build(parsed.table());
  // Reference checks only run on a structurally clean file, so a missing name
  // is not also reported as a dangling endpoint.
  if (report.diagnostics.empty()) report.diagnostics = validate(canvas);
  if (report.diagnostics.empty()) report.canvas = std::move(canvas);
  return report;
}

CanvasLoadReport load_canvas_report(const std::filesystem::path& path) {
  const auto name = path.generic_string();
  auto text = read_file(path);
  if (!text) {
    CanvasLoadReport report;
    report.diagnostics.push_back({SourceLoc{name, 0, 0}, "file", "cannot read canvas file"});
    return report;
  }
  return load_canvas_report_from_string(*text, name);
}

Result<Canvas> load_canvas_from_string(std::string_view text, std::string_view name) {
  auto report = load_canvas_report_from_string(text, name);
  if (!report.ok()) return fail(config::to_error(report.diagnostics));
  return std::move(*report.canvas);
}

Result<Canvas> load_canvas(const std::filesystem::path& path) {
  auto report = load_canvas_report(path);
  if (!report.ok()) return fail(config::to_error(report.diagnostics));
  return std::move(*report.canvas);
}

}  // namespace pychron::canvas
