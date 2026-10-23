// Quantities of the Ar-Ar domain's values and stages (domain seam spec 3.4):
// parsed with the domain, they read as the axes always have; they evaluate
// through the ArArReduced that arar_reduce attaches.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "arar_fixtures.hpp"
#include "pychron/arar/domain.hpp"
#include "pychron/arar/reduce.hpp"
#include "pychron/processing/domain.hpp"
#include "pychron/processing/quantity.hpp"

namespace pp = pychron::processing;
using pychron::arar::test::make_air;
using pychron::arar::test::make_unknown;
using pychron::arar::reduce_with_arar;

namespace {

const pp::Domain& arar() {
  static const auto domain = pychron::arar::make_domain();
  return *domain;
}

pp::Quantity q(const char* text) {
  auto out = pp::Quantity::parse(text, &arar());
  EXPECT_TRUE(out) << text << ": " << (out ? "" : out.error().what);
  return *out;
}

// Relative: the values were written down with %.17g from the quantities as
// they were before the domain seam (core's reduce, then arar_reduce).
void expect_value(const char* text, const pp::ReducedAnalysis& ra, double value, double error) {
  const auto v = q(text).eval(ra);
  ASSERT_TRUE(v) << text;
  EXPECT_NEAR(v->value, value, std::abs(value) * 1e-12) << text;
  EXPECT_NEAR(v->error, error, std::abs(error) * 1e-12) << text;
}

}  // namespace

TEST(ArArQuantities, AgeAndRatiosEvaluateAsBefore) {
  const auto unk = reduce_with_arar(make_unknown(0));
  ASSERT_TRUE(pychron::arar::reduced_of(*unk));
  expect_value("age", *unk, 15.33748658835945, 0.056334020824722914);
  expect_value("age_w_j", *unk, 15.33748658835945, 0.058367545629637821);
  expect_value("age_w_position", *unk, 15.33748658835945, 0.056334020824722914);
  expect_value("F", *unk, 8.5378097809780975, 0.031492548945205631);
  expect_value("f", *unk, 8.5378097809780975, 0.031492548945205631);
  expect_value("kca", *unk, 204.0612244897959, 4.1695122870048911);
  expect_value("cak", *unk, 0.0049004900490049012, 0.00010013001501268984);
  expect_value("kcl", *unk, 52.706355475964898, 0.2876735367351031);
  expect_value("clk", *unk, 0.018973043971063776, 0.00010355568341801801);
  expect_value("radiogenic_yield", *unk, 85.370413704137036, 0.2990386335775731);
  expect_value("rad40_percent", *unk, 85.370413704137036, 0.2990386335775731);
  expect_value("rad40", *unk, 853.69560000000001, 3.0309867980576892);
  expect_value("k39", *unk, 99.989999999999995, 0.10000000000000001);
  expect_value("j", *unk, 0.001, 9.9999999999999995e-07);
  expect_value("Ar39.decay_corrected", *unk, 99.989999999999995, 0.10000000000000001);
  expect_value("Ar36.interference_corrected", *unk, 0.48999999999999999, 0.01);
  expect_value("Ar40.decay_corrected/Ar39.decay_corrected", *unk, 10.000900090009001, 0.011182263186041741);
  // `j` is a variable of its own each time: it does not cancel the ages' J.
  expect_value("age/j", *unk, 15337.48658835945, 58.384590408158381);
  expect_value("age_w_j/j", *unk, 15337.48658835945, 60.349058631232666);
  expect_value("Ar40/Ar39", *unk, 10.000900090009001, 0.011182263186041741);
  expect_value("F/kca", *unk, 0.041839451871979877, 0.00087072260502060901);

  // An air has no J: no ages, the ratios stand.
  const auto air = reduce_with_arar(make_air(0));
  ASSERT_TRUE(pychron::arar::reduced_of(*air));
  for (const char* none : {"age", "age_w_j", "age_w_position", "j", "age/j"}) EXPECT_FALSE(q(none).eval(*air)) << none;
  expect_value("F", *air, -30.909090909090818, 6.8217922823820727);
  expect_value("kca", *air, 2.0204081632653059, 0.046006940181365738);
  expect_value("cak", *air, 0.49494949494949497, 0.011270550288282742);
  expect_value("kcl", *air, 10.489404641776, 1.2423265988112837);
  expect_value("clk", *air, 0.095334295334295188, 0.011291044145730705);
  expect_value("radiogenic_yield", *air, -1.0355329949238548, 0.22832092297852566);
  expect_value("rad40", *air, -30.599999999999909, 6.7464975683683459);
  expect_value("k39", *air, 0.98999999999999999, 0.01);
  expect_value("Ar39.decay_corrected", *air, 0.98999999999999999, 0.01);
  expect_value("Ar36.interference_corrected", *air, 10, 0.02);
  expect_value("Ar40.decay_corrected/Ar39.decay_corrected", *air, 2984.848484848485, 30.154214517176076);

  // Without the domain at parse the same text evaluates to the same values:
  // the domain only gives the labels.
  const auto bare = pp::Quantity::parse("age_w_j");
  ASSERT_TRUE(bare);
  const auto with = q("age_w_j").eval(*unk);
  ASSERT_TRUE(bare->eval(*unk) && with);
  EXPECT_DOUBLE_EQ(bare->eval(*unk)->value, with->value);
  EXPECT_DOUBLE_EQ(bare->eval(*unk)->error, with->error);
}

