#pragma once

// Revision history and saved edits (data browsing and visualization design,
// section 11.3, History tab). Sources that keep revisions (the DVC store)
// implement IRevisionSource and return it from IAnalysisSource::revisions().
// A revision's content is shown as a table, so every kind diffs the same way.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/fit_edit.hpp"
#include "pychron/processing/isotope_evolution_fit.hpp"
#include "pychron/processing/reference_fit.hpp"

namespace pychron::processing {

enum class RevisionKind { Intercepts, Baselines, Blanks, IcFactors, Tags, Annotation, Signals };

inline constexpr RevisionKind kRevisionKinds[] = {RevisionKind::Intercepts, RevisionKind::Baselines,
                                                  RevisionKind::Blanks,     RevisionKind::IcFactors,
                                                  RevisionKind::Tags,       RevisionKind::Annotation,
                                                  RevisionKind::Signals};

// The store's spellings: "intercepts", "baselines", "blanks", "icfactors",
// "tags", "annotation", "signals" (also the keys of Analysis::heads).
std::string_view to_string(RevisionKind kind) noexcept;
std::optional<RevisionKind> parse_revision_kind(std::string_view text) noexcept;
std::string_view title(RevisionKind kind) noexcept;  // "Intercepts", "IC factors", ...

struct RevisionSummary {
  std::string id;
  std::string parent;  // empty for a root revision
  RevisionKind kind = RevisionKind::Intercepts;
  std::string changeset_kind;  // collection | reduction | rollback | import | ...
  std::string author, host, message;
  double created = 0.0;  // UTC epoch seconds
  std::int64_t seq = 0;  // change-log order
  bool head = false;
};

// One row per key (isotope, detector, series); cells line up with columns.
struct RevisionTable {
  struct Row {
    std::string key;
    std::vector<std::string> cells;
  };
  std::vector<std::string> columns;  // without the key column
  std::vector<Row> rows;
};

enum class DiffState { Same, Changed, Added, Removed };
std::string_view to_string(DiffState state) noexcept;

struct DiffRow {
  std::string key;
  DiffState state = DiffState::Same;
  std::vector<std::string> before, after;  // per column; empty strings where absent
  std::vector<bool> changed;               // per column
};

struct RevisionDiff {
  std::vector<std::string> columns;  // union of both tables' columns, `before`'s order first
  std::vector<DiffRow> rows;         // `before`'s order, then rows only `after` has
  int changed_rows() const;
};

RevisionDiff diff_revisions(const RevisionTable& before, const RevisionTable& after);

// Saving is a compare-and-swap on the heads the edits were made on: when
// someone else moved one first, nothing is saved and `conflict` says who.
struct SaveOutcome {
  bool saved = false;
  // The new head per kind ("intercepts", "baselines") for one analysis;
  // per analysis uuid for reference fits.
  std::map<std::string, std::string> revisions;
  std::string conflict;
};

class IRevisionSource {
 public:
  virtual ~IRevisionSource() = default;
  // Revisions of one kind of an analysis, newest first.
  virtual Result<std::vector<RevisionSummary>> history(const std::string& analysis, RevisionKind kind) = 0;
  virtual Result<RevisionTable> revision_table(const std::string& revision) = 0;
  // One changeset with a new intercepts revision (signal edits) and/or a new
  // baselines revision (baseline edits): the rows of the head in `heads`
  // (Analysis::heads, as loaded) with the edits applied, committed only if
  // every one of those heads is still current.
  virtual Result<SaveOutcome> save_fits(const std::string& analysis, const std::map<std::string, std::string>& heads,
                                        const std::vector<EditedFit>& edits, const std::string& message) = 0;
  // One changeset with a new blanks (or IC factors) revision for every
  // analysis of `fits`, each built on the head it was fitted at (rows for new
  // isotopes or detectors are added; references recorded, reviewed set);
  // nothing is written if any of those heads moved. Message: fits.message().
  virtual Result<SaveOutcome> save_reference_fits(const ReferenceFitSet& fits) = 0;
  // One changeset with a new intercepts revision for every analysis of
  // `fits` (batch isotope-evolution refits), each on the head it was
  // refitted at; nothing is written if any moved. Revisions by analysis
  // uuid; message fits.message().
  virtual Result<SaveOutcome> save_isotope_fits(const IsotopeFitSet& fits) = 0;
  // Moves the head of `kind` back to `revision` (an earlier revision of the
  // same analysis and kind) if `expected` is still the head. No revision is
  // written: the history keeps every revision, and the newer ones stay there
  // to restore again.
  virtual Result<SaveOutcome> restore_revision(const std::string& analysis, RevisionKind kind,
                                               const std::string& expected, const std::string& revision,
                                               const std::string& message) = 0;
};

}  // namespace pychron::processing
