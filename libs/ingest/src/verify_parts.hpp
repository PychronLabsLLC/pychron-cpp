#pragma once

// The parts of `import verify` (verify.hpp), one source file each: accounting
// (verify_accounting.cpp, catalog_find.cpp), age parity (verify_parity.cpp),
// and what ties them together (verify.cpp).

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/ingest/adapter.hpp"
#include "pychron/ingest/verify.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::ingest::detail {

// The source being verified.
struct VerifySource {
  persistence::IStore& store;
  persistence::Uuid uuid;
  std::string url;  // normalized
};

// Whether the catalog row an item names by natural key is in the store. It
// resolves the item's parents as CatalogResolver does, and creates nothing.
// An InterpretedAgeItem has no natural key: an error.
Result<bool> catalog_row_exists(persistence::IStore& store, const CatalogItem& item);

// Looks the evidence of each unit up in the store.
class Accountant {
 public:
  explicit Accountant(const VerifySource& source) : source_(source) {}

  // Counts the unit and, when a row it names is not there, lists it.
  Result<void> check(const SourceUnit& unit, VerifyReport& report);

 private:
  Result<bool> found(const Evidence& evidence);
  Result<bool> conflict_stored(const std::string& commit, const std::string& path);
  Result<bool> analysis_stored(persistence::Uuid analysis);
  Result<bool> noted(const Evidence& evidence);

  const VerifySource& source_;
  // The lists in the provenance detail of the changeset last asked about.
  std::optional<std::string> noted_commit_;
  std::map<std::string, std::set<std::string>> notes_;
};

// Compares the member ages of the latest revision of each interpreted age
// (named by InterpretedAgeKey) and records the outcome: a value_mismatch
// conflict per failure, `superseded` on one whose comparison now passes.
Result<void> check_parity(const VerifySource& source, persistence::Uuid client,
                          const std::set<std::string>& interpreted_ages, const AgeFn& age_fn,
                          const VerifyOptions& options, VerifyReport& report);

}  // namespace pychron::ingest::detail
