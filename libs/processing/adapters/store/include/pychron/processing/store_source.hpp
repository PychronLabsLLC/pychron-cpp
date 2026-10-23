#pragma once

// An analysis source over the DVC store (data browsing and visualization
// design, section 9.2). Browse and facets are SQL in the store; load() reads
// the analysis detail, the head payloads and the references resolve_refs()
// picks into one Analysis: the gains and the flux monitor itself, the rest
// (an Ar-Ar flux, production, chronology) through the domain's extension
// loader (StoreSourceOptions::extension_loader).
//
// Store rule: a connection belongs to the thread that opened it. The source
// owns a small pool of threads, each opening its own store, and runs every
// call on one of them; callers on any thread block until it returns.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/persistence/store.hpp"
#include "pychron/processing/revisions.hpp"
#include "pychron/processing/source.hpp"

namespace pychron::processing {

// What load() assembles. `refs` are the payloads of the resolved references.
struct StoreAnalysisParts {
  persistence::AnalysisDetail detail;
  std::map<persistence::Kind, persistence::RevisionPayload> heads;
  std::map<persistence::Kind, persistence::Uuid> head_revisions;
  std::vector<persistence::RefPayload> refs;
  // persistence::fingerprint_of the heads and references read, as hex.
  std::string input_fingerprint;
  // The catalog row of the analysis's sample (location, lithology, IGSN for
  // the report); absent when the store has no sample of that name.
  std::optional<persistence::SampleRow> sample;
};

// What a domain makes of an analysis it loads from the store (domain seam spec
// 3.3): its Analysis::extension, from the parts (the resolved references
// among them) and the neutral analysis built from them. Fails with the reason
// when the references cannot be read (the load then fails); null: no data.
using StoreExtensionLoader =
    std::function<Result<std::shared_ptr<const DomainAnalysisData>>(const StoreAnalysisParts&, const Analysis&)>;

struct StoreSourceOptions {
  int connections = 2;  // worker threads, one store each
  // Who saves edits: an app_user name and this machine's client (role
  // "reduction"). Empty: $USER and $HOSTNAME (or "pychron", "localhost").
  std::string user, hostname;
  // The active domain's reduction version (Domain::reduction_version()):
  // part of every input fingerprint, and the version the derived values are
  // written under. Must be set for a source that caches: with it empty the
  // sink and write_derived_now refuse every write (ErrorKind::Config,
  // "StoreSourceOptions::reduction_version not set"), since rows under ""
  // would be a cache no other source reads. Browsing and loading work without.
  std::string reduction_version;
  // Sets Analysis::extension on each load; empty: no extension.
  using ExtensionLoader = StoreExtensionLoader;
  ExtensionLoader extension_loader;
};

// What the sink did with the ages given to it since the last flush.
struct DerivedFlush {
  std::size_t written = 0;  // analyses whose rows were written
  std::size_t current = 0;  // analyses already cached under that fingerprint: not written
  // Analyses dropped: a malformed id or fingerprint, a write the store
  // refused, or a source without a reduction version.
  std::size_t failed = 0;
  std::string last_error;   // of the last that failed
};

class StoreSource final : public IAnalysisSource, public IRevisionSource, public IDerivedSink {
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
  // load(uuid) as of `bookmark` (data publishing spec 4.3): heads, payloads
  // and references from its entries. Not cached; `input_fingerprint` is of
  // the pinned revisions. Config when the bookmark does not pin the analysis.
  Result<AnalysisPtr> load_at(const std::string& uuid, const std::string& bookmark);
  Result<RawData> load_raw(const std::string& uuid) override;
  IRevisionSource* revisions() noexcept override { return this; }
  IDerivedSink* derived() noexcept override { return this; }

  // The sink (age cache spec 4.4, 4.5). put() queues one task on the pool
  // and returns. The task reads the fingerprints the batch's analyses are
  // cached under, in one statement, and writes those that differ, in one
  // transaction: `age`, `age_w_j` (Ma) and `kca`, or an `age` without value
  // for an analysis that has none. A batch the store refuses is dropped: it
  // is a cache, and the next reduction or the backfill fills it. What is
  // queued when the source closes is written first.
  void put(std::vector<DerivedValues> ages) override;
  // Returns when every write queued so far is done, with what became of
  // them since the last flush. For tests and for elctl.
  DerivedFlush flush_derived();

  // For the backfill (derive.hpp, age cache spec 4.5): for each analysis,
  // the fingerprint its age is cached under (none when it has no age row) and
  // its input fingerprint now, both hex; `current` is empty when it could not
  // be read. One statement for the cached ones, a few an analysis for the rest.
  struct Fingerprints {
    std::optional<std::string> cached;
    std::string current;
  };
  Result<std::map<std::string, Fingerprints>> fingerprints(const std::vector<std::string>& analyses);
  // Writes the ages now, in one transaction, whatever is cached (the
  // backfill's --all); how many were written. A malformed id or
  // fingerprint, or a source without a reduction version, is a Config error
  // and nothing is written.
  Result<std::size_t> write_derived_now(const std::vector<DerivedValues>& ages);
  // Forgets loaded analyses without moving the generation: a walk over a
  // whole store holds no more than a page of them.
  void drop_loaded(const std::vector<std::string>& analyses);

  Result<std::vector<RevisionSummary>> history(const std::string& analysis, RevisionKind kind) override;
  Result<RevisionTable> revision_table(const std::string& revision) override;
  Result<SaveOutcome> save_fits(const std::string& analysis, const std::map<std::string, std::string>& heads,
                                const std::vector<EditedFit>& edits, const std::string& message) override;
  Result<SaveOutcome> save_reference_fits(const ReferenceFitSet& fits) override;
  Result<SaveOutcome> save_isotope_fits(const IsotopeFitSet& fits) override;
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

// The neutral analysis of `parts`; with `loader` set, its `extension` is what
// the loader makes of the parts and that analysis. Fails when the loader does.
Result<Analysis> analysis_from_store(const StoreAnalysisParts& parts, const StoreExtensionLoader& loader = {});

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

// The rows of a blanks / IC factors revision with reference fits applied:
// value, error, fit, error type (blanks), reference detector and standard
// ratio (IC factors), references, reviewed; a manual override is cleared.
// Rows for keys the revision lacks are appended.
persistence::Blanks apply_blank_fits(persistence::Blanks rows, const std::vector<ReferenceRowFit>& fits);
persistence::IcFactors apply_icfactor_fits(persistence::IcFactors rows, const std::vector<ReferenceRowFit>& fits);

// "[1, 5, 9]" <-> indices. Malformed text yields what was read before the error.
std::vector<std::size_t> parse_index_list(std::string_view json);
std::string index_list_json(const std::vector<std::size_t>& indices);

// Numbers (and booleans, as 0/1) of a flat JSON object; other members are
// skipped. Malformed text yields what was read before the error.
std::map<std::string, double> flat_json_numbers(std::string_view json);

// The keys of the "nonfinite" member of a row's extra_json: the JSON pointers
// ("/value", "/error") of the numbers the legacy importer read as NaN or
// Infinity and stored as NULL. Empty without such a member or on malformed text.
std::vector<std::string> nonfinite_pointers(std::string_view extra_json);

}  // namespace pychron::processing
