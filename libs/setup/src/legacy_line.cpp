#include "pychron/setup/legacy_line.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

#include "legacy/lite.hpp"
#include "pychron/core/config/loader.hpp"
#include "pychron/systems/canvas/cross_validate.hpp"
#include "pychron/systems/canvas/loader.hpp"

namespace pychron::setup {

namespace fs = std::filesystem;
using namespace legacy;

namespace {

// --- legacy model -------------------------------------------------------------

struct LValve {
  std::string name, address, description, actuator;
  std::vector<std::string> interlocks;
};
struct LPipette {
  std::string name, inner, outer;
};
struct LValves {
  std::vector<LValve> valves;
  std::vector<LValve> manual;
  std::vector<LPipette> pipettes;
  // A setting the line config has no place for -> the valves that use it.
  std::map<std::string, std::vector<std::string>> unsupported;
};

struct LElement {
  std::string kind;  // valve, manual_valve, rough_valve, pipette, label, stage (anything drawn as a box)
  std::string legacy_kind;
  std::string name, text;
  // Legacy: unset = the name is drawn; "" = nothing is.
  std::optional<std::string> display_name;
  double x = 0, y = 0;
  std::optional<double> w, h;
  bool use_symbol = false;
  bool no_symbol = false;  // use_symbol="False" written out
};
struct LConnection {
  std::string kind;  // connection, h, v, tee, elbow
  std::string start, end, left, mid, right;
  std::string corner;  // elbow only
  // Where an end meets its element, from the element's lower-left corner in
  // world units; unset = its centre.
  std::optional<std::pair<double, double>> start_offset, end_offset;
};
struct LCanvas {
  std::vector<LElement> elements;
  std::vector<LConnection> connections;
  std::optional<std::pair<double, double>> xview, yview;
  double ox = 0, oy = 0;
  double valve_w = 2, valve_h = 2;  // <valve_dimension>
  bool pixels = false;  // valves2D.cfg: window pixels, y up
  double window_w = 800, window_h = 800;
};

std::optional<std::string> read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool is_file(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

std::optional<std::pair<double, double>> pair_of(const std::string& s) {
  const auto parts = split_list(s);
  if (parts.size() != 2) return std::nullopt;
  try {
    return std::make_pair(std::stod(parts[0]), std::stod(parts[1]));
  } catch (...) {
    return std::nullopt;
  }
}

// --- valves -------------------------------------------------------------------

std::vector<std::string> interlocks_of(const YNode* n) {
  std::vector<std::string> out;
  if (n == nullptr) return out;
  if (n->kind == YNode::Kind::Scalar) return split_list(n->scalar);
  if (n->is_seq())
    for (const auto& item : n->seq)
      if (item.kind == YNode::Kind::Scalar) out.push_back(trim(item.scalar));
  return out;
}

LValves valves_from_yaml(const YNode& root, std::vector<std::string>& notes) {
  static const std::set<std::string> known{"name", "address", "description", "interlock", "interlocks",
                                           "kind", "actuator", "inner",   "outer"};
  LValves out;
  if (!root.is_seq()) {
    notes.push_back("valves.yaml: not a list of valves; nothing read");
    return out;
  }
  for (const auto& item : root.seq) {
    if (!item.is_map()) continue;
    const std::string name = item.text("name");
    if (name.empty()) {
      notes.push_back("valves.yaml: an entry without a name was skipped");
      continue;
    }
    for (const auto& k : item.keys)
      if (!known.contains(k)) out.unsupported[k].push_back(name);
    const std::string kind = item.text("kind");
    if (kind == "pipette") {
      out.pipettes.push_back({name, item.text("inner"), item.text("outer")});
    } else if (kind == "manual_valve") {
      out.manual.push_back({name, "", item.text("description"), "", {}});
    } else {
      LValve v{name, item.text("address"), item.text("description"), item.text("actuator"), {}};
      v.interlocks = interlocks_of(item.get("interlock"));
      for (auto& i : interlocks_of(item.get("interlocks"))) v.interlocks.push_back(i);
      out.valves.push_back(std::move(v));
    }
  }
  return out;
}

void valves_from_xml(const XNode& node, LValves& out, std::vector<std::string>& notes) {
  static const std::set<std::string> known{"address", "description", "interlock", "interlocks", "actuator"};
  for (const auto& c : node.children) {
    if (c.tag == "group") {
      valves_from_xml(c, out, notes);
    } else if (c.tag == "manual_valve") {
      out.manual.push_back({c.text, "", c.child_text("description"), "", {}});
    } else if (c.tag == "pipette") {
      out.pipettes.push_back({c.text, c.child_text("inner"), c.child_text("outer")});
    } else if (c.tag == "valve") {
      LValve v{c.text, c.child_text("address"), c.child_text("description"), c.child_text("actuator"), {}};
      for (const auto& d : c.children) {
        if (d.tag == "interlock" || d.tag == "interlocks")
          for (auto& i : split_list(d.text)) v.interlocks.push_back(i);
        else if (!known.contains(d.tag)) out.unsupported[d.tag].push_back(v.name);
      }
      for (const auto& [k, val] : c.attrs) out.unsupported[k + "=\"" + val + "\""].push_back(v.name);
      out.valves.push_back(std::move(v));
    } else if (!c.tag.empty()) {
      notes.push_back("valves.xml: <" + c.tag + "> " + c.text + " not imported");
    }
  }
}

// --- canvas -------------------------------------------------------------------

const std::set<std::string>& switchable() {
  static const std::set<std::string> s{"valve", "manual_valve", "rough_valve"};
  return s;
}

void element_common(LElement& e, const std::string& translation, const std::string& dimension) {
  if (auto t = pair_of(translation)) {
    e.x = t->first;
    e.y = t->second;
  }
  if (auto d = pair_of(dimension)) {
    e.w = d->first;
    e.h = d->second;
  }
}

std::string element_kind(const std::string& legacy_kind) {
  if (switchable().contains(legacy_kind) || legacy_kind == "pipette" || legacy_kind == "label") return legacy_kind;
  return "stage";
}

bool is_connection(const std::string& k) {
  return k == "connection" || k == "hconnection" || k == "vconnection" || k == "tee_connection" ||
         k == "elbow" || k == "elbow_connection";
}

std::string conn_kind(const std::string& k, const std::string& orientation) {
  if (k == "tee_connection") return "tee";
  if (k == "elbow" || k == "elbow_connection") return "elbow";
  if (k == "hconnection" || orientation == "horizontal") return "h";
  if (k == "vconnection" || orientation == "vertical") return "v";
  return "connection";
}

LCanvas canvas_from_yaml(const YNode& root, std::vector<std::string>& notes) {
  LCanvas out;
  if (!root.is_map()) return out;
  for (std::size_t k = 0; k < root.keys.size(); ++k) {
    const std::string& kind = root.keys[k];
    const YNode& list = root.values[k];
    if (!list.is_seq()) continue;  // an empty or commented-out kind
    for (const auto& item : list.seq) {
      if (!item.is_map()) continue;
      if (is_connection(kind)) {
        LConnection c{conn_kind(kind, item.text("orientation")), item.text("start"), item.text("end"),
                      item.text("left"),  item.text("mid"),   item.text("right"), item.text("corner"), {}, {}};
        // an end is a name, or {name: X, offset: "dx,dy"}
        if (const YNode* s = item.get("start")) c.start_offset = pair_of(s->text("offset"));
        if (const YNode* e = item.get("end")) c.end_offset = pair_of(e->text("offset"));
        for (const char* arm : {"left", "mid", "right"})
          if (const YNode* a = item.get(arm); a && a->get("offset")) {
            notes.push_back("canvas: tee " + item.text("left") + "-" + item.text("mid") + "-" + item.text("right") +
                            ": end offset not carried over");
            break;
          }
        out.connections.push_back(std::move(c));
        continue;
      }
      if (kind == "image") {
        notes.push_back("canvas: image " + item.text("path") + " not carried over");
        continue;
      }
      LElement e;
      e.legacy_kind = kind;
      e.kind = element_kind(kind);
      e.name = item.text("name");
      if (item.get("display_name")) e.display_name = item.text("display_name");
      e.text = item.text("text");
      e.use_symbol = item.text("use_symbol") == "True" || item.text("use_symbol") == "true";
      e.no_symbol = item.get("use_symbol") && !e.use_symbol;
      element_common(e, item.text("translation"), item.text("dimension"));
      if (item.get("vlabel")) notes.push_back("canvas: " + e.name + " vlabel not carried over");
      if (e.kind == "label" && e.text.empty()) e.text = e.name;
      out.elements.push_back(std::move(e));
    }
  }
  return out;
}

// Legacy finds connections with //connection: at any depth, so one written
// inside the element it belongs to (NMGRL's <tank>) counts too.
void xml_connection(const XNode& c, std::vector<LConnection>& connections, std::vector<std::string>& notes) {
  auto orientation = c.attrs.count("orientation") ? c.attrs.at("orientation") : std::string{};
  // legacy reads the <corner> child, never a corner= attribute
  connections.push_back({conn_kind(c.tag, orientation), c.child_text("start"), c.child_text("end"),
                         c.child_text("left"), c.child_text("mid"), c.child_text("right"), c.child_text("corner"),
                         {}, {}});
  LConnection& made = connections.back();
  bool tee_noted = false;
  for (const auto& end : c.children) {
    if (!end.attrs.count("offset")) continue;
    if (end.tag == "start") made.start_offset = pair_of(end.attrs.at("offset"));
    else if (end.tag == "end") made.end_offset = pair_of(end.attrs.at("offset"));
    else if (!std::exchange(tee_noted, true))
      notes.push_back("canvas: tee " + made.left + "-" + made.mid + "-" + made.right +
                      ": end offset not carried over");
  }
}

void nested_xml_connections(const XNode& parent, std::vector<LConnection>& connections,
                            std::vector<std::string>& notes) {
  for (const auto& c : parent.children) {
    if (is_connection(c.tag)) xml_connection(c, connections, notes);
    else nested_xml_connections(c, connections, notes);
  }
}

LCanvas canvas_from_xml(const XNode& root, std::vector<std::string>& notes) {
  LCanvas out;
  for (const auto& c : root.children) {
    if (c.tag == "origin") {
      if (auto p = pair_of(c.text)) std::tie(out.ox, out.oy) = *p;
    } else if (c.tag == "xview" || c.tag == "xvidew") {
      out.xview = pair_of(c.text);
    } else if (c.tag == "yview") {
      out.yview = pair_of(c.text);
    } else if (c.tag == "valve_dimension") {
      if (auto p = pair_of(c.text)) std::tie(out.valve_w, out.valve_h) = *p;
    } else if (c.tag == "color" || c.tag == "connection_dimension") {
      continue;
    } else if (is_connection(c.tag)) {
      xml_connection(c, out.connections, notes);
    } else if (c.tag == "image") {
      notes.push_back("canvas: image " + c.text + " not carried over");
    } else {
      LElement e;
      e.legacy_kind = c.tag;
      e.kind = element_kind(c.tag);
      e.name = c.text;
      // legacy reads the attribute; a child is accepted as well
      if (c.attrs.count("display_name")) e.display_name = c.attrs.at("display_name");
      else if (c.child("display_name")) e.display_name = c.child_text("display_name");
      e.use_symbol = c.child_text("use_symbol") == "True";
      if (c.attrs.count("use_symbol")) {
        e.use_symbol = c.attrs.at("use_symbol") == "True";
        e.no_symbol = !e.use_symbol;
      }
      element_common(e, c.child_text("translation"), c.child_text("dimension"));
      if (e.kind == "label") e.text = c.text;
      out.elements.push_back(std::move(e));
      nested_xml_connections(c, out.connections, notes);
    }
  }
  return out;
}

LCanvas canvas_from_valves2d(const Ini& ini) {
  LCanvas out;
  out.pixels = true;
  if (auto g = ini.find("General"); g != ini.end()) {
    if (auto w = g->second.find("window_width"); w != g->second.end()) out.window_w = std::atof(w->second.c_str());
    if (auto h = g->second.find("window_height"); h != g->second.end()) out.window_h = std::atof(h->second.c_str());
  }
  for (const auto& [section, keys] : ini) {
    if (section.rfind("Valve-", 0) != 0) continue;
    LElement e;
    e.kind = e.legacy_kind = "valve";
    e.name = section.substr(6);
    if (auto pos = keys.find("pos"); pos != keys.end()) element_common(e, pos->second, "");
    out.elements.push_back(std::move(e));
  }
  return out;
}

void apply_canvas_config(const XNode& root, LCanvas& canvas) {
  for (const auto& c : root.children) {
    if (c.tag == "origin") {
      if (auto p = pair_of(c.text)) std::tie(canvas.ox, canvas.oy) = *p;
    } else if (c.tag == "xview" || c.tag == "xvidew") {
      if (auto p = pair_of(c.text)) canvas.xview = p;
    } else if (c.tag == "yview") {
      if (auto p = pair_of(c.text)) canvas.yview = p;
    } else if (c.tag == "valve_dimension") {
      if (auto p = pair_of(c.text)) std::tie(canvas.valve_w, canvas.valve_h) = *p;
    }
  }
}

// --- writing --------------------------------------------------------------------

std::string q(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

std::string num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.0f", std::round(v));
  return buf;
}

struct Actuator {
  std::string name, legacy_class, endpoint, kind, comment;
  std::string transport_kind = "sim", host, port;
};

Actuator resolve_actuator(const std::string& name, const std::optional<fs::path>& devices, std::vector<std::string>& read,
                          std::vector<std::string>& notes) {
  Actuator a;
  a.name = name;
  Ini own;
  if (devices && is_file(*devices / (name + ".cfg"))) {
    own = parse_ini(read_text(*devices / (name + ".cfg")).value_or(""));
    read.push_back("devices/" + name + ".cfg");
  } else {
    notes.push_back("actuator " + name + ": devices/" + name + ".cfg not found; simulated");
    a.kind = "sim_valves";
    a.comment = "legacy actuator " + name + ": no device file was found";
    return a;
  }
  auto general = own["General"];
  a.legacy_class = general.count("type") ? general["type"] : general.count("klass") ? general["klass"] : std::string{};
  Ini comms_from = own;
  if (!own.count("Communications") && !a.legacy_class.empty() && is_file(*devices / (a.legacy_class + ".cfg"))) {
    comms_from = parse_ini(read_text(*devices / (a.legacy_class + ".cfg")).value_or(""));
    read.push_back("devices/" + a.legacy_class + ".cfg");
  }
  auto comms = comms_from["Communications"];
  bool invert = general["invert"] == "True" || comms_from["General"]["invert"] == "True";
  if (invert) notes.push_back("actuator " + name + ": invert=True not carried over (no inverted logic in the line config yet)");
  const std::string type = comms["type"];
  if (!comms["host"].empty() || type == "ethernet") {
    a.host = comms["host"];
    a.port = comms["port"];
    a.endpoint = (a.host.empty() ? std::string("(no host)") : a.host) + ":" + a.port;
  } else if (!comms["port"].empty()) {
    a.endpoint = type + " " + comms["port"] + (comms["baudrate"].empty() ? "" : " @" + comms["baudrate"]);
  } else if (!type.empty()) {
    a.endpoint = type;
  }
  const std::string what = a.legacy_class.empty() ? std::string("unknown class") : a.legacy_class;
  if (a.legacy_class.find("NGX") != std::string::npos) {
    a.kind = "ngx_valves";
    if (!a.host.empty() && !a.port.empty()) {
      a.transport_kind = "tcp";
      a.comment = "legacy " + what + " at " + a.endpoint;
    } else {
      a.comment = "legacy " + what + ": no host in the device file; simulated until one is set";
      notes.push_back("actuator " + name + ": no NGX host in the device file; the transport is simulated");
    }
    return a;
  }
  a.kind = "sim_valves";
  a.comment = "legacy " + what + (a.endpoint.empty() ? "" : " at " + a.endpoint) +
              ": pychron-cpp has no driver for it yet; simulated";
  notes.push_back("actuator " + name + " (" + what + "): no pychron-cpp driver yet; simulated by sim_valves");
  return a;
}

}  // namespace

Result<LegacyLine> import_legacy_line(const fs::path& folder) {
  LegacyLine out;
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) return fail(ErrorKind::Config, folder.string() + " is not a folder");

