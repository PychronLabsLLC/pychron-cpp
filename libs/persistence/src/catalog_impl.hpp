#pragma once

// Entry reads, catalog edits and identifier allocation (catalog.cpp), and the
// hook a unit of work offers to be written inside a catalog edit's
// transaction (entry spec 5.3).

#include <optional>
#include <string>
#include <vector>

#include "store_impl.hpp"

namespace pychron::persistence::detail {

// Revisions staged in a unit of work, written into a transaction someone
// else opened and will commit.
class StagedRefs {
 public:
  virtual ~StagedRefs() = default;
  // Changeset, revisions, payloads and CAS head moves, plus head_move rows;
  // appends the subjects to `entities`. Returns the heads that lost their CAS
  // (nothing must be committed then).
  virtual Result<std::vector<RefConflict>> write(std::vector<ChangeEntityRow>& entities) = 0;
  virtual std::optional<Uuid> changeset() const = 0;
};

// Prepares `uow` (made by make_unit_of_work) for writing as a `kind`
// changeset; it is consumed as by commit(). Null for another unit of work.
Result<StagedRefs*> prepare_staged(IUnitOfWork& uow, Db& db, ChangesetKind kind, std::string message);

Result<std::vector<PrincipalInvestigatorRow>> principal_investigators(Db& db);
Result<std::vector<ProjectRow>> projects(Db& db, std::optional<Uuid> pi);
Result<std::vector<MaterialRow>> materials(Db& db);
Result<std::vector<SampleRow>> samples(Db& db, Dialect dialect, const SampleQuery& query);
Result<std::vector<IrradiationRow>> irradiations(Db& db, Dialect dialect);
Result<std::vector<LevelRow>> levels(Db& db, Uuid irradiation);
Result<std::optional<LevelSheet>> level_sheet(Db& db, Uuid level);
Result<std::vector<RefObjectRow>> ref_objects(Db& db, RefType type, std::optional<Uuid> irradiation);
Result<std::optional<std::int64_t>> identifier_counter(Db& db, const std::string& scope);
Result<std::int64_t> max_numeric_identifier(Db& db, Dialect dialect);
Result<std::optional<CatalogFields>> catalog_row(Db& db, CatalogTable table, Uuid uuid);

Result<CatalogOutcome> apply_catalog_edits(Db& db, Uuid client, const CatalogEditBatch& batch, StagedRefs* refs);
Result<AllocationOutcome> allocate_identifiers(Db& db, Dialect dialect, Uuid client,
                                               const IdentifierAllocation& allocation);

}  // namespace pychron::persistence::detail
