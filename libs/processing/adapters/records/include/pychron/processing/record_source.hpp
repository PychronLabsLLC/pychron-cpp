#pragma once

// An analysis source over a records directory (data browsing and
// visualization design, section 9.2): the FilePersister layout
// <root>/<identifier>/<runid>.json, one AnalysisRecord per file.
//
// Records carry no domain data (an Ar-Ar flux or production): the domain's
// extension loader adds it (domain seam spec 3.3), Ar-Ar's from an optional
// <root>/references.toml (libs/arar record_loader.hpp).

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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

// What a domain adds to the analyses of a records directory (domain seam spec
// 3.3). A loader belongs to one source: what `refresh` remembers is what that
// source has applied.
struct RecordExtensionLoader {
  // Reads again what `load` draws on (a file beside the records), at the
  // start of each RecordDirectorySource::refresh(); true when it changed
  // since the last call, and every record is then mapped again. A failure
  // fails the refresh. Empty: nothing to read, never changed.
  std::function<Result<bool>()> refresh;
  // The extension of the analysis mapped from `record`; null: none.
  std::function<std::shared_ptr<const DomainAnalysisData>(const experiment::record::AnalysisRecord& record,
                                                          const Analysis& analysis)>
      load;
};

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

  // Sets each analysis's extension from now on: the next refresh maps every
  // record again with it.
  void set_extension_loader(RecordExtensionLoader loader);

 private:
  struct Entry {
    std::filesystem::path path;
    std::filesystem::file_time_type mtime;
    std::uintmax_t size = 0;
    AnalysisPtr analysis;
    AnalysisSummary summary;
  };

  std::filesystem::path root_;
  mutable std::mutex mutex_;
  std::map<std::string, Entry> by_path_;  // path string -> entry
  std::map<std::string, std::string> path_of_uuid_;
  std::vector<std::string> problems_;
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<bool> scanned_{false};

  RecordExtensionLoader loader_;
  bool loader_changed_ = false;  // set_extension_loader since the last refresh
};

}  // namespace pychron::processing
