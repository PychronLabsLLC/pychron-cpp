#pragma once

// Nothing is written behind a stored revision (legacy ingestion spec, section
// 10, items 31, 34 and 35). The store appends: a revision written now becomes
// the head of its subject and kind. That is right only for content that comes
// after what is stored. It is not for a revision an earlier run refused, for
// commits of a branch a later merge placed earlier in the walk than commits
// already imported, or when the head was made outside this source (someone's
// edit). Such a revision is late: the head stays, and the revision is kept in
// a conflict.
//
// "After" is a place in the source's walk order as the adapter has it now
// (RevisionItem::order, ISourceAdapter::order_of), not the order in which
// revisions reached the writer.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "pychron/core/error.hpp"
#include "pychron/ingest/adapter.hpp"
#include "pychron/ingest/batch.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::ingest::detail {

inline constexpr std::string_view kLateRevisionNotApplied = "late_revision_not_applied";

// Why a revision that is not stored is not written. `behind`: the commit of
// the stored revision that decides it, when there is one.
struct Late {
  enum class Cause {
    No,                   // it is written
    HeadNotOfThisSource,  // the head was made by a user or imported from another source
    LaterRevisionStored,  // this source stored a revision from a commit later in the walk
    StoredCommitUnknown   // this source stored a revision from a commit the walk does not have
  };
  Cause cause = Cause::No;
  std::string behind = {};
  explicit operator bool() const { return cause != Cause::No; }
};

// What one run knows about the revisions stored for the subjects and kinds it
// has had to decide about, kept current with what the run writes: the store
// does not show what a batch has staged.
class StoredChains {
 public:
  // `url`: the source's, normalized. All four outlive the run.
  void begin_run(persistence::IStore& store, ISourceAdapter& adapter, persistence::Uuid source, std::string url);

  // The run sent this revision as a root of an analysis of this source: it is
  // this source's, from `commit`, whether or not its provenance is stored yet.
  Result<void> sent_root(persistence::Uuid revision, std::string_view commit);

  // Whether a revision of (subject, kind) at walk place `order` that is not
  // stored would be written behind what is stored. One history read per
  // subject and kind per run.
  Result<Late> late(persistence::Uuid subject, persistence::Kind kind, std::optional<std::int64_t> order);

  // The run writes a revision of (subject, kind) from `commit` at `order`.
  void written(persistence::Uuid subject, persistence::Kind kind, std::optional<std::int64_t> order,
               std::string_view commit);

 private:
  struct Chain {
    bool empty = true;                 // no revision is stored
    bool head_ours = false;            // the head is a revision of this source
    std::optional<std::int64_t> last;  // the latest place in the walk a revision of this source comes from
    std::string last_commit;
    std::string unknown_commit;        // of a revision of this source: not in the walk
  };
  struct Ours {
    std::optional<std::int64_t> order;
    std::string commit;
  };

  Result<Chain> load(persistence::Uuid subject, persistence::Kind kind);
  Result<std::optional<Ours>> ours(const persistence::RevisionInfo& revision);

  persistence::IStore* store_ = nullptr;
  ISourceAdapter* adapter_ = nullptr;
  persistence::Uuid source_ = {};
  std::string url_;
  std::map<std::pair<persistence::Uuid, persistence::Kind>, Chain> chains_;
  std::map<persistence::Uuid, Ours> roots_;  // see sent_root()
};

// The detail of the conflict of a late revision: the reason, "late": true
// (verify lists it as a warning), why, the commit, path and kind, the git blob
// sha, and under "content" what the revision held. A content larger than
// 64 KiB is left out; the blob sha names it.
std::string late_revision_detail(const RevisionItem& revision, const Late& late);

// Whether a conflict's detail is one late_revision_detail() made.
bool is_late_revision_detail(std::string_view detail_json);

}  // namespace pychron::ingest::detail
