#include "pychron/processing/quantity.hpp"

#include <gtest/gtest.h>

#include <cmath>

#include "fixtures.hpp"

namespace pp = pychron::processing;
namespace r = pychron::reduction;
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
  EXPECT_EQ(q("f").text(), "F");
  EXPECT_EQ(q("rad40_percent").text(), "radiogenic_yield");
  EXPECT_EQ(q("gain.H1").text(), "gain.H1");
  EXPECT_EQ(q("env.lab_temperature").text(), "env.lab_temperature");
  EXPECT_TRUE(q("Ar40/Ar36").is_ratio());
  EXPECT_TRUE(q("age").needs_reduction());
  EXPECT_FALSE(q("Ar40.intercept").needs_reduction());
}

TEST(Quantity, RejectsBadText) {
  for (const char* bad : {"", "Ar", "Ar40.nope", "foo", "Ar40/Ar36/Ar38", "gain.", "40Ar", "gain.H 1"})
    EXPECT_FALSE(pp::Quantity::parse(bad)) << bad;
}

TEST(Quantity, LabelsAndUnits) {
  EXPECT_EQ(q("Ar40").label(), "Ar40 (fA)");
  EXPECT_EQ(q("Ar40/Ar36").label(), "Ar40/Ar36");
  EXPECT_EQ(q("age").label(), "Age (Ma)");
  EXPECT_EQ(q("Ar40.intercept").label(), "Ar40 intercept (fA)");
  EXPECT_EQ(q("gain.H1").label(), "H1 gain");
}

TEST(Quantity, IsotopeStages) {
  auto ra = pp::reduce_analysis(make_air(0), {});
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
  auto ra = pp::reduce_analysis(make_air(0), {});
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

TEST(Quantity, ReducedValuesNeedAFlux) {
  auto air = pp::reduce_analysis(make_air(0), {});
  EXPECT_TRUE(air->arar);
  EXPECT_FALSE(q("age").eval(*air));  // no J
  EXPECT_TRUE(q("F").eval(*air));
  auto unk = pp::reduce_analysis(make_unknown(0), {});
  ASSERT_TRUE(unk->arar);
  const auto age = q("age").eval(*unk);
  ASSERT_TRUE(age);
  EXPECT_GT(age->value, 0.0);
  const auto with_j = q("age_w_j").eval(*unk);
  ASSERT_TRUE(with_j);
  EXPECT_GT(with_j->error, age->error);
  EXPECT_TRUE(q("radiogenic_yield").eval(*unk));
  EXPECT_TRUE(q("Ar39.decay_corrected").eval(*unk));
  EXPECT_TRUE(q("Ar36.interference_corrected").eval(*unk));
}

TEST(Quantity, MetadataAndMissing) {
  auto ra = pp::reduce_analysis(make_air(2), {});
  EXPECT_DOUBLE_EQ(q("extract_value").eval(*ra)->value, 7.0);
  EXPECT_DOUBLE_EQ(q("aliquot").eval(*ra)->value, 3.0);
  EXPECT_DOUBLE_EQ(q("gain.H1").eval(*ra)->value, 1.01);
  EXPECT_FALSE(q("gain.L5").eval(*ra));
  EXPECT_FALSE(q("Kr84").eval(*ra));
  EXPECT_FALSE(q("step_index").eval(*ra));
  EXPECT_FALSE(q("env.lab_temperature").eval(*ra));
}

TEST(Quantity, Available) {
  std::vector<pp::ReducedPtr> v{pp::reduce_analysis(make_air(0), {}), pp::reduce_analysis(make_unknown(1), {})};
  const auto names = pp::available_quantities(v);
  auto has = [&](const char* n) { return std::find(names.begin(), names.end(), n) != names.end(); };
  EXPECT_TRUE(has("age"));
  EXPECT_TRUE(has("Ar40/Ar36"));
  EXPECT_TRUE(has("Ar40/Ar39"));
  EXPECT_FALSE(has("Ar36/Ar40"));
  EXPECT_TRUE(has("gain.H1"));
  EXPECT_TRUE(has("Ar36.intercept"));
  for (const auto& n : names) EXPECT_TRUE(pp::Quantity::parse(n)) << n;
}

}  // namespace