  // Where the pieces are: the setupfiles folder, or one of its parts.
  const fs::path base = (folder.filename() == "extractionline" || folder.filename() == "canvas2D" ||
                         folder.filename() == "devices")
                            ? folder.parent_path()
                            : folder;
  auto first = [&](std::initializer_list<fs::path> candidates) -> std::optional<fs::path> {
    for (const auto& c : candidates)
      if (is_file(c)) return c;
    return std::nullopt;
  };
  const auto valves_yaml = first({base / "extractionline" / "valves.yaml", base / "valves.yaml"});
  const auto valves_xml = first({base / "extractionline" / "valves.xml", base / "valves.xml"});
  const auto canvas_yaml = first({base / "canvas2D" / "canvas.yaml", base / "canvas.yaml"});
  const auto canvas_xml = first({base / "canvas2D" / "canvas.xml", base / "canvas.xml"});
  const auto valves2d = first({base / "canvas2D" / "valves2D.cfg", base / "valves2D.cfg"});
  const auto canvas_config = first({base / "canvas2D" / "canvas_config.xml", base / "canvas_config.xml"});
  std::optional<fs::path> devices;
  if (fs::is_directory(base / "devices", ec)) devices = base / "devices";
  auto rel = [&](const fs::path& p) { return fs::relative(p, base, ec).generic_string(); };

