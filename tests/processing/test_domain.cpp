// The Domain interface (domain seam spec 4): the core-only noble-gas domain
// running the neutral units on data with no argon. The Ar-Ar domain's cases
// are tests/arar/test_domain_arar.cpp.

#include "pychron/processing/domain.hpp"

#include <gtest/gtest.h>

#include "fixtures.hpp"
#include "pychron/processing/time_series.hpp"

namespace pp = pychron::processing;
using pp::test::make_unknown;

namespace {

const std::string kFingerprint(64, 'a');

std::vector<std::string> kinds_of(const std::vector<pp::FigureEntry>& figures) {
  std::vector<std::string> out;
  out.reserve(figures.size());
  for (const auto& f : figures) out.push_back(f.kind);
  return out;
}

// A memory source with an age cache that records what it is given.
struct SinkSource : pp::MemorySource, pp::IDerivedSink {
  pp::IDerivedSink* derived() noexcept override { return this; }
  void put(std::vector<pp::DerivedValues> values) override { puts.push_back(std::move(values)); }
  std::vector<std::vector<pp::DerivedValues>> puts;
};

pp::Pipeline reduce_pipeline(const pp::Domain& domain) {
  pp::Pipeline p;
  const auto& reg = domain.units();
  p.add(reg, "select", "select");
  std::string previous = "select";
  for (const auto& kind : domain.reduction_chain()) {
    p.add(reg, kind, kind, {previous});
    previous = kind;
  }
  return p;
}

// An analysis of the noble gases and no argon at all.
std::shared_ptr<pp::Analysis> noble_analysis(int index, const std::string& type) {
  auto a = std::make_shared<pp::Analysis>();
  a->uuid = "noble-" + std::to_string(index);
  a->identifier = "N1";
  a->aliquot = index + 1;
  a->runid = pp::make_runid(a->identifier, a->aliquot, -1);
  a->analysis_type = type;
  a->timestamp = 1'700'000'000.0 + 3600.0 * index;
  a->mass_spectrometer = "noble";
  double v = 1.0;
  for (const char* name : {"He3", "He4", "Ne20", "Ne21", "Ne22"}) {
    pp::IsotopeData d;
    d.key = name;
    d.isotope = name;
    d.detector = "H1";
    d.intercept = {v * (index + 1), 0.01};
    d.baseline = {0.001, 0.0001};
    d.blank = {0.0, 0.0};
    d.n = 50;
    a->isotopes.push_back(d);
    v += 1.0;
  }
  return a;
}

}  // namespace

TEST(Domain, NobleGasHasNoAgeNoExportNoRecallExtras) {
  const auto domain = pp::make_noble_gas_domain();
  EXPECT_EQ(kinds_of(domain->figures()),
            (std::vector<std::string>{"time_series", "isotope_evolution_fit", "blank_fit", "icfactor_fit"}));
  EXPECT_TRUE(domain->derived().empty());
  EXPECT_TRUE(domain->exporters().empty());
  EXPECT_TRUE(domain->quantities().empty());
  auto a = make_unknown(0);
  a->input_fingerprint = kFingerprint;
  const auto reduced = pp::reduce_analysis(a, pp::ReductionSettings{});
  EXPECT_FALSE(domain->derive(*reduced).has_value());
  EXPECT_TRUE(domain->recall_sections(*reduced).empty());
  EXPECT_TRUE(domain->error_budget(*reduced).empty());
  EXPECT_EQ(domain->reduction_version(), "noble-1");
}

TEST(Domain, EveryFigureKindIsInTheDomainsRegistry) {
  const auto domain = pp::make_noble_gas_domain();
  for (const auto& entry : domain->figures())
    EXPECT_NE(domain->units().find(entry.kind), nullptr) << domain->id() << ": " << entry.kind;
}

TEST(Domain, NobleGasRunsEveryCoreUnitOnAnArgonFreeFixture) {
  const auto domain = pp::make_noble_gas_domain();
  SinkSource source;
  auto first = noble_analysis(0, "unknown");
  first->input_fingerprint = kFingerprint;
  auto blank = noble_analysis(1, "blank_unknown");
  blank->input_fingerprint = std::string(64, 'b');
  source.add(first);
  source.add(blank);

  pp::Runner runner(domain->units(), &source, domain.get());
  const auto& reg = domain->units();

  // select -> reduce -> filter -> group -> edits -> group_stats
  pp::Pipeline stats = reduce_pipeline(*domain);
  stats.add(reg, "filter", "filter", {"reduce"});
  stats.add(reg, "group", "group", {"filter"});
  stats.add(reg, "edits", "edits", {"group"});
  stats.add(reg, "stats", "group_stats", {"edits"});
  ASSERT_TRUE(stats.find("stats")->options.set("quantity", domain->default_quantity()));
  auto out = runner.run(stats, "stats");
  ASSERT_TRUE(out) << out.error().what;
  const auto& results = *std::get<pp::GroupResultsPtr>(out->at(0));
  ASSERT_FALSE(results.rows.empty());
  for (const auto& row : results.rows) {
    EXPECT_EQ(row.n_included, 0u);
    EXPECT_FALSE(row.error.empty());
  }

  // select -> reduce -> group -> graph -> edits -> time_series
  pp::Pipeline series = reduce_pipeline(*domain);
  series.add(reg, "group", "group", {"reduce"});
  series.add(reg, "graph", "group", {"group"});
  ASSERT_TRUE(series.find("graph")->options.set("level", std::string("graph")));
  series.add(reg, "edits", "edits", {"graph"});
  series.add(reg, "figure", "time_series", {"edits"});
  std::vector<pp::Options> panels;
  for (const char* isotope : {"He3", "He4", "Ne20", "Ne21", "Ne22"}) {
    auto row = series.find("figure")->options.new_row("panels");
    ASSERT_TRUE(row.set("quantity", std::string(isotope)));
    panels.push_back(row);
  }
  ASSERT_TRUE(series.find("figure")->options.set_rows("panels", panels));
  out = runner.run(series, "figure");
  ASSERT_TRUE(out) << out.error().what;
  const auto& scene = *std::get<pp::ScenePtr>(out->at(0));
  ASSERT_EQ(scene.graphs.size(), 1u);
  ASSERT_EQ(scene.graphs[0].panels.size(), 5u);
  for (const auto& panel : scene.graphs[0].panels) {
    std::size_t points = 0;
    for (const auto& layer : panel.layers)
      if (const auto* p = std::get_if<pp::PointLayer>(&layer)) points += p->y.size();
    EXPECT_EQ(points, 2u) << panel.quantity;
  }

  EXPECT_TRUE(source.puts.empty());  // no domain value, so nothing for the cache
}
