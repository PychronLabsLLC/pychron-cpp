// Group ages over a dataset the reduction chain made (select, reduce,
// arar_reduce) agree with the same ages over analyses each reduced on its own
// (reduce_with_arar). Per analysis the two are identical; a sum across
// analyses is not bit for bit: reduce mints every analysis's signals before
// arar_reduce mints production, J and constants, so the variables are
// numbered in another order and UFloat::variance() sums its terms in another
// order. The domain seam spec (3.2) allows 1e-12 relative; this pins it.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "arar_fixtures.hpp"
#include "pychron/arar/arar_groups.hpp"
#include "pychron/arar/domain.hpp"
#include "pychron/arar/reduce.hpp"
#include "pychron/processing/domain.hpp"
#include "pychron/processing/quantity.hpp"
#include "pychron/processing/units.hpp"

namespace pp = pychron::processing;
using pychron::arar::test::make_step;

namespace {

constexpr double kRelative = 1e-12;

// Eight heating steps of one aliquot, every one with F = 10 (make_step builds
// its 40Ar from F, 39Ar and atmospheric 36Ar), so each has the same age. The
// interference ratios are zero, so F stays 10, but carry errors: each step's
// production variables then enter its rad40 and 39ArK, and the sums over the
// steps hold terms of both the signals and the production variables, which
// the two reductions number in different orders. (With these steps the two
// sigmas differ in the last two or three bits; nominal values are equal.)
std::vector<pp::AnalysisPtr> heating_steps() {
  constexpr std::array<double, 8> ar39{5, 20, 40, 60, 15, 8, 33, 12};
  constexpr std::array<double, 8> ar36{0.5, 0.3, 0.2, 0.1, 0.2, 0.4, 0.15, 0.25};
  std::vector<pp::AnalysisPtr> out;
  for (std::size_t i = 0; i < ar39.size(); ++i) {
    auto a = make_step(static_cast<int>(i), ar39[i], ar36[i], 10.0);
    pychron::reduction::ProductionRatios production;
    production.k4039 = {0.0, 5e-3};
    production.ca3937 = {0.0, 1e-3};
    production.ca3637 = {0.0, 2e-4};
    pychron::arar::edit_analysis_data(*a).production = production;
    out.push_back(std::move(a));
  }
  return out;
}

// The dataset select, reduce and arar_reduce give `analyses`.
pp::DatasetPtr reduced_by_the_chain(const std::vector<pp::AnalysisPtr>& analyses, const pp::Domain& domain) {
  pp::MemorySource source(analyses);
  pp::Pipeline p;
  const auto& reg = domain.units();
  p.add(reg, "select", "select");
  std::string previous = "select";
  for (const auto& kind : domain.reduction_chain()) {
    p.add(reg, kind, kind, {previous});
    previous = kind;
  }
  pp::Runner runner(domain.units(), &source, &domain);
  auto ran = runner.run(p, domain.reduction_chain().back());
  EXPECT_TRUE(ran) << (ran ? std::string() : ran.error().what);
  if (!ran) return nullptr;
  return std::get<pp::DatasetPtr>(ran->at(0));
}

// `analyses`, each reduced on its own.
pp::Dataset reduced_one_by_one(const std::vector<pp::AnalysisPtr>& analyses) {
  pp::Dataset d;
  for (const auto& a : analyses) {
    pp::DatasetItem item;
    item.analysis = pychron::arar::reduce_with_arar(a);
    d.mutable_items().push_back(item);
  }
  return d;
}

pp::GroupItems all(const pp::Dataset& d) {
  pp::GroupItems out;
  for (const auto& it : d.items()) out.push_back(&it);
  return out;
}

void expect_relative(double actual, double expected, const char* what) {
  EXPECT_LE(std::abs(actual - expected), kRelative * std::abs(expected))
      << what << ": " << actual << " vs " << expected << ", relative difference "
      << std::abs(actual - expected) / std::abs(expected);
}

}  // namespace

TEST(GroupAgesOverARun, IntegratedAgeAgreesWithPerAnalysisReduction) {
  const auto analyses = heating_steps();
  const auto domain = pychron::arar::make_domain();
  const pp::DatasetPtr chain = reduced_by_the_chain(analyses, *domain);
  ASSERT_TRUE(chain);
  ASSERT_EQ(chain->items().size(), analyses.size());
  for (const auto& it : chain->items()) ASSERT_TRUE(it.analysis->reduction_error.empty()) << it.analysis->reduction_error;
  const pp::Dataset alone = reduced_one_by_one(analyses);

  for (const bool include_j_error : {false, true}) {
    SCOPED_TRACE(include_j_error ? "with J error" : "without J error");
    const auto over_chain = pp::integrated_age(all(*chain), include_j_error);
    ASSERT_TRUE(over_chain) << over_chain.error().what;
    const auto over_alone = pp::integrated_age(all(alone), include_j_error);
    ASSERT_TRUE(over_alone) << over_alone.error().what;
    expect_relative(over_chain->nominal(), over_alone->nominal(), "nominal");
    expect_relative(over_chain->std_dev(), over_alone->std_dev(), "sigma");
    EXPECT_GT(over_chain->std_dev(), 0.0);

    // Every step has F = 10, so the total-gas age is any step's age.
    for (const auto& it : chain->items()) {
      const auto step = pp::Quantity::parse("age")->eval(*it.analysis);
      ASSERT_TRUE(step);
      expect_relative(over_chain->nominal(), step->value, "integrated age vs step age");
    }
  }
}
