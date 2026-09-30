#pragma once

// Deterministic AnalysisRecord serialization. Both formats are renderings of
// the same canonical table (keys sorted, arrays in record order), so equal
// records always produce byte-identical text.

#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/experiment/record/types.hpp"

namespace pychron::experiment::record {

std::string to_toml(const AnalysisRecord& rec);
std::string to_json(const AnalysisRecord& rec);

// Fails (ErrorKind::Config) on syntax errors, wrong types, or a
// schema_version other than kRecordSchemaVersion.
Result<AnalysisRecord> from_toml(std::string_view text);
Result<AnalysisRecord> from_json(std::string_view text);

// sha256 of the canonical TOML with provenance.sha blanked.
std::string compute_sha(const AnalysisRecord& rec);
bool verify_sha(const AnalysisRecord& rec);

}  // namespace pychron::experiment::record
