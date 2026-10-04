#pragma once

// "Membership only": the analysis provenance row of a source that did not
// create the analysis but only made it a member of its repository
// (BatchWriter, stage_membership). The writer marks the row with a top-level
// flag in its detail; everyone who asks reads it here, as JSON, so that the
// same words elsewhere in a detail are not taken for the flag.

#include <string>

#include <nlohmann/json.hpp>

#include "pychron/persistence/import.hpp"

namespace pychron::ingest::detail {

inline constexpr char kMembershipOnly[] = "membership_only";

// The detail stage_membership writes.
inline std::string membership_only_detail() { return nlohmann::json{{kMembershipOnly, true}}.dump(); }

inline bool is_membership_only(const persistence::ProvenanceRow& row) {
  if (!row.detail_json) return false;
  const auto parsed = nlohmann::json::parse(*row.detail_json, nullptr, false);
  if (!parsed.is_object()) return false;
  const auto flag = parsed.find(kMembershipOnly);
  return flag != parsed.end() && flag->is_boolean() && flag->get<bool>();
}

}  // namespace pychron::ingest::detail