  // Valves.
  LValves valves;
  std::vector<std::string> skipped;
  if (valves_yaml) {
    valves = valves_from_yaml(parse_yaml(read_text(*valves_yaml).value_or(""), skipped), out.notes);
    out.read.push_back(rel(*valves_yaml));
    if (valves_xml) out.notes.push_back(rel(*valves_xml) + " ignored: valves.yaml is read instead");
  } else if (valves_xml) {
    valves_from_xml(parse_xml(read_text(*valves_xml).value_or(""), skipped), valves, out.notes);
    out.read.push_back(rel(*valves_xml));
  } else {
    return fail(ErrorKind::Config, "no extractionline/valves.yaml or valves.xml under " + base.string());
  }
  for (const auto& s : skipped) out.notes.push_back("valve file: skipped " + s);
  for (const auto& [setting, names] : valves.unsupported) {
    std::string list;
    for (std::size_t i = 0; i < names.size() && i < 8; ++i) list += (i ? ", " : "") + names[i];
    if (names.size() > 8) list += ", ...";
    out.notes.push_back(setting + " not carried over (" + std::to_string(names.size()) +
                        (names.size() == 1 ? " valve: " : " valves: ") + list + ")");
  }
  if (valves.valves.empty() && valves.manual.empty())
    return fail(ErrorKind::Config, out.read.back() + " lists no valves");