// The axis titles of figures and the browser: what the table of quantity.cpp
// gave before the domain seam, now the domain's.
TEST(ArArQuantities, LabelsAndUnitsAreTheAxisTitles) {
  struct Row {
    const char* text;
    const char* canonical;
    const char* label;
    const char* units;
  };
  const std::array<Row, 21> rows{{
      {"age", "age", "Age (Ma)", "Ma"},
      {"age_w_j", "age_w_j", "Age (with J error) (Ma)", "Ma"},
      {"age_w_position", "age_w_position", "Age (with position error) (Ma)", "Ma"},
      {"F", "F", "40Ar*/39ArK", ""},
      {"f", "F", "40Ar*/39ArK", ""},
      {"kca", "kca", "K/Ca", ""},
      {"cak", "cak", "Ca/K", ""},
      {"kcl", "kcl", "K/Cl", ""},
      {"clk", "clk", "Cl/K", ""},
      {"radiogenic_yield", "radiogenic_yield", "%40Ar* (%)", "%"},
      {"rad40_percent", "radiogenic_yield", "%40Ar* (%)", "%"},
      {"rad40", "rad40", "40Ar* (fA)", "fA"},
      {"k39", "k39", "39ArK (fA)", "fA"},
      {"j", "j", "J", ""},
      {"Ar39.decay_corrected", "Ar39.decay_corrected", "Ar39 decay corrected (fA)", "fA"},
      {"Ar36.interference_corrected", "Ar36.interference_corrected", "Ar36 interference corrected (fA)", "fA"},
      {"Ar40.decay_corrected/Ar39.decay_corrected", "Ar40.decay_corrected/Ar39.decay_corrected",
       "Ar40 decay corrected/Ar39 decay corrected", ""},
      {"age/j", "age/j", "Age/J", ""},
      {"age_w_j/j", "age_w_j/j", "Age (with J error)/J", ""},
      {"F/kca", "F/kca", "40Ar*/39ArK/K/Ca", ""},
      {"Ar40/Ar39", "Ar40/Ar39", "Ar40/Ar39", ""},
  }};
  for (const auto& row : rows) {
    const auto quantity = q(row.text);
    EXPECT_EQ(quantity.text(), row.canonical) << row.text;
    EXPECT_EQ(quantity.label(), row.label) << row.text;
    EXPECT_EQ(quantity.units(), row.units) << row.text;
    EXPECT_TRUE(quantity.needs_reduction() || std::string(row.text) == "Ar40/Ar39") << row.text;
  }
  EXPECT_EQ(q("age").label(false), "Age");
  EXPECT_EQ(q("radiogenic_yield").label(false), "%40Ar*");
}

