#pragma once

// A source adapter knows one source format and never touches the store
// (legacy ingestion spec, section 2.1).

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/ingest/batch.hpp"

namespace pychron::ingest {

// How an analysis that is in the store got there, as far as one source can tell.
struct AnalysisOrigin {
  // Its collection changeset is the one this source derives for a record
  // first added at the commit asked about: this source created it from there.
  bool from_this_source = false;
  // This source created it, from that record or another one (a second copy
  // of an analysis in the same source).
  bool in_this_source = false;
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

// ---------------------------------------------------------------- accounting
//
// For `import verify` an adapter lists every unit its source holds (a file as
// one commit left it; a row of a table) and says, for each, where the import
// left it. The verifier does not take the adapter's word: it looks every
// piece of evidence up in the store, and a unit is accounted for only when
// all of it is there.

// A row the import leaves in the store. `commit` and `path` are a source key
// as in SourceKey; a multi-part file names each part "<file>#<part>".
struct Evidence {
  enum class Kind {
    // What the writer leaves for an item it is sent at (commit, path): the
    // provenance row, or, when it refused the item, the conflict
    // conflict_id(url, commit, path). With `blob_sha` set, a provenance row of
    // `path` with that blob at another commit also counts: content the walk
    // had already imported when it came to this commit.
    Recorded,
    // The conflict conflict_id(url, commit, path): the adapter refused the unit.
    Conflict,
    // A provenance row of `path` with `blob_sha`, at any commit.
    Blob,
    // A provenance row of this source for `entity` (an analysis this source
    // imported under another path).
    Entity,
    // The provenance detail of the commit's changeset lists `path` under
    // `list`: "rewrites" (entries with a "path") or "removed" (strings).
    Note,
    // The catalog row `catalog` names by natural key.
    CatalogRow
  };
  Kind kind = Kind::Recorded;
  std::string commit = {}, path = {}, blob_sha = {};
  persistence::Uuid entity = {};
  std::string list = {};
  std::optional<CatalogItem> catalog = std::nullopt;
};

enum class UnitDisposition {
  Ignored,     // not part of the import by rule (a run log; a file that holds nothing): never reported
  Imported,    // it has rows of its own
  Folded,      // its content lives in another unit's row (a satellite file in its analysis)
  Unchanged,   // the same content was imported from an earlier unit
  Conflict,    // refused; a conflict row says why
  Removed,     // a deletion; evidence only where the import records what went
  Unclassified // the adapter cannot say: reported as unaccounted
};

// Imported, Folded, Unchanged and Conflict need evidence, and all of it must
// be found. Removed needs none, but what it names must be found. Ignored is
// not looked up.
struct SourceUnit {
  std::string commit, path, blob_sha;  // blob_sha: empty for a deletion
  bool deleted = false;
  UnitDisposition disposition = UnitDisposition::Unclassified;
  std::vector<Evidence> evidence = {};
  // Set when the unit is a version of an interpreted age: the name of its
  // InterpretedAgeKey. The verifier compares the ages stored under it.
  std::string interpreted_age = {};
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
  // Walks the whole source again, read-only, and hands every unit it holds to
  // `visit`, in no particular order, each exactly once. What a unit became is
  // decided as the import decides it, so `state` is asked what plan() and
  // next_batch() would ask. It replaces any plan: call plan() again before
  // next_batch(). An error from `visit` stops the walk and is returned.
  virtual Result<void> for_each_unit(IImportState& state,
                                     const std::function<Result<void>(const SourceUnit&)>& visit) = 0;
};

}  // namespace pychron::ingest
