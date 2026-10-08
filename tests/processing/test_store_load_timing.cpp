// Not a test of behaviour: a stopwatch on the figure pipeline against a real
// store, for when loading is slow or a change might make it so. It does
// nothing unless told which store:
//
//   PYCHRON_BENCH_DB=sqlite:/path/to/store.db [PYCHRON_BENCH_N=400] \
//     build/dev/tests/processing/pychron_processing_store_tests --gtest_filter='StoreLoadTiming.*'
//
// It writes nothing to the store. It takes the PYCHRON_BENCH_N analyses
// (default 24) of the identifiers that have most, and times each node of the
// pipeline a figure window runs: select (loading from the store), reduce,
// group, edits and the ideogram. Its first run, on a store of 8700 analyses,
// is what found that 24 analyses took 8.7 s to load and that none of it was
// calculation.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>

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

  const auto& reg = UnitRegistry::builtin();
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

  Runner runner(reg, &source);
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
  Runner again(reg, &source);
  t0 = now_s();
  (void)again.run(p, "figure");
  std::printf("second window        %8.3f s\n", now_s() - t0);
}

}  // namespace pychron::processing
