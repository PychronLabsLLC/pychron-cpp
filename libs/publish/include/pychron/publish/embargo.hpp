#pragma once

// Embargoes by name (data publishing design, section 4.2): set, lift and
// list, with a project, sample or analysis named as a person names it (a
// project name, "sample" or "sample@project", a run id), or by uuid.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/publish.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::publish {

using persistence::Uuid;
using persistence::UtcTime;

struct EmbargoRequest {
  persistence::EmbargoScopeType type = persistence::EmbargoScopeType::Sample;
  // A project name or uuid; a sample name, "name@project" or uuid; an
  // analysis run id ("70001-01", "70001-01A") or uuid.
  std::string name_or_uuid;
  std::optional<UtcTime> until;  // nullopt: until lifted by hand
  std::optional<std::string> reason;
};

// The scope a name means. Config naming the candidates when a sample name
// is in several projects, or a project name belongs to several PIs; Config
// when nothing matches.
Result<persistence::EmbargoScope> resolve_scope(persistence::IStore& store, persistence::EmbargoScopeType type,
                                                std::string_view name_or_uuid);

Result<Uuid> set_embargo(persistence::IStore& store, const persistence::Actor& actor, const EmbargoRequest& request);
// Config when the embargo is unknown or already lifted.
Result<void> lift_embargo(persistence::IStore& store, const persistence::Actor& actor, std::string_view embargo_uuid);
// Lifts every live embargo on the scope; returns how many.
Result<std::size_t> lift_embargoes_on(persistence::IStore& store, const persistence::Actor& actor,
                                      persistence::EmbargoScopeType type, std::string_view name_or_uuid);

struct EmbargoListing {
  persistence::EmbargoRow row;
  std::string scope_name;  // "project Alpha", "sample FC-2 (Alpha)", "analysis 70001-01"
};
Result<std::vector<EmbargoListing>> list_embargoes(persistence::IStore& store, bool live_only, UtcTime at);

// "70001-01A" -> identifier, aliquot, increment (-1 without a step letter).
struct RunId {
  std::string identifier;
  int aliquot = 0;
  int increment = -1;
  friend bool operator==(const RunId&, const RunId&) = default;
};
std::optional<RunId> parse_runid(std::string_view text);

}  // namespace pychron::publish
