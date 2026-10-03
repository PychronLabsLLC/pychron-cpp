#pragma once

// An analysis source over the DVC store (data browsing and visualization
// design, section 9.2). Browse and facets are SQL in the store; load() reads
// the analysis detail, the head payloads and the references resolve_refs()
// picks (flux, production, chronology, gains) into one Analysis.
//
// Store rule: a connection belongs to the thread that opened it. The source
// owns a small pool of threads, each opening its own store, and runs every
// call on one of them; callers on any thread block until it returns.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "pychron/persistence/store.hpp"
#include "pychron/processing/revisions.hpp"
#include "pychron/processing/source.hpp"

namespace pychron::processing {

struct StoreSourceOptions {
  int connections = 2;  // worker threads, one store each
  // Who saves edits: an app_user name and this machine's client (role
  // "reduction"). Empty: $USER and $HOSTNAME (or "pychron", "localhost").
  std::string user, hostname;
};

class StoreSource final : public IAnalysisSource, public IRevisionSource {
 public:
  // Opens one store per connection (applying migrations as `config` says)
  // and fails if any cannot be opened.
  static Result<std::unique_ptr<StoreSource>> open(persistence::StoreConfig config, StoreSourceOptions options = {});
  ~StoreSource() override;
  StoreSource(const StoreSource&) = delete;
  StoreSource& operator=(const StoreSource&) = delete;

  std::string name() const override;
  std::uint64_t generation() const override;
  // Reads the change log since the last refresh; any change bumps the
  // generation and drops cached analyses.
  Result<void> refresh() override;
  Result<BrowsePage> browse(const BrowseQuery& query) override;
  Result<std::vector<std::string>> facet(Facet facet, const BrowseQuery& query) override;
  Result<AnalysisPtr> load(const std::string& uuid) override;
  Result<RawData> load_raw(const std::string& uuid) override;
  IRevisionSource* revisions() noexcept override { return this; }

  Result<std::vector<RevisionSummary>> history(const std::string& analysis, RevisionKind kind) override;
  Result<RevisionTable> revision_table(const std::string& revision) override;
  Result<SaveOutcome> save_fits(const std::string& analysis, const std::map<std::string, std::string>& heads,
                                const std::vector<EditedFit>& edits, const std::string& message) override;
  Result<SaveOutcome> restore_revision(const std::string& analysis, RevisionKind kind, const std::string& expected,
                                       const std::string& revision, const std::string& message) override;

  struct Impl;

 private:
  explicit StoreSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// "postgresql://user:password@host/db" -> "postgresql://user:***@host/db".
std::string redact_password(std::string url);

// Pure mappings, exposed for tests.
persistence::BrowseFilter to_store_filter(const BrowseQuery& query);
AnalysisSummary summary_from_row(const persistence::BrowseRow& row);
persistence::BrowseFacet to_store_facet(Facet facet) noexcept;

// What load() assembles. `refs` are the payloads of the resolved references.
struct StoreAnalysisParts {
  persistence::AnalysisDetail detail;
  std::map<persistence::Kind, persistence::RevisionPayload> heads;
  std::map<persistence::Kind, persistence::Uuid> head_revisions;
  std::vector<persistence::RefPayload> refs;
};
Result<Analysis> analysis_from_store(const StoreAnalysisParts& parts);

// One decoded raw series; slices [start_index, end_index) when either is set.
Result<RawSeries> series_from_blob(const persistence::SignalRefRow& ref, const persistence::BlobData& blob);

// A revision's payload as a table (History tab).
RevisionTable revision_table_from(const persistence::RevisionPayload& payload);

// The rows of an intercepts (signal edits) or baselines (baseline edits)
// revision with refits applied: value, error, fit, error type, n (raw
// points), fn (points used), outlier filter and user exclusions; a manual
// override on an edited row is cleared. Edits of the other kind are skipped.
// Fails when an edit names an isotope or detector the rows do not have.
Result<persistence::Intercepts> apply_intercept_edits(persistence::Intercepts rows, const std::vector<EditedFit>& edits);
Result<persistence::Baselines> apply_baseline_edits(persistence::Baselines rows, const std::vector<EditedFit>& edits);

// "[1, 5, 9]" <-> indices. Malformed text yields what was read before the error.
std::vector<std::size_t> parse_index_list(std::string_view json);
std::string index_list_json(const std::vector<std::size_t>& indices);

// Numbers (and booleans, as 0/1) of a flat JSON object; other members are
// skipped. Malformed text yields what was read before the error.
std::map<std::string, double> flat_json_numbers(std::string_view json);

}  // namespace pychron::processing
