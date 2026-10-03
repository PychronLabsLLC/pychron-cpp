// The units of a project repository for the verifier: what each file of each
// commit became (project_import.hpp, UnitAccount).

#include <algorithm>
#include <utility>

#include "project_import.hpp"

namespace pychron::dvc::detail {

using ingest::Evidence;
using ingest::SourceUnit;
using ingest::UnitDisposition;

Result<void> UnitAccount::repeat(SourceUnit unit, const Last& last) {
  // Nothing new: the unit is whatever the one it repeats is.
  const bool plain = last.disposition == UnitDisposition::Ignored || last.disposition == UnitDisposition::Unclassified;
  unit.disposition = plain ? last.disposition : UnitDisposition::Unchanged;
  unit.evidence = last.evidence;
  unit.repeats = last.commit;
  return visit_(unit);
}

Result<void> UnitAccount::close(Open& open) {
  if (auto r = visit_(open.unit); !r) return r;
  Last settled{open.unit.commit, true, open.unit.disposition, open.unit.evidence};
  for (auto& unit : open.repeats)
    if (auto r = repeat(std::move(unit), settled); !r) return r;
  // Later repeats of the path are settled as this unit, unless the path has
  // been handed on again since.
  if (const auto last = last_.find(open.unit.path); last != last_.end() && last->second.commit == open.unit.commit)
    last->second = std::move(settled);
  return {};
}

Result<void> UnitAccount::settle(Ledger& ledger, const ingest::ImportBatch& batch) {
  // The changes the walk was given, in walk order.
  for (auto& change : ledger.changes) {
    SourceUnit unit;
    unit.commit = std::move(change.commit);
    unit.path = std::move(change.path);
    unit.blob_sha = std::move(change.blob_sha);
    switch (change.seen) {
      case Ledger::Seen::Ignored:
        unit.disposition = UnitDisposition::Ignored;
        if (auto r = visit_(unit); !r) return r;
        break;
      case Ledger::Seen::Deleted:
        // A deleted analysis file takes nothing away: the analysis stays.
        unit.deleted = true;
        unit.blob_sha.clear();
        unit.disposition = UnitDisposition::Removed;
        if (auto r = visit_(unit); !r) return r;
        break;
      case Ledger::Seen::Repeated: {
        const auto last = last_.find(unit.path);
        if (last == last_.end()) {
          if (auto r = visit_(unit); !r) return r;  // unclassified: nothing it could repeat
        } else if (const auto waiting = open_.find({last->second.commit, unit.path}); waiting != open_.end()) {
          waiting->second.repeats.push_back(std::move(unit));
        } else if (auto r = repeat(std::move(unit), last->second); !r) {
          return r;
        }
        break;
      }
      case Ledger::Seen::Taken: {
        if (classify_path(unit.path).kind == FileKind::InterpretedAge) unit.interpreted_age = unit.path;
        Last& last = last_[unit.path];
        last = Last{unit.commit, false, UnitDisposition::Unclassified, {}};
        Key key{unit.commit, unit.path};
        open_.insert_or_assign(std::move(key), Open{std::move(unit), {}, false, false});
        break;
      }
    }
  }
  ledger.changes.clear();

  // What the batch leaves for each file. A key that names no open unit is not
  // a file of the walk (the conflict of a made-up catalog row).
  std::vector<Open*> touched;
  const auto add = [&](const std::string& commit, const std::string& path, Evidence evidence) -> Open* {
    const auto found = open_.find({commit, path});
    if (found == open_.end()) return nullptr;
    Open& open = found->second;
    open.unit.evidence.push_back(std::move(evidence));
    if (!open.touched) {
      open.touched = true;
      touched.push_back(&open);
    }
    return &open;
  };
  // Every row is looked for at the file's own commit and path.
  const auto revision_at = [&](const ingest::SourceKey& key) {
    if (!key.path.empty()) add(key.commit, key.path, {Evidence::Kind::Revision, key.commit, key.path});
  };
  const auto analysis_at = [&](const ingest::SourceKey& key, persistence::Uuid analysis) {
    add(key.commit, key.path, {Evidence::Kind::Analysis, key.commit, key.path, analysis});
  };
  for (const auto& item : batch.analyses) {
    analysis_at(item.keys.record, item.ingest.analysis);
    for (const ingest::SourceKey* root : {&item.keys.signals, &item.keys.intercepts, &item.keys.baselines,
                                          &item.keys.blanks, &item.keys.icfactors, &item.keys.tags})
      revision_at(*root);
  }
  for (const auto& item : batch.memberships) analysis_at(item.key, item.analysis);
  for (const auto& changeset : batch.changesets) {
    for (const auto& revision : changeset.revisions) revision_at(revision.key);  // an identity revision: at its record
    for (const auto& note : changeset.rewrites)
      add(changeset.commit, note.path, {Evidence::Kind::Note, changeset.commit, note.path, {}, "rewrites"});
  }
  for (const auto& conflict : batch.conflicts)
    if (!conflict.key.commit.empty())
      add(conflict.key.commit, conflict.key.path, {Evidence::Kind::Conflict, conflict.key.commit, conflict.key.path});
  for (auto& entry : ledger.silent) {
    // A spectrometer file is named by many analyses: it is settled by the
    // first that uses it, wherever the batches are cut.
    if (const auto found = open_.find({entry.commit, entry.path}); found != open_.end() && found->second.folded)
      continue;
    if (Open* open = add(entry.commit, entry.path, std::move(entry.evidence))) {
      open->folded = true;
      open->unit.disposition = entry.disposition;
    }
  }
  ledger.silent.clear();

  for (Open* open : touched) {
    SourceUnit& unit = open->unit;
    const bool refused = std::all_of(unit.evidence.begin(), unit.evidence.end(),
                                     [](const Evidence& e) { return e.kind == Evidence::Kind::Conflict; });
    if (refused)
      unit.disposition = UnitDisposition::Conflict;
    else if (!open->folded)
      unit.disposition = UnitDisposition::Imported;
    if (auto r = close(*open); !r) return r;
  }
  for (const Open* open : touched) open_.erase({open->unit.commit, open->unit.path});
  return {};
}

Result<void> UnitAccount::finish() {
  for (auto& [key, open] : open_) {
    open.unit.disposition = classify_path(open.unit.path).kind == FileKind::Spectrometer
                                ? UnitDisposition::Ignored
                                : UnitDisposition::Unclassified;
    if (auto r = close(open); !r) return r;
  }
  open_.clear();
  return {};
}

}  // namespace pychron::dvc::detail
