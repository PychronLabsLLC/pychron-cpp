#include "pychron/entry/identifier_plan.hpp"

#include <algorithm>

namespace pychron::entry {

namespace ps = persistence;

ps::IdentifierAllocation IdentifierPlan::allocation() const {
  ps::IdentifierAllocation out;
  out.expected_last = expected_last;
  for (const auto& a : assignments) out.assignments.push_back(ps::IdentifierAssignment{a.position, a.number, a.replaces});
  return out;
}

IdentifierPlan plan_identifiers(std::vector<ps::LevelSheet> sheets, std::int64_t last, bool overwrite) {
  std::sort(sheets.begin(), sheets.end(), [](const ps::LevelSheet& a, const ps::LevelSheet& b) {
    return a.level.name != b.level.name ? a.level.name < b.level.name : a.level.uuid < b.level.uuid;
  });
  IdentifierPlan plan;
  plan.expected_last = last;
  std::int64_t n = last;
  for (auto& sheet : sheets) {
    std::sort(sheet.positions.begin(), sheet.positions.end(),
              [](const ps::PositionRow& a, const ps::PositionRow& b) { return a.position < b.position; });
    for (const auto& p : sheet.positions) {
      if (!p.sample) continue;
      if (p.identifier) {
        if (!overwrite || p.n_analyses > 0 || p.in_load) continue;
      }
      ++n;
      plan.assignments.push_back(PlannedIdentifier{p.uuid, sheet.level.name, p.position, p.sample_name, p.identifier, n,
                                                   p.identifier ? p.identifier_uuid : std::nullopt});
    }
  }
  plan.last = n;
  return plan;
}

std::vector<std::string> human_error_checks(const std::vector<ps::LevelSheet>& sheets, const EntrySettings& settings,
                                            const std::string& package) {
  std::vector<std::string> out;
  const std::string project = settings.irradiation_project_prefix + package;
  for (const auto& sheet : sheets) {
    bool any = false, monitor = false;
    for (const auto& p : sheet.positions) {
      if (!p.sample) continue;
      any = true;
      if (p.sample_name == settings.monitor_sample && p.material == settings.monitor_material) {
        monitor = true;
        if (p.project != project)
          out.push_back("level " + sheet.level.name + " position " + std::to_string(p.position) + ": monitor " +
                        p.sample_name + " is in project " + p.project + ", not " + project);
      }
    }
    if (any && !monitor) out.push_back("level " + sheet.level.name + " has no monitor (" + settings.monitor_sample + ")");
  }
  return out;
}

Result<std::int64_t> current_last(ps::IStore& store) {
  auto counter = store.identifier_counter(std::string(ps::kIdentifierScope));
  if (!counter) return fail(counter.error());
  if (*counter) return **counter;
  return store.max_numeric_identifier();
}

Result<std::vector<ps::LevelSheet>> package_sheets(ps::IStore& store, ps::Uuid package) {
  auto levels = store.levels(package);
  if (!levels) return fail(levels.error());
  std::vector<ps::LevelSheet> out;
  for (const auto& l : *levels) {
    auto sheet = store.level_sheet(l.uuid);
    if (!sheet) return fail(sheet.error());
    if (*sheet) out.push_back(std::move(**sheet));
  }
  return out;
}

}  // namespace pychron::entry
