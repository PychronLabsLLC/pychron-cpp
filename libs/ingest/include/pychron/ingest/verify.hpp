#pragma once

// `import verify` (legacy ingestion spec, sections 6 and 10): whether an
// import of one source can be trusted. It reports
//
//   the import   the source is registered, its last run finished, and it has
//                not moved since;
//   accounting   every unit of the source is imported or explained: each
//                piece of evidence the adapter names (adapter.hpp) is looked
//                up in the store;
//   idempotence  running the import again, from the stored token and from the
//                start, would write nothing;
//   conflicts    what the import left pending: those that mean data is
//                missing or disagrees, and those that only annotate a row
//                that was imported;
//   age parity   the ages the legacy system stored with each interpreted age
//                against ages computed from the imported data as it stood
//                when that interpreted age was saved.
//
// Verify writes nothing but the outcome of the parity check: a
// `value_mismatch` conflict per failed comparison, and `superseded` on such a
// conflict once its comparison passes. The resume token and the source's
// status are left as they are. The report does not depend on how the import,
// or this walk, was cut into batches.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/ingest/adapter.hpp"
#include "pychron/ingest/writer.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::ingest {

// The point a legacy age was computed at: the commit that saved its
// interpreted age. It is a place in the walk of the source, not a time: git
// author dates tie and run out of order (spec 10.6 and 10.30). An age
// function reduces the analysis from, for each kind, the last revision whose
// source commit is at or before `commit` in the walk order of `source`, with
// the reference data of that point.
struct AsOf {
  persistence::Uuid interpreted_age;  // the interpreted age the legacy age is stored with
  persistence::Uuid revision;         // its head revision, which holds the age
  persistence::Uuid changeset;        // the changeset that stored that revision
  persistence::Uuid source;           // the import source the revision came from
  std::string commit;                 // the source commit of that revision, from its provenance row
  persistence::UtcTime created;       // the changeset's time (the commit's author date): information only
};

struct ComputedAge {
  double age = 0;
  double age_err = 0;  // analytical: without the error of J
  // With the error of J; nullopt: not computed. Compared only where the
  // legacy file says its per-analysis errors include J.
  std::optional<double> age_err_w_j = std::nullopt;
  // What the age was computed with (for example the constants preset), in
  // free form. Copied into the report and the conflict of a failure.
  std::string basis = {};
};
// The state at `AsOf` cannot be reproduced, or the analysis cannot be reduced
// (no J, no blank): never a pass, never a failure. `reason` is tallied as
// given, so keep it free of names and numbers.
struct NotComparable {
  std::string reason;
};
using ParityAge = std::variant<ComputedAge, NotComparable>;
using AgeFn = std::function<Result<ParityAge>(persistence::Uuid analysis, const AsOf& as_of)>;

struct VerifyOptions {
  // Relative: |a - b| / max(|a|, |b|), on the age and on its error.
  double tolerance = 1e-9;
};

struct UnaccountedUnit {
  SourceUnit unit;
  // What the store does not have. Empty: the unit names no row (the adapter
  // could not classify it, or said Folded or Unchanged without pointing at one).
  std::vector<Evidence> missing;
};

struct ParityFailure {
  persistence::Uuid analysis, interpreted_age;
  persistence::Uuid conflict;  // the value_mismatch row
  double legacy_age = 0, computed_age = 0;
  // The legacy error that was compared, and the computed one of the same
  // kind (see `error_compared`). nullopt: the error was not compared.
  std::optional<double> legacy_age_err;
  double computed_age_err = 0;
  double age_difference = 0, age_err_difference = 0;  // relative
  // "age_err_wo_j": the member's own error without J; "age_err": its age_err,
  // which the file says is without J; "age_err_w_j": its age_err, which the
  // file says includes J. Empty: the error was not compared.
  std::string error_compared = {};
  std::string basis = {};  // ComputedAge::basis
};

// The import source as the store has it, against the adapter's source now.
struct SourceState {
  bool registered = false;
  std::string status;                     // registered | running | paused | finished | failed; empty: not registered
  int done = 0, total = 0;
  std::optional<std::string> stored_head;  // the head the last batch was read at
  std::string current_head;                // the adapter's head now

  // The last run reached the end of the source, and the source has not moved since.
  bool finished_and_current() const {
    return registered && status == "finished" && stored_head && *stored_head == current_head;
  }
};

struct VerifyReport {
  // The import. Everything below describes the source as it is now; unless
  // it is the source that was imported, to its end, none of it says the
  // import can be trusted.
  SourceState source;

  // Accounting.
  int units = 0;    // every unit the source holds
  int ignored = 0;  // of those, not part of the import by rule
  std::vector<UnaccountedUnit> unaccounted;  // sorted by path, then commit

  // Idempotence: rows a run would add (RunStats::would_write; catalog rows
  // are not counted there, the accounting covers them).
  int would_write = 0;         // resuming from the stored token
  int replay_would_write = 0;  // walking the whole source again

  // Pending conflicts of this source, parity failures of this run included.
  int pending_blocking = 0;  // data was not imported, or does not agree
  int pending_warnings = 0;  // detail has "imported": true or "synthesized": true
  std::vector<persistence::Uuid> blocking_conflicts, warning_conflicts;  // sorted

  // Age parity. Each interpreted age is compared by its head revision only;
  // every member of that revision is one comparison.
  int parity_pass = 0;           // age and error agree
  // The age agrees and no error was compared: the member has no
  // age_err_wo_j and the file does not say whether its age_err includes J
  // (or says it does and the age function gave no such error).
  int parity_pass_age_only = 0;
  int parity_fail = 0, parity_not_comparable = 0;
  std::map<std::string, int> not_comparable_reasons;  // reason -> members
  std::vector<ParityFailure> parity_failures;         // sorted by interpreted age, then analysis

  // The import finished and the source has not moved, everything accounted
  // for, nothing to write, no blocking conflict, no parity failure. Members
  // that are not comparable do not make it false: look at
  // parity_not_comparable.
  bool ok() const {
    return source.finished_and_current() && unaccounted.empty() && would_write == 0 && replay_would_write == 0 && pending_blocking == 0 &&
           parity_fail == 0;
  }
};

// `config` is the writer configuration the import ran with; its dry_run and
// replay are set here. `client` writes the parity conflicts. An empty
// `age_fn` leaves every member not comparable. The adapter is planned and
// walked several times; plan it again before using it for an import.
// Whether a pending conflict only annotates what is in the store (a warning)
// rather than saying data is missing or disagrees (blocking): spec 10.26 and
// 10.35. The rule VerifyReport's two counts are made with.
bool is_warning_conflict(const persistence::ImportConflictRow& row);

Result<VerifyReport> verify(persistence::IStore& store, persistence::Uuid client, ISourceAdapter& adapter,
                            const WriterConfig& config, const AgeFn& age_fn, VerifyOptions options = {});

}  // namespace pychron::ingest
