#pragma once

// A source adapter knows one source format and never touches the store
// (legacy ingestion spec, section 2.1).

#include <optional>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/ingest/batch.hpp"

namespace pychron::ingest {

// How an analysis that is in the store got there, as far as one source can tell.
struct AnalysisOrigin {
  // Its collection changeset is the one this source derives for a record
  // first added at the commit asked about: this source created it from there.
  bool from_this_source = false;
  // The git blob sha of the record it was created from; empty when it was not
  // imported (or its provenance is not written yet).
  std::string record_blob_sha;
};

// A read-only view of what is already imported, given to adapters.
struct IImportState {
  virtual ~IImportState() = default;
  // The git blob sha recorded for the head revision of (subject, kind) from
  // this source; nullopt when the head is not an import from it.
  virtual Result<std::optional<std::string>> head_blob_sha(const SubjectRef& subject, persistence::Kind kind) = 0;
  virtual Result<bool> analysis_exists(persistence::Uuid analysis) = 0;
  // nullopt: the analysis is not in the store. `record_commit`: the commit
  // that first adds its record in this source.
  virtual Result<std::optional<AnalysisOrigin>> analysis_origin(persistence::Uuid analysis,
                                                                std::string_view record_commit) = 0;
  // Whether this source has recorded the file at (commit, path): a provenance
  // row exists for it.
  virtual Result<bool> imported(std::string_view commit, std::string_view path) = 0;
  // The analysis that has this run identity now, from any source (increment
  // -1: no step). An adapter asks before it sends an analysis: two analyses
  // cannot share a run id.
  virtual Result<std::optional<persistence::Uuid>> analysis_with_runid(const std::string& identifier, int aliquot,
                                                                       int increment) = 0;
  // The identifier that sits at an irradiation position; nullopt: none. A
  // position holds one identifier.
  virtual Result<std::optional<std::string>> identifier_at(const std::string& irradiation, const std::string& level,
                                                           int position) = 0;
};

struct SourceDescription {
  persistence::ImportSourceKind kind = persistence::ImportSourceKind::ProjectRepo;
  std::string url;     // as given; the writer normalizes it
  std::string branch;  // empty: none
  std::string head;
};

class ISourceAdapter {
 public:
  virtual ~ISourceAdapter() = default;
  virtual Result<SourceDescription> describe() = 0;
  // Positions the stream after `resume_token` (nullopt: at the start) and
  // returns the units remaining. `state` outlives the adapter's batches.
  virtual Result<int> plan(std::optional<std::string> resume_token, IImportState& state) = 0;
  // nullopt: end of stream.
  virtual Result<std::optional<ImportBatch>> next_batch() = 0;
};

}  // namespace pychron::ingest