  // Canvas.
  LCanvas canvas;
  skipped.clear();
  if (canvas_yaml) {
    canvas = canvas_from_yaml(parse_yaml(read_text(*canvas_yaml).value_or(""), skipped), out.notes);
    out.read.push_back(rel(*canvas_yaml));
  } else if (canvas_xml) {
    canvas = canvas_from_xml(parse_xml(read_text(*canvas_xml).value_or(""), skipped), out.notes);
    out.read.push_back(rel(*canvas_xml));
  } else if (valves2d) {
    canvas = canvas_from_valves2d(parse_ini(read_text(*valves2d).value_or("")));
    out.read.push_back(rel(*valves2d));
    out.notes.push_back("valves2D.cfg has positions only: connections and the other elements must be drawn again");
  } else {
    out.notes.push_back("no canvas file: the canvas holds the valves in a row");
  }
  for (const auto& s : skipped) out.notes.push_back("canvas file: skipped " + s);
  if (canvas_config) {
    std::vector<std::string> ignored;
    apply_canvas_config(parse_xml(read_text(*canvas_config).value_or(""), ignored), canvas);
    out.read.push_back(rel(*canvas_config));
  }

  // --- the line ---------------------------------------------------------------
  std::set<std::string> valve_names, manual_names, pipette_names;
  for (const auto& v : valves.valves) valve_names.insert(v.name);
  for (const auto& m : valves.manual) manual_names.insert(m.name);
  for (const auto& p : valves.pipettes) pipette_names.insert(p.name);

