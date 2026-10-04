#pragma once

// A replay never writes behind a stored revision (legacy ingestion spec,
// section 10, item 31). A replay walks a source again from its first commit
// and meets revisions an earlier run refused. One that a later commit of the
// same source has since been stored over, for the same subject and kind, is
// late: written now it would become the head, with content older than what
// the head holds. It is recorded as a conflict instead.

#include <set>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/ingest/batch.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::ingest::detail {

inline constexpr std::string_view kLateRevisionNotApplied = "late_revision_not_applied";

// Where one replay has got to. The adapter sends revisions in walk order and a
// replay starts at the first commit, so when a revision arrives, every
// revision of an earlier commit has been passed, whatever the batches. A
// stored revision of this source that has not been passed therefore lies
// later in the walk.
//
// The state is the walk's own: it does not depend on how the walk is cut into
// batches, and a replay that was stopped starts again with none.
class ReplayOrder {
 public:
  void clear() { passed_.clear(); }

  // The walk has reached this revision, stored or not.
  void pass(persistence::Uuid revision) { passed_.insert(revision); }

  // Whether (subject, kind) holds a revision that `source` imported and this
  // walk has not reached. Revisions without a provenance row of `source` (made
  // by a user, or imported from another source) have no place in this walk
  // and are not counted.
  Result<bool> behind_stored(persistence::IStore& store, persistence::Uuid source, persistence::Uuid subject,
                             persistence::Kind kind) const;

 private:
  std::set<persistence::Uuid> passed_;
};

// The detail of the conflict of a late revision: the reason, the commit, path
// and kind, the git blob sha, and under "content" what the revision held. A
// content larger than 64 KiB is left out; the blob sha names it.
std::string late_revision_detail(const RevisionItem& revision);

// Whether a conflict's detail is one late_revision_detail() made.
bool is_late_revision_detail(std::string_view detail_json);

}  // namespace pychron::ingest::detail
