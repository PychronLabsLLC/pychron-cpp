// Not a test of behaviour: a stopwatch on the figure pipeline against a real
// store, for when loading is slow or a change might make it so. It does
// nothing unless told which store (one command line):
//
//   PYCHRON_BENCH_DB=sqlite:/path/to/store.db [PYCHRON_BENCH_N=400]
//     build/dev/tests/processing/pychron_processing_store_tests --gtest_filter='StoreLoadTiming.*'
//
// ReferenceSeries times what the experiment window's reference series dock
// reads after a run is saved: the newest N references of one spectrometer
// since a time (N default 50), then select and reduce of them, on a source
// whose cache is empty, for the blanks tab, the air tab and both.
//
// It writes nothing to the store. It takes the PYCHRON_BENCH_N analyses
// (default 24) of the identifiers that have most, and times each node of the
// pipeline a figure window runs: select (loading from the store), reduce,
// group, edits and the ideogram. Its first run, on a store of 8700 analyses,
// is what found that 24 analyses took 8.7 s to load and that none of it was
// calculation.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "pychron/arar/domain.hpp"
#include "pychron/processing/store_source.hpp"
#include "pychron/processing/units.hpp"

namespace pychron::processing {

static double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

TEST(StoreLoadTiming, FigurePipeline) {
  const char* url = std::getenv("PYCHRON_BENCH_DB");
  if (url == nullptr) return;
  double t0 = now_s();
  auto opened = StoreSource::open(persistence::StoreConfig{url, false});
  ASSERT_TRUE(opened) << to_string(opened.error());
  auto& source = **opened;
  std::printf("open                 %8.3f s\n", now_s() - t0);

  t0 = now_s();
  BrowseQuery q;
  q.analysis_types = {"unknown"};
  q.limit = 2000;
  auto page = source.browse(q);
  ASSERT_TRUE(page) << to_string(page.error());
  std::printf("browse %zu rows       %8.3f s\n", page->rows.size(), now_s() - t0);
  std::map<std::string, std::vector<std::string>> by_id;
  for (const auto& r : page->rows) by_id[r.identifier].push_back(r.uuid);
  std::vector<std::string> uuids;
  const std::size_t want = std::getenv("PYCHRON_BENCH_N") ? std::strtoul(std::getenv("PYCHRON_BENCH_N"), nullptr, 10) : 24;
  // The biggest identifiers first, until there are enough analyses.
  std::vector<std::pair<std::size_t, std::string>> sized;
  for (const auto& [id, v] : by_id) sized.emplace_back(v.size(), id);
  std::sort(sized.rbegin(), sized.rend());
  int groups = 0;
  for (const auto& [n, id] : sized) {
    for (const auto& u : by_id[id]) {
      if (uuids.size() < want) uuids.push_back(u);
    }
    ++groups;
    if (uuids.size() >= want) break;
  }
  std::printf("  %zu analyses in %d groups\n", uuids.size(), groups);

  const auto domain = arar::make_domain();
  const auto& reg = domain->units();
  Pipeline p;
  auto& select = p.add(reg, "select", "select");
  (void)select.options.set("uuids", uuids);
  (void)select.options.set("remove_tags", std::vector<std::string>{});
  p.add(reg, "reduce", "reduce", {"select"});
  p.add(reg, "group", "group", {"reduce"});
  auto& graph = p.add(reg, "graph", "group", {"group"});
  (void)graph.options.set("level", std::string("graph"));
  p.add(reg, "edits", "edits", {"graph"});
  (void)p.find("group")->options.set("key", std::string("identifier"));
  p.add(reg, "figure", "ideogram", {"edits"});

  Runner runner(reg, &source, domain.get());
  double total = 0;
  for (const char* node : {"select", "reduce", "group", "graph", "edits", "figure"}) {
    t0 = now_s();
    auto out = runner.run(p, node);
    const double dt = now_s() - t0;
    total += dt;
    std::printf("%-8s             %8.3f s  %s\n", node, dt, out ? "" : to_string(out.error()).c_str());
  }
  std::printf("total (%zu analyses)  %8.3f s\n", uuids.size(), total);
  // again, with the source's cache warm and the runner's cold
  Runner again(reg, &source, domain.get());
  t0 = now_s();
  (void)again.run(p, "figure");
  std::printf("second window        %8.3f s\n", now_s() - t0);
}

namespace {

struct Tab {
  const char* name;
  std::vector<std::string> types;
};

// The spectrometer with the most analyses of `types`, by a browse of each.
std::string busiest_spectrometer(StoreSource& source, const std::vector<std::string>& types) {
  BrowseQuery q;
  q.analysis_types = types;
  auto names = source.facet(Facet::MassSpectrometer, q);
  if (!names) return {};
  std::string best;
  std::size_t most = 0;
  for (const auto& name : *names) {
    BrowseQuery one = q;
    one.mass_spectrometers = {name};
    one.limit = 100000;
    auto page = source.browse(one);
    if (page && page->rows.size() > most) {
      most = page->rows.size();
      best = name;
    }
  }
  return best;
}

// The browse the dock lists with, then select (by the same query, as the
// dock's pipeline asks for it) and reduce, of one tab on `source`; the times
// printed. Select is given the query, not the browse's uuids: with no uuids
// it would read the query's, and an empty list is no query at all.
double time_tab(StoreSource& source, const Tab& tab, const std::string& spectrometer, std::size_t n) {
  double t0 = now_s();
  BrowseQuery q;
  q.analysis_types = tab.types;
  q.mass_spectrometers = {spectrometer};
  q.from = 0.0;  // since a time: the dock's shape, here from the epoch
  q.limit = n;
  auto page = source.browse(q);
  const double browse_s = now_s() - t0;
  if (!page) {
    std::printf("  %-8s browse failed: %s\n", tab.name, to_string(page.error()).c_str());
    return browse_s;
  }

  const auto& reg = UnitRegistry::builtin();
  Pipeline p;
  auto& select = p.add(reg, "select", "select");
  for (auto set : {select.options.set("analysis_types", tab.types),
                   select.options.set("mass_spectrometers", std::vector<std::string>{spectrometer}),
                   select.options.set("from", 0.0), select.options.set("limit", static_cast<std::int64_t>(n))}) {
    EXPECT_TRUE(set) << to_string(set.error());
  }
  p.add(reg, "reduce", "reduce", {"select"});
  Runner runner(reg, &source);
  t0 = now_s();
  auto out = runner.run(p, "reduce");
  const double load_s = now_s() - t0;
  std::printf("  %-8s %4zu rows  browse %7.3f s  select+reduce %7.3f s  %s\n", tab.name, page->rows.size(), browse_s,
              load_s, out ? "" : to_string(out.error()).c_str());
  return browse_s + load_s;
}

}  // namespace

TEST(StoreLoadTiming, ReferenceSeries) {
  const char* url = std::getenv("PYCHRON_BENCH_DB");
  if (url == nullptr) return;
  const std::size_t n = std::getenv("PYCHRON_BENCH_N") ? std::strtoul(std::getenv("PYCHRON_BENCH_N"), nullptr, 10) : 50;
  const Tab blanks{"blanks", {"blank_unknown", "blank_air", "blank_cocktail", "blank_extraction_line", "blank_laser"}};
  const Tab air{"air", {"air"}};
  // Not a tab: the same shape over unknowns, for a store with few references.
  const Tab unknowns{"unknowns", {"unknown"}};

  const auto open = [&] {
    auto opened = StoreSource::open(persistence::StoreConfig{url, false});
    EXPECT_TRUE(opened) << to_string(opened.error());
    return opened ? std::move(*opened) : nullptr;
  };
  auto probe = open();
  ASSERT_TRUE(probe);
  const std::string spectrometer = busiest_spectrometer(*probe, blanks.types);
  const std::string unknowns_spectrometer = busiest_spectrometer(*probe, unknowns.types);
  probe.reset();
  std::printf("N %zu; blanks and air of %s, unknowns of %s; each line a source with an empty cache\n", n,
              spectrometer.c_str(), unknowns_spectrometer.c_str());

  for (const Tab* tab : {&blanks, &air, &unknowns}) {
    auto source = open();
    ASSERT_TRUE(source);
    (void)time_tab(*source, *tab, tab == &unknowns ? unknowns_spectrometer : spectrometer, n);
  }
  auto source = open();
  ASSERT_TRUE(source);
  std::printf("both tabs, one source:\n");
  const double both = time_tab(*source, blanks, spectrometer, n) + time_tab(*source, air, spectrometer, n);
  std::printf("both tabs            %8.3f s\n", both);
}

}  // namespace pychron::processing