  std::vector<std::string> actuator_order;
  for (auto& v : valves.valves) {
    if (v.actuator.empty()) v.actuator = "switch_controller";
    if (v.address.empty()) v.address = v.name;  // NMGRL: the name is the address
    if (std::find(actuator_order.begin(), actuator_order.end(), v.actuator) == actuator_order.end())
      actuator_order.push_back(v.actuator);
  }
  std::vector<Actuator> actuators;
  for (const auto& name : actuator_order) actuators.push_back(resolve_actuator(name, devices, out.read, out.notes));

  // Interlocks and pipettes may only name valves the line has.
  for (auto& v : valves.valves) {
    std::vector<std::string> kept;
    for (const auto& i : v.interlocks) {
      if (valve_names.contains(i)) kept.push_back(i);
      else out.notes.push_back("valve " + v.name + ": interlock with unknown valve " + i + " dropped");
    }
    v.interlocks = std::move(kept);
  }
  std::vector<LPipette> pipettes;
  for (const auto& p : valves.pipettes) {
    if (valve_names.contains(p.inner) && valve_names.contains(p.outer)) pipettes.push_back(p);
    else out.notes.push_back("pipette " + p.name + ": its valves " + p.inner + "/" + p.outer + " are not both in the line; dropped");
  }

  std::string system_name = base.filename().string();
  if (system_name.empty() || system_name == "setupfiles") system_name = base.parent_path().filename().string();
  std::ostringstream line;
  line << "# Converted from a legacy Pychron setup (" << base.filename().string() << ") by pychron setup.\n"
       << "# Read: ";
  for (std::size_t i = 0; i < out.read.size(); ++i) line << (i ? ", " : "") << out.read[i];
  line << "\n# Review before running on hardware: the notes at the end say what was not carried over.\n\n"
       << "[system]\nname = " << q(system_name.empty() ? std::string("imported") : system_name)
       << "\nscan_interval_ms = 1000\n";
  for (const auto& a : actuators) {
    line << "\n# " << a.comment << "\n[transports." << q(a.name) << "]\nkind = " << q(a.transport_kind) << "\n";
    if (a.transport_kind == "tcp") line << "host = " << q(a.host) << "\nport = " << a.port << "\n";
    line << "timeout_ms = 2000\n\n[drivers." << q(a.name) << "]\nkind = " << q(a.kind) << "\ntransport = " << q(a.name)
         << "\n";
  }
  for (const auto& v : valves.valves) {
    line << "\n[[valves]]\nname = " << q(v.name) << "\n";
    if (!v.description.empty()) line << "description = " << q(v.description) << "\n";
    line << "actuator = " << q(v.actuator) << "\naddress = " << q(v.address) << "\n";
    if (!v.interlocks.empty()) {
      line << "interlocks = [";
      for (std::size_t i = 0; i < v.interlocks.size(); ++i) line << (i ? ", " : "") << q(v.interlocks[i]);
      line << "]\n";
    }
  }
  for (const auto& m : valves.manual) {
    line << "\n[[manual_valves]]\nname = " << q(m.name) << "\n";
    if (!m.description.empty()) line << "description = " << q(m.description) << "\n";
  }
  for (const auto& p : pipettes)
    line << "\n[[pipettes]]\nname = " << q(p.name) << "\ninner = " << q(p.inner) << "\nouter = " << q(p.outer) << "\n";

