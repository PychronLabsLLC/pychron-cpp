#include "pychron/systems/canvas/canvas.hpp"

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
  const auto add = [&](const std::string& name, SourceKind kind, int precedence,
                       const std::optional<std::string>& color) {
    if (precedence <= 0 || out.contains(name)) return;
    out.emplace(name, Source{name, kind, precedence, color, count[kind]++});
  };
  for (const auto& s : canvas.stages) {
    const SourceKind kind = source_kind(s);
    add(s.name, kind, s.precedence.value_or(default_precedence(kind)), s.color);
  }
  for (const auto& p : canvas.pipettes) {
    add(p.name, SourceKind::Pipette, p.precedence.value_or(default_precedence(SourceKind::Pipette)), p.color);
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
