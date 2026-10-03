#pragma once

// Analysis sources (design section 9.1): browse summaries, list facet values,
// load analyses and, separately, their raw series. Implementations must allow
// concurrent calls from several threads.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
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
};

// An in-memory source; tests and figure previews use it.
class MemorySource : public IAnalysisSource {
 public:
  explicit MemorySource(std::vector<AnalysisPtr> analyses = {}, std::map<std::string, RawData> raw = {});
  void add(AnalysisPtr analysis, std::optional<RawData> raw = std::nullopt);

  std::string name() const override { return "memory"; }
  std::uint64_t generation() const override;
  Result<void> refresh() override { return {}; }
  Result<BrowsePage> browse(const BrowseQuery& query) override;
  Result<std::vector<std::string>> facet(Facet facet, const BrowseQuery& query) override;
  Result<AnalysisPtr> load(const std::string& uuid) override;
  Result<RawData> load_raw(const std::string& uuid) override;

 private:
  mutable std::mutex mutex_;
  std::vector<AnalysisPtr> analyses_;
  std::map<std::string, RawData> raw_;
  std::uint64_t generation_ = 1;
};

AnalysisSummary summarize(const Analysis& a);

// browse() over in-memory summaries, newest first, keyset paged.
BrowsePage browse_summaries(const std::vector<AnalysisSummary>& rows, const BrowseQuery& q);
std::vector<std::string> facet_values(const std::vector<AnalysisSummary>& rows, Facet f, const BrowseQuery& q);

}  // namespace pychron::processing
