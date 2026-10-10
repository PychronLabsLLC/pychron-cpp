#pragma once

// A finished AnalysisRecord as the items the DVC store ingests. Pure: no
// clock, no random id, no store access. Every id is a UUIDv5 of the analysis
// uuid and a fixed label, so converting a record twice gives the same items
// and a resend is recognised by the store as a duplicate.

#include <optional>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/record/types.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::experiment::store {

struct RunIngest {
  std::vector<persistence::IngestItem> blobs;  // one per distinct raw series, in first-use order
  persistence::IngestItem analysis;
};

// Fails (ErrorKind::Config) when: the record was not finalized (its sha is not
// 64 hex characters); its uuid or timestamp does not parse; its step is not
// letters; a series has t, v or sigma of different lengths; two series share
// a (kind, key) (the store keeps one signal ref per kind and isotope, and per
// baseline detector); or an intercept names an isotope with no series.
Result<RunIngest> to_ingest(const record::AnalysisRecord& record, persistence::Uuid client);

// "" -> -1, "A" -> 0, "Z" -> 25, "AA" -> 26; case is folded ("aB" is 27);
// anything else nullopt.
std::optional<int> increment_of(std::string_view step);

}  // namespace pychron::experiment::store
