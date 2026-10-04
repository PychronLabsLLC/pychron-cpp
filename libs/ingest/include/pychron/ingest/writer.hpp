#pragma once

// Writes import batches to the store (legacy ingestion spec, section 2.1).
// A batch is written in two steps: first the idempotent writes (catalog rows,
// blobs, analyses, repository membership), then one transaction holding the
// changesets, revisions, provenance, conflicts and the progress token. Ids
// are derived from source keys, so a batch written again changes nothing.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/ingest/adapter.hpp"
#include "pychron/ingest/batch.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::ingest {

struct WriterConfig {
  std::string importer_version;
  std::string lab_time_zone;                      // IANA; recorded on the import source
  std::map<std::string, std::string> author_map;  // git email -> app_user name
  // Write nothing; count in RunStats::would_write what a real run would add.
  bool dry_run = false;
  // Walk the whole source again instead of resuming from the stored token.
  // Everything already imported is a no-op; analyses refused earlier for a
  // missing catalog row, and their later revisions, are written in order and
  // their conflicts become `superseded`. The stored token never moves back.
  // A revision refused earlier is not written when a later commit of this
  // source has since stored a revision of the same subject and kind: the head
  // stays and the revision is a pending identity_clash conflict with reason
  // `late_revision_not_applied` and its content in the detail (spec 10.31).
  bool replay = false;
};

// Items handled in this run, whether or not they were already stored.
struct RunStats {
  int batches = 0;
  int analyses = 0;    // ingested or already present
  int changesets = 0;  // import and reference changesets, not collections
  int revisions = 0;   // of those changesets
  int conflicts = 0;   // from the adapter and from the writer; a conflict stored as resolved is not counted
  // Dry run only: analyses, blobs, memberships, changesets, revisions,
  // conflicts and bookmarks that are not in the store. An analysis or revision
  // with a pending unknown_analysis conflict is recorded, not missing. Catalog
  // rows are not counted: the store has no read that could tell.
  int would_write = 0;
  bool finished = false;  // the adapter reached the end of its stream
};

class BatchWriter {
 public:
  // `store` must outlive the writer. `client` is the registered importer client.
  BatchWriter(persistence::IStore& store, persistence::Uuid client, WriterConfig config);
  ~BatchWriter();
  BatchWriter(const BatchWriter&) = delete;
  BatchWriter& operator=(const BatchWriter&) = delete;

  // Registers the source the adapter describes, or finds it, and returns the
  // stored row with its resume token. A dry run registers nothing.
  Result<persistence::ImportSourceInfo> open(ISourceAdapter& adapter);

  // Plans the adapter from the stored token (from the start with
  // WriterConfig::replay) and writes its batches. Stops,
  // leaving the source `paused`, once `max_batches` are written or
  // `keep_going` returns false after a batch; `finished` at end of stream;
  // `failed` on an error, which is returned. `on_batch` runs after each batch
  // is committed. Either function may be empty. Opens the source if needed.
  Result<RunStats> run(ISourceAdapter& adapter, std::optional<int> max_batches,
                       const std::function<bool()>& keep_going,
                       const std::function<void(const RunStats&, const ImportBatch&)>& on_batch);

  // Valid for the writer's lifetime; reads fail until a source is open.
  IImportState& state();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pychron::ingest
