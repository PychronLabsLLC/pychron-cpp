#include "pychron/processing/derive.hpp"

#include <algorithm>
#include <utility>

#include "pychron/processing/domain.hpp"
#include "pychron/processing/reduced.hpp"

namespace pychron::processing {

namespace {

class StoreDeriveSource final : public IDeriveSource {
 public:
  explicit StoreDeriveSource(StoreSource& source) : source_(source) {}
  Result<BrowsePage> browse(const BrowseQuery& query) override { return source_.browse(query); }
  Result<std::map<std::string, StoreSource::Fingerprints>> fingerprints(
      const std::vector<std::string>& analyses) override {
    return source_.fingerprints(analyses);
  }
  Result<AnalysisPtr> load(const std::string& uuid) override { return source_.load(uuid); }
  Result<std::size_t> write(const std::vector<DerivedValues>& ages) override { return source_.write_derived_now(ages); }
  void drop_loaded(const std::vector<std::string>& analyses) override { source_.drop_loaded(analyses); }

 private:
  StoreSource& source_;
};

std::string run_id(const AnalysisSummary& row) { return row.runid.empty() ? row.uuid : row.runid; }

}  // namespace

Result<DeriveStats> derive_ages(IDeriveSource& source, const Domain& domain, const DeriveOptions& options,
                                const std::function<bool(const DeriveStats&)>& keep_going) {
  DeriveStats stats;
  BrowseQuery query;
  query.exclude_tags.clear();  // invalid analyses are cached too
  const int page_size = std::max(1, options.page_size);
  query.limit = page_size;
  const ReductionSettings settings{};
  while (true) {
    if (options.limit) {
      const int left = *options.limit - stats.seen;
      if (left <= 0) break;
      query.limit = std::min(page_size, left);
    }
    auto page = source.browse(query);
    if (!page) return fail(page.error());
    if (page->rows.empty()) break;

    std::vector<std::string> uuids;
    uuids.reserve(page->rows.size());
    for (const auto& row : page->rows) uuids.push_back(row.uuid);
    auto prints = source.fingerprints(uuids);
    if (!prints) return fail(prints.error());

    std::vector<DerivedValues> ages;
    int without_age = 0;
    for (const auto& row : page->rows) {
      ++stats.seen;
      const auto it = prints->find(row.uuid);
      const bool fresh = it != prints->end() && it->second.cached && !it->second.current.empty() &&
                         *it->second.cached == it->second.current;
      if (fresh && !options.all) {
        ++stats.current;
        continue;
      }
      auto analysis = source.load(row.uuid);
      if (!analysis) {
        ++stats.failed;
        stats.failures.push_back(run_id(row) + ": " + to_string(analysis.error()));
        continue;
      }
      // The age is of the fingerprint the analysis was loaded with, which is
      // the one compared above unless a reference changed in between: then a
      // correct row is written and counted as written.
      auto age = domain.derive(*reduce_analysis(*analysis, settings));
      if (!age) {
        ++stats.failed;
        stats.failures.push_back(run_id(row) + ": no input fingerprint");
        continue;
      }
      if (const auto* marker = age->values.empty() ? nullptr : &age->values.front(); !marker || !marker->value) ++without_age;
      ages.push_back(std::move(*age));
    }
    if (!ages.empty()) {
      auto wrote = source.write(ages);
      if (!wrote) {
        // The store refused a page: the next would fare no better.
        source.drop_loaded(uuids);
        return fail(wrote.error());
      }
      stats.written += static_cast<int>(ages.size());
      stats.no_age += without_age;
    }
    source.drop_loaded(uuids);
    if (!keep_going(stats)) break;
    if (!page->next) break;
    query.after = page->next;
  }
  return stats;
}

Result<DeriveStats> derive_ages(StoreSource& source, const Domain& domain, const DeriveOptions& options,
                                const std::function<bool(const DeriveStats&)>& keep_going) {
  StoreDeriveSource adapter(source);
  return derive_ages(adapter, domain, options, keep_going);
}

}  // namespace pychron::processing
