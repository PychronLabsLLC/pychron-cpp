// The Ar-Ar recall sections (domain seam spec 5.3): the computed values and
// the corrected ratios the recall window shows under Summary, and the age's
// error budget, computed by the domain from the Ar-Ar result.

#include <gtest/gtest.h>

#include <string>

#include "fixtures.hpp"
#include "pychron/arar/domain.hpp"
#include "pychron/arar/reduce.hpp"
#include "pychron/processing/domain.hpp"
#include "pychron/processing/recall.hpp"

namespace pp = pychron::processing;
using pp::test::make_air;
using pp::test::make_unknown;

namespace {

const pp::RecallRow* row(const pp::RecallSection& s, const std::string& name) {
  for (const auto& r : s.rows)
    if (r.name == name) return &r;
  return nullptr;
}

}  // namespace

TEST(ArArRecall, SectionsAndBudgetMatchTodaysModel) {
  const auto domain = pychron::arar::make_domain();

  // An unknown with a flux: its ages, its ratios, and the budget of its age.
  const auto unknown = pychron::arar::reduce_with_arar(make_unknown(0));
  const auto sections = domain->recall_sections(*unknown);
  ASSERT_EQ(sections.size(), 2u);
  const pp::RecallSection& computed = sections[0];
  const pp::RecallSection& ratios = sections[1];
  EXPECT_EQ(computed.title, "Computed");
  EXPECT_EQ(ratios.title, "Corrected ratios");
  ASSERT_NE(row(computed, "Age"), nullptr);
  ASSERT_TRUE(row(computed, "Age")->value);
  EXPECT_GT(row(computed, "Age")->value->value, 0.0);
  ASSERT_TRUE(row(computed, "Age w/o J")->value);
  EXPECT_LT(row(computed, "Age w/o J")->value->error, row(computed, "Age")->value->error);
  for (const char* name : {"Age w/o J, irrad.", "F (40Ar*/39ArK)", "K/Ca", "K/Cl", "40Ar*", "%40Ar*", "39ArK"})
    EXPECT_NE(row(computed, name), nullptr) << name;
  ASSERT_NE(row(ratios, "Ar40/Ar36"), nullptr);
  const auto budget = domain->error_budget(*unknown);
  ASSERT_FALSE(budget.empty());
  double total = 0;
  for (const auto& c : budget) total += c.percent;
  EXPECT_NEAR(total, 100.0, 1e-6);
  EXPECT_GE(budget.front().percent, budget.back().percent);

  // An air without a flux: no age, and why; its ratios; no budget.
  const auto air = pychron::arar::reduce_with_arar(make_air(0));
  const auto air_sections = domain->recall_sections(*air);
  ASSERT_EQ(air_sections.size(), 2u);
  ASSERT_NE(row(air_sections[0], "Age"), nullptr);
  EXPECT_FALSE(row(air_sections[0], "Age")->value);
  EXPECT_EQ(row(air_sections[0], "Age")->note, "no J for this analysis");
  ASSERT_NE(row(air_sections[1], "Ar40/Ar36"), nullptr);
  EXPECT_NEAR(row(air_sections[1], "Ar40/Ar36")->value->value, 295.5, 1e-9);
  EXPECT_TRUE(domain->error_budget(*air).empty());

  // The neutral model keeps what is the analysis's own, and the reason there is no Ar-Ar result.
  const auto model = pp::make_recall_model(*unknown);
  EXPECT_EQ(model.title, "U1-01  air  unknown");
  ASSERT_EQ(model.isotopes.size(), 5u);
  EXPECT_EQ(model.isotopes[0].stages.rbegin()->first, pp::Stage::IcCorrected);
  EXPECT_TRUE(model.reduction_note.empty());
  pychron::arar::ArArSettings settings;
  auto helium = make_air(0);
  helium->isotopes.resize(0);
  const auto none = pychron::arar::reduce_with_arar(helium, settings);
  EXPECT_EQ(pp::make_recall_model(*none).reduction_note, "no argon isotopes to reduce");
  EXPECT_TRUE(domain->error_budget(*none).empty());
}
