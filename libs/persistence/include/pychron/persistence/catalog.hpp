#pragma once

// Catalog reads and edits for sample and package entry
// (2026-10-04-sample-irradiation-entry-design.md, section 5). Std-only.
//
// Catalog rows are not revisioned (DVC schema spec D6). An edit carries the
// values it was made against and the store compares them field by field
// (E2); a batch is one transaction (E3).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "pychron/persistence/ids.hpp"
#include "pychron/persistence/model.hpp"

namespace pychron::persistence {

// The catalog tables that have a natural key, for IStore::find_catalog_row,
// and the tables a catalog edit may name. The key of each, in the order its
// parts are given:
//   PrincipalInvestigator  last_name, first_initial
//   Project                name, principal investigator (uuid, or none)
//   Material               name, grainsize
//   Sample                 name, project (uuid), material (uuid)
//   Irradiation            name
//   Level                  irradiation (uuid), name
//   IrradiationPosition    level (uuid), position
//   User                   name
//   MassSpectrometer       name
//   ExtractDevice          name
//   Load                   name
//   LoadPosition           load (uuid), position, identifier (uuid)
//   Repository             name
//   RefObject              ref_type (stored spelling), key
//   Identifier             identifier
enum class CatalogTable {
  PrincipalInvestigator,
  Project,
  Material,
  Sample,
  Irradiation,
  Level,
  IrradiationPosition,
  User,
  MassSpectrometer,
  ExtractDevice,
  Load,
  LoadPosition,
  Repository,
  RefObject,
  Identifier
};

// The table's name in the schema ("principal_investigator", ...).
std::string_view table_name(CatalogTable table) noexcept;

// One part of a natural key: text, a number, the uuid of a parent row, or
// (monostate) no value, which matches a row that has none there.
using CatalogKeyPart = std::variant<std::monostate, std::string, int, Uuid>;

// ---------------------------------------------------------------- reads (5.1)

struct PrincipalInvestigatorRow {
  Uuid uuid;
  std::string last_name, first_initial;
  std::string display_name;  // "Last, F" or "Last"
  std::optional<std::string> affiliation, email;
  friend bool operator==(const PrincipalInvestigatorRow&, const PrincipalInvestigatorRow&) = default;
};

struct ProjectRow {
  Uuid uuid;
  std::string name;
  std::optional<Uuid> principal_investigator;
  std::string principal_investigator_name;  // display name; empty without a PI
  std::optional<std::string> checkin_date, comment, lab_contact, institution;
  int n_samples = 0;
  friend bool operator==(const ProjectRow&, const ProjectRow&) = default;
};

struct MaterialRow {
  Uuid uuid;
  std::string name, grainsize;
  int n_samples = 0;
  friend bool operator==(const MaterialRow&, const MaterialRow&) = default;
};

// The descriptive columns of a sample (everything but its key).
struct SampleFields {
  std::optional<std::string> note, igsn;
  std::optional<double> lat, lon, elevation;
  std::optional<std::string> storage_location, location, unit;
  std::optional<std::string> lithology, lithology_class, lithology_type, lithology_group;
  std::optional<double> approximate_age;
  friend bool operator==(const SampleFields&, const SampleFields&) = default;
};

struct SampleRow {
  Uuid uuid;
  std::string name;
  Uuid project, material;
  std::optional<Uuid> principal_investigator;
  std::string project_name, principal_investigator_name, material_name, grainsize;
  SampleFields fields;
  UtcTime updated;
  int n_positions = 0, n_analyses = 0;
  friend bool operator==(const SampleRow&, const SampleRow&) = default;
};

struct SampleQuery {
  std::string text;  // case-insensitive substring of the name; empty: any
  std::optional<Uuid> principal_investigator, project, material;
  int limit = 500;
};

// A package (table `irradiation`, E13).
struct IrradiationRow {
  Uuid uuid;
  std::string name;
  std::string kind;  // irradiation | package
  UtcTime created;
  int n_levels = 0, n_positions = 0, n_analyzed = 0;  // n_analyzed: positions whose identifier has analyses
  bool has_chronology = false;
  friend bool operator==(const IrradiationRow&, const IrradiationRow&) = default;
};

struct LevelRow {
  Uuid uuid;
  Uuid irradiation;
  std::string name;
  std::optional<Uuid> holder;  // ref_object of type irradiation_holder
  std::optional<std::string> holder_name;
  std::optional<std::string> note;
  friend bool operator==(const LevelRow&, const LevelRow&) = default;
};

struct PositionRow {
  Uuid uuid;
  int position = 0;
  std::optional<Uuid> sample;
  std::string sample_name, project, principal_investigator, material, grainsize;
  std::optional<double> weight;
  std::optional<std::string> packet, note;
  std::optional<Uuid> identifier_uuid;
  std::optional<std::string> identifier;
  int n_analyses = 0;
  bool in_load = false;
  std::optional<double> j, j_err;  // head of the position's flux_position reference, read-only
  friend bool operator==(const PositionRow&, const PositionRow&) = default;
};

// The head of one reference object.
struct RefHead {
  Uuid ref_object;
  std::optional<Uuid> revision;  // nullopt: the object has no value yet
  friend bool operator==(const RefHead&, const RefHead&) = default;
};

// A reference object and its head (entry reads of productions, holders).
struct RefObjectRow {
  Uuid uuid;
  RefType type = RefType::Document;
  std::string key;
  std::optional<Uuid> irradiation, level;
  std::optional<Uuid> head;  // nullopt: no value yet
  friend bool operator==(const RefObjectRow&, const RefObjectRow&) = default;
};

struct LevelSheet {
  LevelRow level;
  std::string irradiation_name;
  std::string irradiation_kind;
  std::vector<PositionRow> positions;  // by position
  std::optional<RefHead> geometry, production;  // level_geometry, level_production objects
  std::optional<LevelZValue> z;
  std::optional<LevelProductionValue> production_value;
  friend bool operator==(const LevelSheet&, const LevelSheet&) = default;
};

// ---------------------------------------------------------------- edits (5.2)

using CatalogValue = std::variant<std::monostate, std::string, double, std::int64_t, bool, Uuid>;
using CatalogFields = std::map<std::string, CatalogValue>;  // column name -> value

struct CatalogInsert {
  CatalogTable table = CatalogTable::Sample;
  Uuid uuid;
  CatalogFields values;
};
struct CatalogUpdate {
  CatalogTable table = CatalogTable::Sample;
  Uuid uuid;
  CatalogFields expected, values;  // expected: the values the edit was made against
};
struct CatalogDelete {
  CatalogTable table = CatalogTable::Sample;
  Uuid uuid;
  CatalogFields expected;
};
using CatalogEdit = std::variant<CatalogInsert, CatalogUpdate, CatalogDelete>;

struct CatalogEditBatch {
  std::vector<CatalogEdit> edits;  // applied in order; later edits may name earlier inserts
  bool allow_analyzed_sample_change = false;  // E5
  std::string message;
};

struct StaleRow {
  CatalogTable table = CatalogTable::Sample;
  Uuid uuid;
  CatalogFields expected, actual;  // actual empty: the row is gone
  friend bool operator==(const StaleRow&, const StaleRow&) = default;
};

// Rules (section 5.2): unique, constraint, analyzed_identifier,
// analyzed_sample_change, position_has_identifier, in_use, analyzed_rename.
struct Refusal {
  CatalogTable table = CatalogTable::Sample;
  Uuid uuid;
  std::string rule;
  std::string what;
  friend bool operator==(const Refusal&, const Refusal&) = default;
};

struct CatalogApplied {
  ChangeSeq seq = 0;  // 0: the batch was empty and nothing was written
};

// A lost compare-and-swap on a reference head during a combined save (5.3).
struct RefConflict {
  Uuid subject;
  std::optional<Uuid> expected, actual;
  friend bool operator==(const RefConflict&, const RefConflict&) = default;
};

using CatalogOutcome =
    std::variant<CatalogApplied, std::vector<StaleRow>, std::vector<Refusal>, std::vector<RefConflict>>;

// ---------------------------------------------------------------- identifiers (5.4)

struct IdentifierAssignment {
  Uuid position;
  std::int64_t number = 0;
  std::optional<Uuid> replaces;  // the position's current identifier, rewritten in place
  friend bool operator==(const IdentifierAssignment&, const IdentifierAssignment&) = default;
};

struct IdentifierAllocation {
  std::int64_t expected_last = 0;  // the counter value the plan was made from
  std::vector<IdentifierAssignment> assignments;
};

struct AllocationStale {
  std::int64_t actual_last = 0;
};

using AllocationOutcome = std::variant<CatalogApplied, AllocationStale, std::vector<Refusal>>;

// The identifier_counter scope entry uses.
inline constexpr std::string_view kIdentifierScope = "identifier";

}  // namespace pychron::persistence
