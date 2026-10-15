#pragma once

// The backfill of the cache of ages (age cache spec 4.5, writer 2): walks a
// store's analyses newest first and reduces, and caches the age of, each one
// whose cached age is not of its inputs now. A second walk finds everything
// current; one cut short leaves what it wrote, so the next goes on from
// there. Invalid analyses are walked too: the cache is of every analysis.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/source.hpp"
#include "pychron/processing/store_source.hpp"

namespace pychron::processing {

struct DeriveOptions {
  bool all = false;          // reduce and write every analysis, current or not
  std::optional<int> limit;  // walk no more than this many analyses
  int page_size = 200;       // analyses a page: a read of fingerprints and a write each
};

struct DeriveStats {
  int seen = 0;     // analyses walked
  int current = 0;  // cached under the fingerprint of their inputs now: passed over
  int written = 0;  // reduced and written, those without an age included
  int no_age = 0;   // of the written, those whose reduction gave no age (a blank, an air)
  int failed = 0;   // could not be loaded
  std::vector<std::string> failures;  // "<run id>: <why>", one a failure
};

// What the walk needs of a store source. StoreSource is one; tests wrap it.
class IDeriveSource {
 public:
  virtual ~IDeriveSource() = default;
  virtual Result<BrowsePage> browse(const BrowseQuery& query) = 0;
  virtual Result<std::map<std::string, StoreSource::Fingerprints>> fingerprints(
      const std::vector<std::string>& analyses) = 0;
  virtual Result<AnalysisPtr> load(const std::string& uuid) = 0;
  virtual Result<std::size_t> write(const std::vector<DerivedAge>& ages) = 0;
  virtual void drop_loaded(const std::vector<std::string>& analyses) = 0;
};

// Walks by pages (`page_size`, 200) through browse, with no filter. A page's cached
// fingerprints are read in one statement; only an analysis whose two
// fingerprints differ (or every one, with `all`) is loaded, reduced with the
// default settings and written, in one write a page. A load that fails is
// counted and named, and the walk goes on. `keep_going` is asked after each
// page with the counts so far; false ends the walk there. An error is what
// stopped the walk itself: a page that could not be read or written (what
// earlier pages wrote stays).
Result<DeriveStats> derive_ages(IDeriveSource& source, const DeriveOptions& options,
                                const std::function<bool(const DeriveStats&)>& keep_going);
Result<DeriveStats> derive_ages(StoreSource& source, const DeriveOptions& options,
                                const std::function<bool(const DeriveStats&)>& keep_going);

}  // namespace pychron::processing
