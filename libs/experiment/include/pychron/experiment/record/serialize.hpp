#pragma once

// Deterministic AnalysisRecord serialization. Both formats are renderings of
// the same canonical table (keys sorted, arrays in record order), so equal
// records always produce byte-identical text.

#include <optional>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/experiment/record/types.hpp"

namespace pychron::experiment::record {

std::string to_toml(const AnalysisRecord& rec);
std::string to_json(const AnalysisRecord& rec);

// Fails (ErrorKind::Config) on syntax errors, wrong types, or a
// schema_version other than kRecordSchemaV2 and kRecordSchemaVersion. A v2
// record reads in v3's terms (one gas named "", intercepts keyed
// "<label>@<detector>", one baseline position a detector) and keeps
// schema_version 2, so it is written back, and its sha computed, as it was.
Result<AnalysisRecord> from_toml(std::string_view text);
Result<AnalysisRecord> from_json(std::string_view text);

// `rec` as a v2 record reads (schema_version 2, in v3's terms as above) when
// v2 can say all of it: no gas but the implicit one, every series at its
// first segment and position, no baseline position but one a detector, no
// baseline choice or modifier. nullopt when it needs v3. The record builder
// finalizes such a record as v2, so an ordinary run keeps the bytes and the
// stored items it had before v3 (as instrument.simulated and
// provenance.status did).
std::optional<AnalysisRecord> as_v2(const AnalysisRecord& rec);

// sha256 of the canonical TOML with provenance.sha blanked.
std::string compute_sha(const AnalysisRecord& rec);
bool verify_sha(const AnalysisRecord& rec);

}  // namespace pychron::experiment::record
