#include "pychron/processing/quantity.hpp"

#include <gtest/gtest.h>

#include <cmath>

#include "fixtures.hpp"

namespace pp = pychron::processing;
using pp::test::make_air;
using pp::test::make_unknown;

namespace {

pp::Quantity q(const char* text) {
  auto out = pp::Quantity::parse(text);
  EXPECT_TRUE(out) << text << ": " << (out ? "" : out.error().what);
  return *out;
}

TEST(Quantity, ParsesAndCanonicalizes) {
  EXPECT_EQ(q("Ar40").text(), "Ar40");
  EXPECT_EQ(q(" Ar40.ic_corrected ").text(), "Ar40");
  EXPECT_EQ(q("Ar40.bs").text(), "Ar40.baseline");
  EXPECT_EQ(q("Ar40/Ar36").text(), "Ar40/Ar36");
  EXPECT_EQ(q("H1:Ar40.intercept").text(), "H1:Ar40.intercept");
  EXPECT_EQ(q("gain.H1").text(), "gain.H1");
  EXPECT_EQ(q("env.lab_temperature").text(), "env.lab_temperature");
  EXPECT_TRUE(q("Ar40/Ar36").is_ratio());
  EXPECT_TRUE(q("age").needs_reduction());
  EXPECT_FALSE(q("Ar40.intercept").needs_reduction());
}

// Malformed text, an isotope's stage that is not a name, or a family key that
// is not one. A bare name is a domain's (tests/arar: a domain's stage it does
// not know is refused when parse is given the domain).
TEST(Quantity, RejectsBadText) {
  for (const char* bad : {"", "Ar40/Ar36/Ar38", "gain.", "40Ar", "gain.H 1", "Ar40.", "Ar40.no pe", "x.decay_corrected",
                          "age/"})
    EXPECT_FALSE(pp::Quantity::parse(bad)) << bad;
}

TEST(Quantity, LabelsAndUnits) {
  EXPECT_EQ(q("Ar40").label(), "Ar40 (fA)");
  EXPECT_EQ(q("Ar40/Ar36").label(), "Ar40/Ar36");
  EXPECT_EQ(q("Ar40.intercept").label(), "Ar40 intercept (fA)");
  EXPECT_EQ(q("gain.H1").label(), "H1 gain");
}

TEST(Quantity, IsotopeStages) {
  auto ra = pp::reduce_analysis(make_air(0));
  const auto i = q("Ar40.intercept").eval(*ra);
  ASSERT_TRUE(i);
  EXPECT_DOUBLE_EQ(i->value, 2955.01);
  const auto bs = q("Ar40.bs_corrected").eval(*ra);
  ASSERT_TRUE(bs);
  EXPECT_NEAR(bs->value, 2955.0, 1e-9);
  // include_baseline_error is false: the baseline's error does not propagate.
  EXPECT_NEAR(bs->error, 0.5, 1e-12);
  EXPECT_NEAR(q("Ar40.baseline").eval(*ra)->value, 0.01, 1e-15);
  EXPECT_NEAR(q("Ar40").eval(*ra)->value, 2955.0, 1e-9);
}

TEST(Quantity, RatioPropagatesLikeUFloat) {
  auto ra = pp::reduce_analysis(make_air(0));
  const auto ratio = q("Ar40/Ar36").eval(*ra);
  ASSERT_TRUE(ratio);
  EXPECT_NEAR(ratio->value, 295.5, 1e-9);
  const double rel = std::hypot(0.5 / 2955.0, 0.02 / 10.0);
  EXPECT_NEAR(ratio->error, 295.5 * rel, 1e-9);
  // A ratio of a stage with itself is exact: the variables cancel.
  const auto self = q("Ar40.intercept/Ar40.intercept").eval(*ra);
  ASSERT_TRUE(self);
  EXPECT_DOUBLE_EQ(self->value, 1.0);
  EXPECT_NEAR(self->error, 0.0, 1e-15);
}

// Without a domain's reduction there are no domain values or stages, and no crash.
TEST(Quantity, DomainTermsHaveNoValueWithoutTheDomainsReduction) {
  auto unk = pp::reduce_analysis(make_unknown(0));
  EXPECT_EQ(unk->domain, nullptr);
  EXPECT_FALSE(q("age").eval(*unk));
  EXPECT_FALSE(q("j").eval(*unk));
  EXPECT_FALSE(q("Ar39.decay_corrected").eval(*unk));
  EXPECT_FALSE(q("Ar36.interference_corrected/Ar36").eval(*unk));
  EXPECT_TRUE(q("Ar39.decay_corrected").needs_reduction());
  EXPECT_EQ(q("Ar39.decay_corrected").text(), "Ar39.decay_corrected");
  // Without the domain at parse, a domain's term reads as written.
  EXPECT_EQ(q("Ar39.decay_corrected").label(), "Ar39 decay_corrected");
  EXPECT_EQ(q("age").label(), "age");
  EXPECT_EQ(q("age").units(), "");
}

// A bare name that is not core's is a domain's: it parses, and without the
// domain's reduction it has no value.
TEST(Quantity, ADomainNameHasNoValueWithoutADomain) {
  const auto age = pp::Quantity::parse("age");
  ASSERT_TRUE(age) << age.error().what;
  EXPECT_EQ(age->numerator().kind, pp::Term::Kind::Domain);
  EXPECT_EQ(age->numerator().name, "age");
  EXPECT_EQ(age->text(), "age");
  EXPECT_TRUE(age->needs_reduction());
  const auto core_only = pp::reduce_analysis(make_unknown(0));
  ASSERT_EQ(core_only->domain, nullptr);
  EXPECT_FALSE(age->eval(*core_only));
  EXPECT_FALSE(q("foo").eval(*core_only));
  EXPECT_FALSE(q("age/Ar39").eval(*core_only));
}

// An isotope with a stage core does not have is a domain's stage.
TEST(Quantity, ADomainStageParses) {
  const auto stage = pp::Quantity::parse("Ar40.interference_corrected");
  ASSERT_TRUE(stage) << stage.error().what;
  const pp::Term& t = stage->numerator();
  EXPECT_EQ(t.kind, pp::Term::Kind::Domain);
  EXPECT_EQ(t.key, "Ar40");
  EXPECT_EQ(t.domain_stage, "interference_corrected");
  EXPECT_TRUE(stage->needs_reduction());
  EXPECT_EQ(stage->text(), "Ar40.interference_corrected");
  // A core stage is not a domain's.
  EXPECT_EQ(q("Ar40.bs_corrected").numerator().kind, pp::Term::Kind::Isotope);
  EXPECT_FALSE(q("Ar40.bs_corrected").needs_reduction());
}

TEST(Quantity, MetadataAndMissing) {
  auto ra = pp::reduce_analysis(make_air(2));
  EXPECT_DOUBLE_EQ(q("extract_value").eval(*ra)->value, 7.0);
  EXPECT_DOUBLE_EQ(q("aliquot").eval(*ra)->value, 3.0);
  EXPECT_DOUBLE_EQ(q("gain.H1").eval(*ra)->value, 1.01);
  EXPECT_FALSE(q("gain.L5").eval(*ra));
  EXPECT_FALSE(q("Kr84").eval(*ra));
  EXPECT_FALSE(q("step_index").eval(*ra));
  EXPECT_FALSE(q("env.lab_temperature").eval(*ra));
}

TEST(Quantity, Available) {
  std::vector<pp::ReducedPtr> v{pp::reduce_analysis(make_air(0)), pp::reduce_analysis(make_unknown(1))};
  const auto names = pp::available_quantities(v, {});
  auto has = [&](const char* n) { return std::find(names.begin(), names.end(), n) != names.end(); };
  EXPECT_FALSE(has("age"));  // no domain's reduction: tests/arar test_quantity_arar.cpp has it
  EXPECT_TRUE(has("Ar40/Ar36"));
  EXPECT_TRUE(has("Ar40/Ar39"));
  EXPECT_FALSE(has("Ar36/Ar40"));
  EXPECT_TRUE(has("gain.H1"));
  EXPECT_TRUE(has("Ar36.intercept"));
  EXPECT_TRUE(has("Ar37/Ar36"));
  EXPECT_FALSE(has("Ar36/Ar37"));
  // A domain's names are listed only when an analysis has a value for them.
  EXPECT_EQ(pp::available_quantities(v, {"age", "decay_corrected"}), names);
  for (const auto& n : names) EXPECT_TRUE(pp::Quantity::parse(n)) << n;
}

}  // namespace