  // --- the canvas -------------------------------------------------------------
  // Keep what the line knows; draw the rest as boxes; drop what cannot be drawn.
  std::vector<LElement> elements;
  std::set<std::string> drawn;
  std::vector<std::string> illustrated;  // gauges drawn without a reading
  for (auto e : canvas.elements) {
    if (e.name.empty() && e.kind != "label") continue;
    if (e.kind != "label" && drawn.contains(e.name)) {
      out.notes.push_back("canvas: a second element named " + e.name + " dropped");
      continue;
    }
    if (switchable().contains(e.kind)) {
      if (manual_names.contains(e.name)) e.kind = "manual_valve";
      else if (valve_names.contains(e.name)) e.kind = e.kind == "rough_valve" ? "rough_valve" : "valve";
      else {
        out.notes.push_back("canvas: " + e.legacy_kind + " " + e.name + " is not in the valve file; dropped");
        continue;
      }
    } else if (e.kind == "pipette" && !pipette_names.contains(e.name)) {
      e.kind = "stage";
    }
    // A gauge is drawn, for illustration: legacy gauge controllers are not
    // imported, so it shows no reading until the line defines it.
    if (e.legacy_kind == "gauge") {
      e.kind = "gauge";
      illustrated.push_back(e.name);
    }
    if (e.kind != "label") drawn.insert(e.name);
    elements.push_back(std::move(e));
  }
  if (!illustrated.empty()) {
    std::string names;
    for (const auto& n : illustrated) names += (names.empty() ? "" : ", ") + n;
    out.notes.push_back("canvas: " + std::to_string(illustrated.size()) +
                        " gauges drawn for illustration, with no reading (legacy gauge controllers are not imported): " +
                        names);
  }
  // Valves the canvas does not draw go in a row along the bottom.
  {
    double x = 0;
    bool any = false;
    for (const auto& list : {&valves.valves, &valves.manual})
      for (const auto& v : *list)
        if (!drawn.contains(v.name)) any = true;
    if (any) {
      double ymin = 0;
      for (const auto& e : elements) ymin = std::min(ymin, e.y);
      for (const auto& list : {&valves.valves, &valves.manual}) {
        for (const auto& v : *list) {
          if (drawn.contains(v.name)) continue;
          LElement e;
          e.kind = list == &valves.manual ? "manual_valve" : "valve";
          e.name = v.name;
          e.x = x;
          e.y = canvas.pixels ? 20 : ymin - 6;
          x += canvas.pixels ? 40 : 4;
          drawn.insert(v.name);
          elements.push_back(std::move(e));
        }
      }
      out.notes.push_back("canvas: valves the drawing does not show were placed in a row at the bottom");
    }
  }

