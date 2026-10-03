// Path -> file kind (tests/dvc/fixtures/README.md, section 3).

#include <array>
#include <vector>

#include "pychron/dvc/legacy_layout.hpp"

namespace pychron::dvc {

namespace {

// <p>/<directory>/<t><suffix>. The suffix is ".<directory[:4]>.json", except
// that a directory starting with '.' brings its own dot (".data" -> ".dat.json").
struct Modifier {
  std::string_view directory, suffix;
  FileKind kind;
  bool under_reduction;  // may also sit under a top-level "reduction/"
};

constexpr std::array<Modifier, 12> kModifiers{{
    {".data", ".dat.json", FileKind::Data, false},
    {"intercepts", ".inte.json", FileKind::Intercepts, false},
    {"baselines", ".base.json", FileKind::Baselines, false},
    {"blanks", ".blan.json", FileKind::Blanks, false},
    {"icfactors", ".icfa.json", FileKind::IcFactors, false},
    {"tags", ".tags.json", FileKind::Tags, true},
    {"peakcenter", ".peak.json", FileKind::PeakCenter, false},
    {"extraction", ".extr.json", FileKind::Extraction, false},
    {"monitor", ".moni.json", FileKind::Monitor, false},
    {"cosmogenic", ".cosm.json", FileKind::Cosmogenic, false},
    {"ia", ".ia.json", FileKind::InterpretedAge, true},
    {"logs", ".logs.log", FileKind::Ignored, false},
}};

constexpr std::string_view kJson = ".json";
constexpr std::string_view kProduction = ".production.json";

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool is_hex40(std::string_view s) {
  if (s.size() != 40) return false;
  for (const char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  return true;
}

std::vector<std::string_view> split(std::string_view path) {
  std::vector<std::string_view> parts;
  while (true) {
    const auto slash = path.find('/');
    parts.push_back(path.substr(0, slash));
    if (slash == std::string_view::npos) return parts;
    path.remove_prefix(slash + 1);
  }
}

PathInfo analysis_file(FileKind kind, std::string_view prefix, std::string_view tail) {
  PathInfo info;
  info.kind = kind;
  if (kind == FileKind::Ignored) return info;
  info.key = std::string(prefix) + std::string(tail);
  info.key_is_uuid = kind != FileKind::InterpretedAge && persistence::Uuid::parse(info.key).has_value();
  return info;
}

PathInfo root_file(std::string_view name) {
  PathInfo info;
  if (name.front() == '.' || name.starts_with("README")) {
    info.kind = FileKind::Ignored;  // git's own files (.gitignore, .gitattributes) and the host's README
  } else if (ends_with(name, kProduction)) {
    const std::string_view key = name.substr(0, name.size() - kProduction.size());
    const auto dot = key.rfind('.');
    if (dot != std::string_view::npos && dot > 0 && dot + 1 < key.size()) {
      info.kind = FileKind::FrozenProduction;
      info.key = std::string(key);
    }
  } else if (ends_with(name, kJson) && is_hex40(name.substr(0, name.size() - kJson.size()))) {
    info.kind = FileKind::Spectrometer;
    info.key = std::string(name.substr(0, name.size() - kJson.size()));
  }
  return info;
}

}  // namespace

PathInfo classify_path(std::string_view repo_path) {
  auto parts = split(repo_path);
  for (const auto part : parts)
    if (part.empty()) return {};
  if (parts.size() == 1) return root_file(parts[0]);

  const bool reduction = parts.size() == 4 && parts[0] == "reduction";
  if (reduction) parts.erase(parts.begin());

  if (parts.size() == 2 && !reduction) {
    // <p>/<t>.json: the tail of a runid or uuid has no dot.
    const std::string_view prefix = parts[0], name = parts[1];
    if (!ends_with(name, kJson)) return {};
    const std::string_view tail = name.substr(0, name.size() - kJson.size());
    if (tail.empty() || tail.find('.') != std::string_view::npos) return {};
    return analysis_file(FileKind::Record, prefix, tail);
  }
  if (parts.size() == 3) {
    const std::string_view prefix = parts[0], directory = parts[1], name = parts[2];
    for (const auto& m : kModifiers) {
      if (m.directory != directory) continue;
      if (reduction && !m.under_reduction) return {};
      if (!ends_with(name, m.suffix)) return {};
      const std::string_view tail = name.substr(0, name.size() - m.suffix.size());
      if (tail.empty()) return {};
      return analysis_file(m.kind, prefix, tail);
    }
  }
  return {};
}

FrozenProductionKey split_frozen_production_key(std::string_view key) {
  const auto dot = key.rfind('.');
  if (dot == std::string_view::npos) return {std::string(key), {}};
  return {std::string(key.substr(0, dot)), std::string(key.substr(dot + 1))};
}

std::string make_runid(std::string_view identifier, int aliquot, int increment) {
  return persistence::make_runid(std::string(identifier), aliquot, increment);
}

}  // namespace pychron::dvc
