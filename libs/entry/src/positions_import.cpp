#include "pychron/entry/positions_import.hpp"

#include <charconv>
#include <cmath>

#include "pychron/entry/names.hpp"
#include "pychron/core/number.hpp"

namespace pychron::entry {

namespace ps = persistence;

PositionImportResult apply_position_import(const CsvTable& table, const CatalogSnapshot& catalog,
                                           std::map<std::string, LevelSheetEdit>& levels,
                                           const std::vector<std::string>& pi_names_allowed) {
  PositionImportResult out;
  std::map<std::string, std::size_t> column;
  for (std::size_t i = 0; i < table.header.size(); ++i) {
    std::string h = trim(table.header[i]);
    for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (h == "pi") h = "principal_investigator";
    if (h == "hole") h = "position";
    column.emplace(h, i);
  }
  for (const char* required : {"level", "position", "sample"})
    if (!column.count(required)) out.errors.push_back(std::string("no '") + required + "' column");
  if (!out.errors.empty()) return out;

  struct Planned {
    std::string level;
    int position;
    const ps::SampleRow* sample;
    std::optional<double> weight;
    std::optional<std::string> packet, note;
  };
  std::vector<Planned> planned;
  for (std::size_t r = 0; r < table.rows.size(); ++r) {
    const auto& cells = table.rows[r];
    const auto get = [&](const char* name) {
      const auto it = column.find(name);
      return it == column.end() || it->second >= cells.size() ? std::string() : trim(cells[it->second]);
    };
    const std::string at = "line " + std::to_string(table.lines[r]) + ": ";
    Planned p{get("level"), 0, nullptr, std::nullopt, std::nullopt, std::nullopt};
    if (!levels.count(p.level)) {
      out.errors.push_back(at + "no level '" + p.level + "' in this package");
      continue;
    }
    const std::string position = get("position");
    auto [end, ec] = std::from_chars(position.data(), position.data() + position.size(), p.position);
    if (ec != std::errc() || end != position.data() + position.size() || p.position < 1) {
      out.errors.push_back(at + "position '" + position + "' is not a number from 1");
      continue;
    }
    const std::string sample = get("sample");
    if (!sample.empty()) {
      std::string pi = get("principal_investigator");
      if (!pi.empty())
        if (auto parsed = parse_pi(pi, pi_names_allowed)) pi = display_name(*parsed);
      const std::string project = get("project"), material = get("material"), grainsize = get("grainsize");
      const ps::SampleRow* found = nullptr;
      int matches = 0;
      for (const auto& s : catalog.samples) {
        if (s.name != sample) continue;
        if (!project.empty() && s.project_name != project) continue;
        if (!pi.empty() && s.principal_investigator_name != pi) continue;
        if (!material.empty() && s.material_name != material) continue;
        if (!grainsize.empty() && s.grainsize != grainsize) continue;
        found = &s;
        ++matches;
      }
      if (matches == 0) {
        out.errors.push_back(at + "no sample " + sample + " with that project, PI and material");
        continue;
      }
      if (matches > 1) {
        out.errors.push_back(at + "sample " + sample + " is ambiguous; give its project, PI and material");
        continue;
      }
      p.sample = found;
    }
    const std::string weight = get("weight");
    if (!weight.empty()) {
      const auto w = parse_double(weight);
      if (!w) {
        out.errors.push_back(at + "weight '" + weight + "' is not a number");
        continue;
      }
      p.weight = *w;
    }
    if (const std::string packet = get("packet"); !packet.empty()) {
      if (!valid_packet(packet)) {
        out.errors.push_back(at + "packet '" + packet + "' is letters then digits");
        continue;
      }
      p.packet = packet;
    }
    if (const std::string note = get("note"); !note.empty()) p.note = note;
    planned.push_back(p);
  }
  if (!out.errors.empty()) return out;
  for (const auto& p : planned) {
    LevelSheetEdit& edit = levels.at(p.level);
    edit.add_row(p.position);
    if (p.sample) edit.assign_sample({p.position}, *p.sample);
    if (p.weight) edit.set_weight(p.position, p.weight);
    if (p.packet) edit.set_packet(p.position, p.packet);
    if (p.note) edit.set_note(p.position, p.note);
    ++out.applied;
  }
  return out;
}

}  // namespace pychron::entry
