#pragma once

// Natural-key resolution for the BatchWriter: turns catalog items, and the
// names other items refer to catalog rows by, into store uuids.

#include <map>
#include <optional>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/ingest/batch.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::ingest::detail {

// Every lookup is an ensure-by-natural-key store call: an existing row wins
// and is not written; a missing one is created, with an id derived from its
// natural key where the store accepts one. Results are remembered for the
// resolver's lifetime (catalog rows are never deleted), so a name used by
// every item of a run costs one store call.
class CatalogResolver {
 public:
  CatalogResolver(persistence::IStore& store, persistence::Uuid client) : store_(store), client_(client) {}

  Result<void> write(const CatalogItem& item);

  Result<persistence::Uuid> user(const std::string& name);
  Result<persistence::Uuid> repository(const std::string& name);
  Result<persistence::Uuid> ref_object(const RefObjectKey& key);

  // The uuid of a reference object without touching the store: the one
  // resolved earlier, else the id this importer would create it with.
  // An unknown ref_type is an error.
  Result<persistence::Uuid> ref_object_id(const RefObjectKey& key) const;

 private:
  using Uuid = persistence::Uuid;
  struct ProjectKey {
    std::string name;
    std::optional<std::string> pi_last_name, pi_first_initial;
  };

  template <class Ensure>
  Result<Uuid> cached(const char* table, const std::string& key, Ensure&& ensure);

  Result<Uuid> principal_investigator(const PiItem& item);
  Result<Uuid> project(const ProjectKey& key);
  Result<Uuid> material(const std::string& name, const std::string& grainsize);
  Result<Uuid> sample(persistence::SampleSpec fields, const ProjectKey& project, const std::string& material,
                      const std::string& grainsize);
  Result<Uuid> irradiation(const std::string& name);
  Result<Uuid> level(const LevelItem& item);
  Result<Uuid> position(const PositionItem& item);
  Result<Uuid> mass_spectrometer(persistence::MassSpectrometerSpec spec);
  Result<Uuid> ref_object(persistence::RefType type, const std::string& key, const RefObjectItem* scope);

  persistence::IStore& store_;
  Uuid client_;
  std::map<std::string, Uuid> known_;  // "<table>\n<natural key>" -> uuid
};

}  // namespace pychron::ingest::detail