// With the domain, an isotope's stage is core's or the domain's, and a bare
// name is core's or the domain's: anything else is refused, naming it.
TEST(ArArQuantities, TheDomainKnowsItsStages) {
  EXPECT_FALSE(pp::Quantity::parse("Ar40.nope", &arar()));
  EXPECT_FALSE(pp::Quantity::parse("Ar40.age", &arar()));
  EXPECT_FALSE(pp::Quantity::parse("decay_corrected", &arar()));  // a stage is not a value
  EXPECT_FALSE(pp::Quantity::parse("age/foo", &arar()));
}

// A typo of a domain's name is refused when the domain is known; without it
// the name is taken as the domain's, and has no value.
TEST(ArArQuantities, AnUnknownNameIsRefusedUnderTheDomain) {
  const auto typo = pp::Quantity::parse("agee", &arar());
  ASSERT_FALSE(typo);
  EXPECT_NE(typo.error().what.find("unknown term 'agee'"), std::string::npos) << typo.error().what;

  const auto bare = pp::Quantity::parse("agee");
  ASSERT_TRUE(bare) << bare.error().what;
  EXPECT_EQ(bare->numerator().kind, pp::Term::Kind::Domain);
  const auto unk = reduce_with_arar(make_unknown(0));
  EXPECT_FALSE(bare->eval(*unk));
}

TEST(ArArQuantities, ReducedValuesNeedAFlux) {
  auto air = reduce_with_arar(make_air(0));
  EXPECT_TRUE(pychron::arar::reduced_of(*air));
  EXPECT_FALSE(q("age").eval(*air));  // no J
  EXPECT_TRUE(q("F").eval(*air));
  auto unk = reduce_with_arar(make_unknown(0));
  ASSERT_TRUE(pychron::arar::reduced_of(*unk));
  const auto age = q("age").eval(*unk);
  ASSERT_TRUE(age);
  EXPECT_GT(age->value, 0.0);
  const auto with_j = q("age_w_j").eval(*unk);
  ASSERT_TRUE(with_j);
  EXPECT_GT(with_j->error, age->error);
  EXPECT_TRUE(q("radiogenic_yield").eval(*unk));
  EXPECT_TRUE(q("Ar39.decay_corrected").eval(*unk));
  EXPECT_TRUE(q("Ar36.interference_corrected").eval(*unk));
  EXPECT_TRUE(q("j").eval(*unk));
}

// The names the domain offers come from the quantity table: each has its
// label and units, and the list is the one offered before it was built from
// the table (J is not offered).
TEST(ArArQuantities, TheOfferedNamesAreTheTablesRows) {
  const auto names = arar().quantities();
  EXPECT_EQ(names, (std::vector<std::string>{"age", "age_w_j", "age_w_position", "F", "kca", "cak", "kcl", "clk",
                                             "radiogenic_yield", "rad40", "k39", "decay_corrected",
                                             "interference_corrected"}));
  for (const auto& n : names) EXPECT_TRUE(arar().quantity_info(n)) << n;
  EXPECT_TRUE(arar().quantity_info("j"));
}

TEST(ArArQuantities, Available) {
  std::vector<pp::ReducedPtr> v{reduce_with_arar(make_air(0)), reduce_with_arar(make_unknown(1))};
  const auto names = pp::available_quantities(v, arar().quantities());
  auto has = [&](const char* n) { return std::find(names.begin(), names.end(), n) != names.end(); };
  EXPECT_TRUE(has("age"));
  EXPECT_TRUE(has("age_w_j"));
  EXPECT_TRUE(has("kca"));
  EXPECT_TRUE(has("F"));
  EXPECT_TRUE(has("Ar39.decay_corrected"));
  EXPECT_TRUE(has("Ar36.interference_corrected"));
  EXPECT_FALSE(has("decay_corrected"));
  EXPECT_TRUE(has("Ar40/Ar36"));
  EXPECT_TRUE(has("Ar40/Ar39"));
  EXPECT_FALSE(has("Ar36/Ar40"));
  EXPECT_TRUE(has("gain.H1"));
  EXPECT_TRUE(has("Ar36.intercept"));
  for (const auto& n : names) EXPECT_TRUE(pp::Quantity::parse(n, &arar())) << n;
  // Without the domain's names: core's list only.
  const auto core = pp::available_quantities(v, {});
  EXPECT_EQ(std::find(core.begin(), core.end(), "age"), core.end());
}
