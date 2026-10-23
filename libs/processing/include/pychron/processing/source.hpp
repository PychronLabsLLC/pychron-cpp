#pragma once

// Analysis sources (design section 9.1): browse summaries, list facet values,
// load analyses and, separately, their raw series. Implementations must allow
// concurrent calls from several threads.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/model.hpp"

namespace pychron::processing {

// Keyset cursor: rows strictly older than (timestamp, uuid).
struct BrowseCursor {
  double timestamp = 0.0;
  std::string uuid;
  friend bool operator==(const BrowseCursor&, const BrowseCursor&) = default;
};

struct BrowseQuery {
  std::string text;  // case-insensitive prefix of run id, identifier or sample
  std::vector<std::string> identifiers, samples, projects, principal_investigators, materials, analysis_types,
      mass_spectrometers, extract_devices, loads, irradiations, levels, repositories;
  std::optional<double> from, to;     // UTC epoch seconds, inclusive
  std::optional<double> last_hours;   // relative to the newest analysis in the source
  std::vector<std::string> exclude_tags{"invalid"};
  // Cached age (Ma), inclusive. An analysis without one matches no bound.
  std::optional<double> age_min, age_max;
  std::vector<std::string> uuids;  // non-empty: only these analyses
  int limit = 200;
  std::optional<BrowseCursor> after;
};

struct AnalysisSummary {
  std::string uuid, runid, identifier, sample, project, material, principal_investigator, analysis_type,
      mass_spectrometer, extract_device, load, irradiation, level, repository, tag;
  int aliquot = 0;
  int increment = -1;
  double timestamp = 0.0;
  std::optional<double> extract_value;
  std::string extract_units;
  // The age cached for the analysis and its 1 sigma error (Ma), where the
  // source keeps one; never recomputed here, so it may be out of date.
  std::optional<double> age, age_err;
};

struct BrowsePage {
  std::vector<AnalysisSummary> rows;    // newest first
  std::optional<BrowseCursor> next;     // present when more rows match
  std::optional<std::size_t> total;     // matching rows, when cheap to know
};

enum class Facet {
  AnalysisType,
  MassSpectrometer,
  ExtractDevice,
  Project,
  PrincipalInvestigator,
  Sample,
  Material,
  Identifier,
  Irradiation,
  Level,
  Load,
  Repository,
};

std::string_view to_string(Facet f) noexcept;

// True when `s` passes every filter of `q` except `ignore` (facets are computed
// from all other filters). Paging, text and last_hours are applied too;
// `newest` is the newest timestamp in the source (for last_hours).
bool matches(const BrowseQuery& q, const AnalysisSummary& s, double newest,
             std::optional<Facet> ignore = std::nullopt);
std::string facet_value(const AnalysisSummary& s, Facet f);

class IRevisionSource;  // revisions.hpp

// One value a domain derives from a reduction and the cache keeps: a name
// ("age"), the value with its error, and the units of both (empty: none).
// `value` absent: the analysis has none, which is cached as such.
struct DerivedValue {
  std::string name;
  std::optional<reduction::UFloat> value;
  std::string units;
};

// What one reduction of one analysis gives the cache of derived values (age
// cache spec 4.1, 4.4): one value per name the domain derives. The first is
// the marker row, written for every derived analysis, valueless when there is
// no value (the Ar-Ar domain: `age`, "the analysis reduces to no age: a
// blank, an air, an unknown without flux").
struct DerivedValues {
  std::string uuid, input_fingerprint;
  std::vector<DerivedValue> values;
};

// The value called `name`, or null.
[[nodiscard]] inline const DerivedValue* find_derived(const DerivedValues& d, std::string_view name) noexcept {
  for (const auto& v : d.values)
    if (v.name == name) return &v;
  return nullptr;
}

class IDerivedSink {
 public:
  virtual ~IDerivedSink() = default;
  // Queues the values and returns at once: no caller waits on a cache. A write
  // that fails is the sink's to report; the next reduction fills it.
  virtual void put(std::vector<DerivedValues> values) = 0;
};

class IAnalysisSource {
 public:
  virtual ~IAnalysisSource() = default;
  // Revision history and saving edits, for sources that keep revisions (the
  // DVC store); nullptr otherwise. Owned by the source.
  virtual IRevisionSource* revisions() noexcept { return nullptr; }
  virtual std::string name() const = 0;
  // Changes whenever refresh() finds new or changed analyses.
  virtual std::uint64_t generation() const = 0;
  virtual Result<void> refresh() = 0;
  virtual Result<BrowsePage> browse(const BrowseQuery& query) = 0;
  // Distinct non-empty values of `facet` among analyses matching every other
  // filter of `query`, sorted.
  virtual Result<std::vector<std::string>> facet(Facet facet, const BrowseQuery& query) = 0;
  virtual Result<AnalysisPtr> load(const std::string& uuid) = 0;
  virtual Result<RawData> load_raw(const std::string& uuid) = 0;
  // Where reductions of this source's analyses leave their ages, for a source
  // that keeps a cache of them (the DVC store); nullptr otherwise. Owned by
  // the source.
  virtual IDerivedSink* derived() noexcept { return nullptr; }
};

// An in-memory source; tests and figure previews use it.
class MemorySource : public IAnalysisSource {
 public:
  explicit MemorySource(std::vector<AnalysisPtr> analyses = {}, std::map<std::string, RawData> raw = {});
  // Adds, or replaces the analysis with the same uuid (keeping its raw data
  // unless `raw` is given).
  void add(AnalysisPtr analysis, std::optional<RawData> raw = std::nullopt);
  // Gives the analysis a cached age (Ma) for browse to show and filter by,
  // as a store's derived_value would (tests, previews).
  void set_age(const std::string& uuid, double age, double age_err);
  // Drops the analysis with its raw data and age. An unknown uuid does nothing.
  void remove(const std::string& uuid);

  std::string name() const override { return "memory"; }
  std::uint64_t generation() const override;
  Result<void> refresh() override { return {}; }
  Result<BrowsePage> browse(const BrowseQuery& query) override;
  Result<std::vector<std::string>> facet(Facet facet, const BrowseQuery& query) override;
  Result<AnalysisPtr> load(const std::string& uuid) override;
  Result<RawData> load_raw(const std::string& uuid) override;

 private:
  // Every analysis summarized, with its age where set_age gave one. Called
  // with mutex_ held.
  std::vector<AnalysisSummary> summaries() const;

  mutable std::mutex mutex_;
  std::vector<AnalysisPtr> analyses_;
  std::map<std::string, RawData> raw_;
  std::map<std::string, std::pair<double, double>> ages_;  // uuid -> age, error
  std::uint64_t generation_ = 1;
};

AnalysisSummary summarize(const Analysis& a);

// browse() over in-memory summaries, newest first, keyset paged.
BrowsePage browse_summaries(const std::vector<AnalysisSummary>& rows, const BrowseQuery& q);
std::vector<std::string> facet_values(const std::vector<AnalysisSummary>& rows, Facet f, const BrowseQuery& q);

}  // namespace pychron::processing