  // A legacy element's translation is its lower-left corner, a valve's too
  // (valve_dimension square unless it has its own): the 55-wide melbourne
  // stage at -26 is the one every valve from -25 to 25 connects to.
  // `extent` keeps each element's legacy size: connection offsets are
  // measured from the same corner.
  std::map<std::string, std::pair<double, double>> extent;
  if (!canvas.pixels)
    for (auto& e : elements) {
      if (e.kind == "label") continue;
      if (switchable().contains(e.kind)) {
        if (!e.w || !e.h) {
          e.w = canvas.valve_w;
          e.h = canvas.valve_h;
        }
      } else if (!e.w || !e.h) {
        continue;
      }
      e.x += *e.w / 2;
      e.y += *e.h / 2;
      extent[e.name] = {*e.w, *e.h};
    }

  // World units (y up) -> pixels (y down).
  double xmin = 0, xmax = 0, ymin = 0, ymax = 0;
  if (canvas.pixels) {
    xmax = canvas.window_w;
    ymax = canvas.window_h;
  } else if (canvas.xview && canvas.yview) {
    std::tie(xmin, xmax) = *canvas.xview;
    std::tie(ymin, ymax) = *canvas.yview;
  } else {
    bool first_e = true;
    for (const auto& e : elements) {
      const double hw = e.w.value_or(2) / 2, hh = e.h.value_or(2) / 2;
      xmin = first_e ? e.x - hw : std::min(xmin, e.x - hw);
      xmax = first_e ? e.x + hw : std::max(xmax, e.x + hw);
      ymin = first_e ? e.y - hh : std::min(ymin, e.y - hh);
      ymax = first_e ? e.y + hh : std::max(ymax, e.y + hh);
      first_e = false;
    }
    xmin -= 5;
    xmax += 5;
    ymin -= 5;
    ymax += 5;
  }
  if (xmax <= xmin) xmax = xmin + 100;
  if (ymax <= ymin) ymax = ymin + 100;
  const double scale = canvas.pixels ? 1.0 : std::clamp(1000.0 / (xmax - xmin), 4.0, 40.0);
  auto px = [&](double x) { return (x + canvas.ox - xmin) * scale; };
  auto py = [&](double y) { return (ymax - (y + canvas.oy)) * scale; };

