#pragma once

// `import verify` (legacy ingestion spec, sections 6 and 10): whether an
// import of one source can be trusted. It reports
//
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

// The moment a legacy age was computed at: when its interpreted age was
// saved. An age function reduces the analysis from the revisions that were
// head then, with the reference data of then (spec 10.6).
struct AsOf {
  persistence::Uuid interpreted_age;  // the interpreted age the legacy age is stored with
  persistence::Uuid revision;         // its revision that holds the age: the latest one
  persistence::Uuid changeset;        // the changeset that stored that revision
  persistence::UtcTime created;       // that changeset's time: the author date of the commit
};

struct ComputedAge {
  double age = 0, age_err = 0;
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
  std::optional<double> legacy_age_err;  // nullopt: the file has none, and the error was not compared
  double computed_age_err = 0;
  double age_difference = 0, age_err_difference = 0;  // relative
};

struct VerifyReport {
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

  // Age parity. Each interpreted age is compared by its latest revision only;
  // every member of that revision is one comparison.
  int parity_pass = 0, parity_fail = 0, parity_not_comparable = 0;
  std::map<std::string, int> not_comparable_reasons;  // reason -> members
  std::vector<ParityFailure> parity_failures;         // sorted by interpreted age, then analysis

  // Everything accounted for, nothing to write, no blocking conflict, no
  // parity failure. Members that are not comparable do not make it false:
  // look at parity_not_comparable.
  bool ok() const {
    return unaccounted.empty() && would_write == 0 && replay_would_write == 0 && pending_blocking == 0 &&
           parity_fail == 0;
  }
};

// `config` is the writer configuration the import ran with; its dry_run and
// replay are set here. `client` writes the parity conflicts. An empty
// `age_fn` leaves every member not comparable. The adapter is planned and
// walked several times; plan it again before using it for an import.
Result<VerifyReport> verify(persistence::IStore& store, persistence::Uuid client, ISourceAdapter& adapter,
                            const WriterConfig& config, const AgeFn& age_fn, VerifyOptions options = {});

}  // namespace pychron::ingest
