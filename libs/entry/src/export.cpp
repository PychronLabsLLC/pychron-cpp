#include "pychron/entry/export.hpp"

#include <charconv>

#include "pychron/entry/csv.hpp"

namespace pychron::entry {

namespace {

std::string number(const std::optional<double>& v) {
  if (!v) return "";
  char buf[64];
  auto [end, ec] = std::to_chars(buf, buf + sizeof buf, *v);
  return ec == std::errc() ? std::string(buf, end) : std::string();
}

void append(const persistence::LevelSheet& sheet, std::vector<std::vector<std::string>>& rows) {
  for (const auto& p : sheet.positions)
    rows.push_back({sheet.level.name, std::to_string(p.position), p.identifier.value_or(""), p.sample_name, p.project,
                    p.principal_investigator, p.material, p.grainsize, number(p.weight), p.packet.value_or(""),
                    p.note.value_or(""), number(p.j), number(p.j_err)});
}

}  // namespace

const std::vector<std::string>& position_columns() {
  static const std::vector<std::string> columns = {"level",    "position", "identifier", "sample", "project",
                                                   "principal_investigator", "material", "grainsize", "weight",
                                                   "packet",   "note",     "j",          "j_err"};
  return columns;
}

std::string export_level_csv(const persistence::LevelSheet& sheet) {
  std::vector<std::vector<std::string>> rows;
  append(sheet, rows);
  return write_csv(position_columns(), rows);
}

std::string export_package_csv(const std::vector<persistence::LevelSheet>& sheets) {
  std::vector<std::vector<std::string>> rows;
  for (const auto& s : sheets) append(s, rows);
  return write_csv(position_columns(), rows);
}

}  // namespace pychron::entry
