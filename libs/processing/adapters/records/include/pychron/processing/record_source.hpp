#pragma once

// An analysis source over a records directory (data browsing and
// visualization design, section 9.2): the FilePersister layout
// <root>/<identifier>/<runid>.json, one AnalysisRecord per file.
//
// Records carry no flux or production, so ages need an optional
// <root>/references.toml:
//
//   [flux."66001"]                 # by identifier
//   j = 0.0012
//   j_err = 1.2e-6
//   position_jerr = 0.0            # optional
//
//   [production."NM-300"]          # by irradiation; pairs are [value, error]
//   K4039 = [0.0002, 0.00001]     # legacy keys: K4039 K3839 K3739 Ca3937 Ca3837 Ca3637 Cl3638 Ca_K Cl_K
//   Ca3937 = [0.0007, 0.00001]
//
//   [[chronology."NM-300"]]        # by irradiation
//   power = 1.0
//   start = "2025-01-01T00:00:00Z"
//   end = "2025-01-01T10:00:00Z"

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pychron/experiment/record/types.hpp"
#include "pychron/processing/source.hpp"

namespace pychron::processing {

// "2026-10-02T12:34:56Z" (fractional seconds allowed) -> UTC epoch seconds.
std::optional<double> parse_utc(std::string_view text);
// "" -> -1, "A" -> 0, "Z" -> 25, "AA" -> 26.
int increment_from_step(std::string_view step);

// Pure mappings, exposed for tests.
Analysis analysis_from_record(const experiment::record::AnalysisRecord& rec);
RawData raw_from_record(const experiment::record::AnalysisRecord& rec);

class RecordDirectorySource final : public IAnalysisSource {
 public:
  explicit RecordDirectorySource(std::filesystem::path root);

  const std::filesystem::path& root() const noexcept { return root_; }

  std::string name() const override;
  std::uint64_t generation() const override { return generation_.load(); }
  // Rescans the directory; parses new or changed files only.
  Result<void> refresh() override;
  Result<BrowsePage> browse(const BrowseQuery& query) override;
  Result<std::vector<std::string>> facet(Facet facet, const BrowseQuery& query) override;
  Result<AnalysisPtr> load(const std::string& uuid) override;
  Result<RawData> load_raw(const std::string& uuid) override;

  // Files that failed to parse in the last refresh, with the reason.
  std::vector<std::string> problems() const;

 private:
  struct Entry {
    std::filesystem::path path;
    std::filesystem::file_time_type mtime;
    std::uintmax_t size = 0;
    AnalysisPtr analysis;
    AnalysisSummary summary;
  };

  void apply_references(Analysis& a) const;
  Result<void> load_references();

  std::filesystem::path root_;
  mutable std::mutex mutex_;
  std::map<std::string, Entry> by_path_;  // path string -> entry
  std::map<std::string, std::string> path_of_uuid_;
  std::vector<std::string> problems_;
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<bool> scanned_{false};

  // references.toml
  std::optional<std::filesystem::file_time_type> references_mtime_;
  std::map<std::string, reduction::Flux> flux_;
  std::map<std::string, reduction::ProductionRatios> production_;
  std::map<std::string, std::vector<reduction::Dose>> chronology_;
};

}  // namespace pychron::processing
