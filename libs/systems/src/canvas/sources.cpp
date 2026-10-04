#include "pychron/systems/canvas/canvas.hpp"

#include <set>

namespace pychron::canvas {

int default_precedence(SourceKind kind) noexcept {
  switch (kind) {
    case SourceKind::Pump: return 120;
    case SourceKind::Pipette: return 100;
    case SourceKind::Laser: return 100;
    case SourceKind::Tank: return 110;
    case SourceKind::Spectrometer: return 80;
    case SourceKind::Getter: return 70;
    case SourceKind::None: return 0;
  }
  return 0;
}

SourceKind source_kind(const StageElement& stage) noexcept {
  if (stage.kind) return *stage.kind;
  switch (stage.symbol) {
    case StageSymbol::Turbo:
    case StageSymbol::IonPump: return SourceKind::Pump;
    case StageSymbol::Laser: return SourceKind::Laser;
    case StageSymbol::Spectrometer:
    case StageSymbol::Quadrupole: return SourceKind::Spectrometer;
    case StageSymbol::Getter: return SourceKind::Getter;
    case StageSymbol::None: return SourceKind::None;
  }
  return SourceKind::None;
}

std::map<std::string, Source, std::less<>> sources(const Canvas& canvas) {
  std::map<std::string, Source, std::less<>> out;
  std::map<SourceKind, int> count;
  std::map<std::string, std::optional<std::string>, std::less<>> named_tank;  // what the file says, by pipette
  const auto add = [&](const std::string& name, SourceKind kind, int precedence,
                       const std::optional<std::string>& color, const std::optional<std::string>& tank) {
    if (precedence <= 0 || out.contains(name)) return;
    out.emplace(name, Source{name, kind, precedence, color, count[kind]++, {}});
    if (kind == SourceKind::Pipette) named_tank.emplace(name, tank);
  };
  for (const auto& s : canvas.stages) {
    const SourceKind kind = source_kind(s);
    add(s.name, kind, s.precedence.value_or(default_precedence(kind)), s.color, s.tank);
  }
  for (const auto& p : canvas.pipettes) {
    add(p.name, SourceKind::Pipette, p.precedence.value_or(default_precedence(SourceKind::Pipette)), p.color, p.tank);
  }

  // Whose pipette is it: the file's word, else the one tank that shares a
  // valve with it (tank - valve - pipette, as an aliquot system is plumbed).
  std::set<std::string, std::less<>> valves;
  for (const auto& v : canvas.valves) {
    if (v.kind != ValveKind::Switch) valves.insert(v.name);
  }
  std::map<std::string, std::set<std::string>, std::less<>> beside;  // valve -> what it is piped to
  const auto pipe = [&](const std::string& a, const std::string& b) {
    if (valves.contains(a)) beside[a].insert(b);
    if (valves.contains(b)) beside[b].insert(a);
  };
  for (const auto& c : canvas.connections) pipe(c.start, c.end);
  for (const auto& e : canvas.elbows) pipe(e.start, e.end);
  const auto is_tank = [&out](const std::string& name) {
    const auto it = out.find(name);
    return it != out.end() && it->second.kind == SourceKind::Tank;
  };
  for (const auto& [pipette, named] : named_tank) {
    Source& source = out.at(pipette);
    if (named) {
      if (is_tank(*named)) source.tank = *named;
      continue;
    }
    std::set<std::string> tanks;
    for (const auto& [valve, ends] : beside) {
      if (!ends.contains(pipette)) continue;
      for (const auto& other : ends) {
        if (is_tank(other)) tanks.insert(other);
      }
    }
    if (tanks.size() == 1) source.tank = *tanks.begin();
  }
  return out;
}

const Source* dominant(const std::map<std::string, Source, std::less<>>& sources,
                       const std::set<std::string>& volumes) {
  const Source* best = nullptr;
  // `volumes` is sorted, so among equals the first name stays.
  for (const auto& volume : volumes) {
    const auto it = sources.find(volume);
    if (it == sources.end()) continue;
    const Source& s = it->second;
    if (best == nullptr || s.precedence > best->precedence ||
        (s.precedence == best->precedence && s.kind < best->kind)) {
      best = &s;
    }
  }
  return best;
}

}  // namespace pychron::canvas
