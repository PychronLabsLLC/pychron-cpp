#include "pychron/systems/spectrometer/data_dir.hpp"

#include <algorithm>
#include <array>
#include <set>

#include "pychron/systems/spectrometer/config_loader.hpp"
#include "pychron/systems/spectrometer/config_validate.hpp"
#include "toml_reader.hpp"

namespace pychron::spectrometer::cfg {

namespace fs = std::filesystem;
using detail::Obj;
using detail::Reader;

// ---- layout ---------------------------------------------------------------

fs::path config_path(const fs::path& root) { return root / "spectrometer.toml"; }
fs::path molecular_weights_path(const fs::path& root) { return root / "molecular_weights.toml"; }
fs::path table_dir(const fs::path& root, std::string_view name) { return root / "tables" / std::string(name); }
fs::path table_current_pointer(const fs::path& root, std::string_view name) { return table_dir(root, name) / "current"; }
fs::path profile_path(const fs::path& root, std::string_view name) {
  return root / "profiles" / (std::string(name) + ".toml");
}

std::vector<std::string> list_profiles(const fs::path& root) {
  std::vector<std::string> out;
  std::error_code ec;
  for (fs::directory_iterator it(root / "profiles", ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() == ".toml") out.push_back(it->path().stem().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

Result<fs::path> resolve_current_table(const fs::path& root, std::string_view name) {
  const auto pointer = table_current_pointer(root, name);
  auto text = detail::read_file(pointer);
  if (!text) return fail(ErrorKind::Config, "cannot read " + pointer.generic_string());
  const auto first = text->find_first_not_of(" \t\r\n");
  const auto last = text->find_last_not_of(" \t\r\n");
  const std::string version = first == std::string::npos ? std::string() : text->substr(first, last - first + 1);
  const fs::path rel(version);
  // The pointer names a sibling file; anything else would escape the table dir.
  if (version.empty() || rel.has_parent_path() || rel.filename() != rel || version == "." || version == "..") {
    return fail(ErrorKind::Config, pointer.generic_string() + ": invalid version '" + version + "'");
  }
  auto file = table_dir(root, name) / rel;
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    return fail(ErrorKind::Config, pointer.generic_string() + " points at missing version '" + version + "'");
  }
  return file;
}

// ---- field table files ----------------------------------------------------

namespace {

constexpr std::array<std::pair<std::string_view, FitKind>, 4> kFits{{
    {"discrete", FitKind::Discrete},
    {"linear", FitKind::Linear},
    {"quadratic", FitKind::Quadratic},
    {"cubic", FitKind::Cubic},
}};

constexpr std::array<std::pair<std::string_view, Axis>, 3> kAxes{
    {{"dac", Axis::Dac}, {"field", Axis::Field}, {"mass", Axis::Mass}}};

}  // namespace

std::string_view to_string(FitKind fit) noexcept {
  for (const auto& [name, f] : kFits) {
    if (f == fit) return name;
  }
  return "unknown";
}

std::vector<std::string> TableFile::columns() const {
  std::vector<std::string> out;
  if (points.empty()) return out;
  for (const auto& [det, value] : points.front().values) {
    if (has_column(det)) out.push_back(det);
  }
  return out;
}

bool TableFile::has_column(std::string_view detector) const {
  if (points.empty()) return false;
  return std::all_of(points.begin(), points.end(),
                     [&](const TablePoint& p) { return p.values.contains(std::string(detector)); });
}

TableLoadReport load_table_from_string(std::string_view toml, std::string_view file, std::string_view name) {
  TableLoadReport report;
  auto root = detail::parse_toml(toml, file, report.diagnostics);
  if (!root) return report;
  Reader r(std::string(file), report.diagnostics);
  TableFile t;
  t.name = std::string(name);
  t.source_file = std::string(file);
  const Obj top = r.obj(*root, "");
  r.only(top, {"fit", "axis", "points"});
  r.choice(top, "fit", t.fit, detail::Choices<FitKind>{kFits.data(), kFits.size()}, true);
  r.choice(top, "axis", t.axis, detail::Choices<Axis>{kAxes.data(), kAxes.size()}, true);

  const auto* n = root->get("points");
  const auto* arr = n != nullptr ? n->as_array() : nullptr;
  if (n != nullptr && arr == nullptr) {
    r.error(r.loc(*n), "points", "expected array of tables ([[points]])");
  } else if (arr == nullptr || arr->empty()) {
    r.error(top.loc, "points", "table needs at least one point ([[points]])");
  } else {
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const auto path = "points[" + std::to_string(i) + "]";
      const auto* pt = r.table(*arr->get(i), path);
      if (pt == nullptr) continue;
      const Obj o = r.obj(*pt, path);
      TablePoint p;
      r.str(o, "isotope", p.isotope, true);
      r.number(o, "mass", p.mass, true);
      for (auto&& [k, v] : *pt) {
        if (k.str() == "isotope" || k.str() == "mass") continue;
        if (auto value = Reader::as_number(v)) {
          p.values[std::string(k.str())] = *value;
        } else {
          r.error(r.loc(v), Reader::join(path, k.str()),
                  "expected number, got " + std::string(detail::type_name(v.type())));
        }
      }
      t.points.push_back(std::move(p));
    }
  }
  if (report.diagnostics.empty()) report.table = std::move(t);
  return report;
}

TableLoadReport load_table(const fs::path& file, std::string_view name) {
  auto text = detail::read_file(file);
  if (!text) {
    TableLoadReport report;
    report.diagnostics.push_back({config::SourceLoc{file.generic_string(), 0, 0}, "file", "cannot read table file"});
    return report;
  }
  return load_table_from_string(*text, file.generic_string(), name);
}

// ---- molecular weights ----------------------------------------------------

const MolecularWeights& default_molecular_weights() {
  // Atomic masses (amu), AME2020.
  static const MolecularWeights kDefaults{
      {"He3", 3.0160293201},     {"He4", 4.0026032541},     {"Ne20", 19.9924401762},   {"Ne21", 20.993846685},
      {"Ne22", 21.991385114},    {"Ar36", 35.967545105},    {"Ar37", 36.96677631},     {"Ar38", 37.96273211},
      {"Ar39", 38.964313},       {"Ar40", 39.9623831237},   {"Ar41", 40.9645006},      {"Kr78", 77.92036494},
      {"Kr80", 79.91637808},     {"Kr82", 81.91348273},     {"Kr83", 82.91412716},     {"Kr84", 83.9114977282},
      {"Kr86", 85.9106106269},   {"Xe124", 123.905892},     {"Xe126", 125.9042983},    {"Xe128", 127.903531},
      {"Xe129", 128.9047808611}, {"Xe130", 129.903509349},  {"Xe131", 130.90508406},   {"Xe132", 131.9041550856},
      {"Xe134", 133.90539466},   {"Xe136", 135.907214484},
  };
  return kDefaults;
}

WeightsLoadReport load_molecular_weights_from_string(std::string_view toml, std::string_view file) {
  WeightsLoadReport report;
  auto root = detail::parse_toml(toml, file, report.diagnostics);
  if (!root) return report;
  Reader r(std::string(file), report.diagnostics);
  MolecularWeights w = default_molecular_weights();
  for (auto&& [k, v] : *root) {
    const auto key = std::string(k.str());
    auto value = Reader::as_number(v);
    if (!value) {
      r.error(r.loc(v), key, "expected number, got " + std::string(detail::type_name(v.type())));
    } else if (*value <= 0.0) {
      r.error(r.loc(v), key, "mass must be > 0");
    } else {
      w[key] = *value;
    }
  }
  if (report.diagnostics.empty()) report.weights = std::move(w);
  return report;
}

// ---- whole directory ------------------------------------------------------

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved): its elements are moved out
void append(std::vector<config::Diagnostic>& out, std::vector<config::Diagnostic>&& more) {
  out.insert(out.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
}

}  // namespace

DataLoadReport load_spectrometer_data(const fs::path& config_file) {
  DataLoadReport report;
  SpectrometerData data;
  data.root = config_file.parent_path();

  auto parsed = parse_config(config_file);
  append(report.diagnostics, std::move(parsed.diagnostics));

  const auto weights_file = molecular_weights_path(data.root);
  std::error_code ec;
  if (fs::exists(weights_file, ec)) {
    auto text = detail::read_file(weights_file);
    if (!text) {
      report.diagnostics.push_back(
          {config::SourceLoc{weights_file.generic_string(), 0, 0}, "file", "cannot read molecular weights"});
    } else {
      auto w = load_molecular_weights_from_string(*text, weights_file.generic_string());
      append(report.diagnostics, std::move(w.diagnostics));
      if (w.weights) data.weights = std::move(*w.weights);
    }
  } else {
    data.weights = default_molecular_weights();
  }

  if (!parsed.config) return report;  // rules need a structurally clean config
  data.config = std::move(*parsed.config);

  // Load the current version of every referenced table. A table directory
  // that does not exist is left for check_field_tables to report; a broken
  // one is reported here.
  std::set<std::string> names{data.config.magnet.field_table};
  if (!data.config.magnet.hv_table.empty()) names.insert(data.config.magnet.hv_table);
  for (const auto& name : names) {
    if (!fs::exists(table_current_pointer(data.root, name), ec)) continue;
    auto file = resolve_current_table(data.root, name);
    if (!file) {
      report.diagnostics.push_back({config::SourceLoc{table_current_pointer(data.root, name).generic_string(), 0, 0},
                                    "tables." + name, file.error().what});
      continue;
    }
    auto t = load_table(*file, name);
    append(report.diagnostics, std::move(t.diagnostics));
    if (t.table) data.tables.emplace(name, std::move(*t.table));
  }

  append(report.diagnostics, validate(data.config, data.tables));
  if (report.diagnostics.empty()) report.data = std::move(data);
  return report;
}

Result<SpectrometerData> load_spectrometer(const fs::path& config_file) {
  auto report = load_spectrometer_data(config_file);
  if (!report.ok()) return fail(config::to_error(report.diagnostics));
  return std::move(*report.data);
}

}  // namespace pychron::spectrometer::cfg