  std::ostringstream cv;
  cv << "# Converted from a legacy Pychron canvas by pychron setup: positions rescaled from\n"
     << "# world units to pixels. Check the drawing and adjust positions as needed.\n\n"
     << "[canvas]\norigin = [0, 0]\nsize = [" << num((xmax - xmin) * scale) << ", " << num((ymax - ymin) * scale)
     << "]\nconnection_width = 5\n";
  for (const auto& e : elements) {
    if (e.kind == "label") {
      cv << "\n[[label]]\ntext = " << q(e.text.empty() ? e.name : e.text) << "\npos = [" << num(px(e.x)) << ", "
         << num(py(e.y)) << "]\n";
      continue;
    }
    cv << "\n[[" << e.kind << "]]\nname = " << q(e.name) << "\npos = [" << num(px(e.x)) << ", " << num(py(e.y)) << "]\n";
    if (e.kind == "stage" || e.kind == "pipette") {
      if (e.w && e.h) cv << "size = [" << num(std::max(10.0, *e.w * scale)) << ", " << num(std::max(10.0, *e.h * scale)) << "]\n";
    }
    if ((e.kind == "stage" || e.kind == "pipette") && e.display_name && *e.display_name != e.name)
      cv << "display_name = " << q(*e.display_name) << "\n";
    if (e.kind == "stage") {
      if (e.use_symbol) cv << "use_symbol = true\n";
      // A spectrometer, laser, turbo or getter is drawn as one, unless the
      // legacy canvas turned its symbol off.
      static const std::map<std::string, std::string> symbols{{"spectrometer", "spectrometer"},
                                                              {"laser", "laser"},
                                                              {"turbo", "turbo"},
                                                              {"getter", "getter"},
                                                              {"ionpump", "ion_pump"}};
      const auto symbol = symbols.find(e.legacy_kind);
      const bool has_glyph = symbol != symbols.end() && !e.no_symbol;
      if (has_glyph) cv << "symbol = " << q(symbol->second) << "\n";
      // What it is still decides the colour of the region it is connected
      // to (legacy precedence): said outright where no symbol says it.
      static const std::map<std::string, std::string> kinds{{"spectrometer", "spectrometer"}, {"laser", "laser"},
                                                            {"turbo", "pump"},                {"getter", "getter"},
                                                            {"ionpump", "pump"},              {"tank", "tank"},
                                                            {"pipette", "pipette"}};
      if (auto kind = kinds.find(e.legacy_kind); kind != kinds.end() && !has_glyph)
        cv << "kind = " << q(kind->second) << "\n";
    }
  }
  std::map<std::string, std::pair<double, double>> at;  // pixel positions as written
  for (const auto& e : elements)
    if (e.kind != "label") at[e.name] = {px(e.x), py(e.y)};
  // A legacy offset is from the element's lower-left corner, y up;
  // canvas.toml's is from its centre, in pixels, y down.
  using Offset = std::optional<std::pair<double, double>>;
  auto shift = [&](const std::string& name, const Offset& o) -> std::pair<double, double> {
    if (!o) return {0, 0};
    const auto [w, h] = extent.contains(name) ? extent.at(name) : std::pair{0.0, 0.0};
    return {(o->first - w / 2) * scale, -(o->second - h / 2) * scale};
  };
  auto meet = [&](const std::string& name, const Offset& o) -> std::pair<double, double> {
    const auto [dx, dy] = shift(name, o);
    return {at[name].first + dx, at[name].second + dy};
  };
  auto offsets = [&](const LConnection& c) {
    for (const auto& [key, name, o] : {std::tuple{"start_offset", &c.start, &c.start_offset},
                                       std::tuple{"end_offset", &c.end, &c.end_offset}}) {
      const auto [dx, dy] = shift(*name, *o);
      if (num(dx) != "0" || num(dy) != "0") cv << key << " = [" << num(dx) << ", " << num(dy) << "]\n";
    }
  };
  for (const auto& c : canvas.connections) {
    if (c.kind == "tee") {
      const bool ok = drawn.contains(c.left) && drawn.contains(c.mid) && drawn.contains(c.right);
      if (!ok) {
        out.notes.push_back("canvas: tee " + c.left + "-" + c.mid + "-" + c.right + " names an element that is not drawn; dropped");
        continue;
      }
      cv << "\n[[tee]]\nleft = " << q(c.left) << "\nright = " << q(c.right) << "\nmid = " << q(c.mid) << "\n";
      continue;
    }
    if (!drawn.contains(c.start) || !drawn.contains(c.end)) {
      out.notes.push_back("canvas: connection " + c.start + "-" + c.end + " names an element that is not drawn; dropped");
      continue;
    }
    if (c.kind == "elbow") {
      // Legacy turns at (start.x, end.y), or at (end.x, start.y) for "lr",
      // whatever else the corner says. canvas.toml names the corner of the
      // ends' bounding box instead, so name the one legacy turned at.
      const auto [sx, sy] = meet(c.start, c.start_offset);
      const auto [ex, ey] = meet(c.end, c.end_offset);
      if (num(sx) != num(ex) && num(sy) != num(ey)) {  // lined up as written: a plain connection
        const bool lr = c.corner == "lr";
        const bool left = lr ? ex < sx : sx < ex;
        const bool upper = lr ? sy < ey : ey < sy;  // pixels: y down
        cv << "\n[[elbow]]\nstart = " << q(c.start) << "\nend = " << q(c.end) << "\ncorner = "
           << q(std::string(upper ? "u" : "l") + (left ? "l" : "r")) << "\n";
        offsets(c);
        continue;
      }
    }
    cv << "\n[[connection]]\nstart = " << q(c.start) << "\nend = " << q(c.end) << "\n";
    if (c.kind == "h" || c.kind == "v") cv << "orientation = " << q(c.kind) << "\n";
    offsets(c);
  }

  if (!out.notes.empty()) {
    line << "\n# Notes from the conversion:\n";
    for (const auto& n : out.notes) line << "#   " << n << "\n";
  }
  out.line_toml = line.str();
  out.canvas_toml = cv.str();

  // The converted files must load the way the programs load them.
  auto system = config::load_system_config_from_string(out.line_toml, "extraction_line.toml");
  if (!system) return fail(ErrorKind::Config, "the converted extraction line does not load:\n" + system.error().what);
  auto drawing = canvas::load_canvas_from_string(out.canvas_toml, "canvas.toml");
  if (!drawing) return fail(ErrorKind::Config, "the converted canvas does not load:\n" + drawing.error().what);
  if (auto checked = canvas::check_canvas(*drawing, *system); !checked)
    return fail(ErrorKind::Config, "the converted canvas does not match the line:\n" + checked.error().what);
  return out;
}

}  // namespace pychron::setup
