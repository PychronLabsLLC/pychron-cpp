#pragma once

// A source adapter knows one source format and never touches the store
// (legacy ingestion spec, section 2.1).

#include <optional>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/ingest/batch.hpp"

namespace pychron::ingest {

// A read-only view of what is already imported, given to adapters.
struct IImportState {
  virtual ~IImportState() = default;
  // The git blob sha recorded for the head revision of (subject, kind) from
  // this source; nullopt when the head is not an import from it.
  virtual Result<std::optional<std::string>> head_blob_sha(const SubjectRef& subject, persistence::Kind kind) = 0;
  virtual Result<bool> analysis_exists(persistence::Uuid analysis) = 0;
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
