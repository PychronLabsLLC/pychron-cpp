#pragma once

// A new package with its levels, chronology and production (sample and
// package entry spec, sections 6 and 9.3), written in one transaction, and
// the package-level edits made afterwards.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/entry/settings.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::entry {

struct NewLevel {
  std::string name;
  std::optional<persistence::Uuid> holder;  // an irradiation_holder reference
  std::optional<double> z;
  std::optional<std::string> note;
};

struct NewPackage {
  std::string name;
  std::string kind = "irradiation";  // irradiation | package
  // Kind irradiation only:
  std::vector<persistence::Dose> doses;
  std::optional<std::string> reactor;                 // its production is copied in as "<name>/<reactor>"
  std::optional<persistence::ProductionValue> production;  // the reactor's ratios
  std::vector<NewLevel> levels;
};

// Every problem with the package as entered; empty when it can be saved.
// Names: valid_package_name, unique among `existing`; levels: unique, not
// empty; doses: power > 0, end after start, in time order without overlap;
// kind irradiation needs a reactor and at least one dose.
std::vector<std::string> validate(const NewPackage& package, const std::vector<std::string>& existing);

struct CreatedPackage {
  persistence::Uuid package;
  std::vector<persistence::Uuid> levels;
  persistence::ChangeSeq seq = 0;
};

// The package, its levels and, for kind irradiation, the chronology, the
// production and each level's production assignment and z, in one changeset.
// Refusals and stale rows are returned as errors (a new package has nothing
// stale; a refusal names a clash).
Result<CreatedPackage> create_package(persistence::IStore& store, const persistence::Actor& actor,
                                      const NewPackage& package);

// Hours of dose and the J the legacy entry estimated from them
// (hours x j_multiplier; labnumber_entry.py:1163-1177). Display only.
double dose_hours(const std::vector<persistence::Dose>& doses);
double estimated_j(const std::vector<persistence::Dose>& doses, const EntrySettings& settings);

// The reactor defaults document ("reactors.json": {"Triga": {"K4039":
// [v, e], ...}}); keys that are not [value, error] pairs are ignored.
Result<std::map<std::string, persistence::ProductionValue>> parse_reactors(std::string_view json);
Result<std::map<std::string, persistence::ProductionValue>> load_reactors(persistence::IStore& store);

// The productions of a package (references "<package>/<name>"), by name.
struct NamedProduction {
  persistence::Uuid ref_object;
  std::string name;
  std::optional<persistence::Uuid> head;
  persistence::ProductionValue value;
};
Result<std::vector<NamedProduction>> package_productions(persistence::IStore& store, persistence::Uuid package,
                                                         const std::string& package_name);

// The chronology of a package, with its reference head; empty when none.
struct PackageChronology {
  std::optional<persistence::Uuid> ref_object, head;
  persistence::ChronologyValue value;
};
Result<PackageChronology> package_chronology(persistence::IStore& store, persistence::Uuid package,
                                             const std::string& package_name);

// The interference ratios, in the order the production editor shows them.
const std::vector<std::string>& production_keys();

}  // namespace pychron::entry
