#pragma once

// Bulk sample entry from CSV or pasted spreadsheet rows (sample and package
// entry spec, sections 6 and 9.2). Planning is pure: it reads a snapshot of
// the catalog and returns, per row, what an import would do. The plan becomes
// one catalog edit batch.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/entry/csv.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::entry {

enum class ImportField {
  Sample,
  Project,
  PrincipalInvestigator,
  Material,
  Grainsize,
  Note,
  Igsn,
  Lat,
  Lon,
  Elevation,
  StorageLocation,
  Location,
  Unit,
  Lithology,
  LithologyClass,
  LithologyType,
  LithologyGroup,
  ApproximateAge,
  Easting,
  Northing,
  Zone
};

// Every field, in template order.
const std::vector<ImportField>& import_fields();
// The template header name ("sample", "principal_investigator", ...).
std::string_view field_name(ImportField field);
// The field a header names: the template name or one of its aliases
// ("latitude", "pi", "grain_size", ...), case and spaces ignored.
std::optional<ImportField> field_for_header(std::string_view header);

// Which field each column holds; nullopt: ignored.
using ColumnMapping = std::vector<std::optional<ImportField>>;
ColumnMapping default_mapping(const std::vector<std::string>& header);

// The catalog rows a plan is checked against.
struct CatalogSnapshot {
  std::vector<persistence::PrincipalInvestigatorRow> principal_investigators;
  std::vector<persistence::ProjectRow> projects;
  std::vector<persistence::MaterialRow> materials;
  std::vector<persistence::SampleRow> samples;
};
Result<CatalogSnapshot> read_snapshot(persistence::IStore& store);

enum class RowState { Create, Exists, Update, Error };
std::string_view to_string(RowState state);

struct ImportRow {
  int line = 0;
  RowState state = RowState::Error;
  std::string sample, project, principal_investigator, material, grainsize;
  persistence::SampleFields fields;     // what the row gives
  std::optional<persistence::Uuid> existing;  // the sample with this natural key
  std::vector<std::string> changed;     // Update: the columns that differ
  std::vector<std::string> messages;    // Error: every problem; otherwise notes
};

struct ImportOptions {
  bool update_existing = false;            // write Update rows; otherwise they are left alone
  std::vector<std::string> pi_names_allowed;  // lab names accepted as a PI ("NMGRL")
};

struct SampleImportPlan {
  std::vector<ImportRow> rows;
  int creates = 0, exists = 0, updates = 0, errors = 0;
  // Rows that would be created along the way, as "Last, F", "project (PI)", "material (grainsize)".
  std::vector<std::string> new_principal_investigators, new_projects, new_materials;
};

SampleImportPlan plan_sample_import(const CsvTable& table, const ColumnMapping& mapping,
                                    const CatalogSnapshot& catalog, const ImportOptions& options);

// Inserts in foreign-key order (PIs, projects, materials, samples) for every
// Create row, and updates (with the snapshot values as expected) for Update
// rows when options.update_existing. Error rows are left out.
persistence::CatalogEditBatch to_batch(const SampleImportPlan& plan, const CatalogSnapshot& catalog,
                                       const ImportOptions& options);

// The sample columns of `fields`, every one (null where the field is unset).
persistence::CatalogFields sample_columns(const persistence::SampleFields& fields);
// The inverse; columns that are not sample fields are ignored.
persistence::SampleFields sample_fields_of(const persistence::CatalogFields& columns);

// A header-only CSV with every field.
std::string template_csv();
// The Error rows as CSV: line, messages, then the row's fields.
std::string errors_csv(const SampleImportPlan& plan);

}  // namespace pychron::entry
