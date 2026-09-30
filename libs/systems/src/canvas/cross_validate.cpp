#include "pychron/systems/canvas/cross_validate.hpp"

#include <filesystem>
#include <set>
#include <string>

namespace pychron::canvas {
namespace {

using config::Diagnostic;

template <class Range>
std::set<std::string, std::less<>> names_of(const Range& items) {
  std::set<std::string, std::less<>> out;
  for (const auto& i : items) out.insert(i.name);
  return out;
}

}  // namespace

CrossReport cross_validate(const Canvas& canvas, const config::SystemConfig& system) {
  CrossReport r;
  const auto system_file = std::filesystem::path(system.source_file).filename().generic_string();
  const auto valves = names_of(system.valves);
  const auto manual = names_of(system.manual_valves);
  const auto gauges = names_of(system.gauges);
  const auto pipettes = names_of(system.pipettes);

  auto missing = [&](const Located& e, const std::string& what, const std::string& name, std::string hint = {}) {
    std::string msg = what + " '" + name + "' is not defined in " + system_file;
    if (!hint.empty()) msg += " (" + hint + ")";
    r.errors.push_back({e.where("name"), e.path + ".name", std::move(msg)});
  };

  std::set<std::string, std::less<>> drawn;
  for (const auto& v : canvas.valves) {
    drawn.insert(v.name);
    switch (v.kind) {
      case ValveKind::Valve:
      case ValveKind::Rough:
        if (!valves.contains(v.name)) {
          missing(v, "valve", v.name, manual.contains(v.name) ? "it is a manual valve; draw it as [[manual_valve]]" : "");
        }
        break;
      case ValveKind::Manual:
        if (!manual.contains(v.name)) {
          missing(v, "manual valve", v.name, valves.contains(v.name) ? "it is an actuated valve; draw it as [[valve]]" : "");
        }
        break;
      case ValveKind::Switch:
        break;
    }
  }
  for (const auto& g : canvas.gauges) {
    if (!gauges.contains(g.name)) missing(g, "gauge", g.name);
  }
  for (const auto& p : canvas.pipettes) {
    if (!pipettes.contains(p.name)) missing(p, "pipette", p.name);
  }

  const auto canvas_file = std::filesystem::path(canvas.source_file).filename().generic_string();
  for (const auto& v : system.valves) {
    if (!drawn.contains(v.name)) {
      r.warnings.push_back({v.loc, v.path, "valve '" + v.name + "' is not drawn on the canvas (" + canvas_file + ")"});
    }
  }
  for (const auto& v : system.manual_valves) {
    if (!drawn.contains(v.name)) {
      r.warnings.push_back(
          {v.loc, v.path, "manual valve '" + v.name + "' is not drawn on the canvas (" + canvas_file + ")"});
    }
  }
  return r;
}

Result<std::vector<config::Diagnostic>> check_canvas(const Canvas& canvas, const config::SystemConfig& system) {
  auto r = cross_validate(canvas, system);
  if (!r.ok()) return fail(config::to_error(r.errors));
  return std::move(r.warnings);
}

}  // namespace pychron::canvas
