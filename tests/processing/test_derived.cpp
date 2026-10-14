// What one reduction gives the store's derived cache (age cache spec 4.1, 4.4).

#include "pychron/processing/derived.hpp"

#include <gtest/gtest.h>

#include <limits>

#include "fixtures.hpp"
#include "pychron/processing/reduced.hpp"

namespace pp = pychron::processing;
namespace r = pychron::reduction;
using pp::test::make_air;
using pp::test::make_unknown;

namespace {

const std::string kFingerprint(64, 'a');

pp::ReducedPtr reduced_unknown(r::AgeUnits units = r::AgeUnits::Ma) {
  auto a = make_unknown(0);
  a->input_fingerprint = kFingerprint;
  if (units != r::AgeUnits::Ma) {
    auto c = r::constants_preset(r::ConstantsPreset::Default);
    c.age_units = units;
    a->context.constants = c;
  }
  return pp::reduce_analysis(a, pp::ReductionSettings{});
}

}  // namespace

TEST(DerivedAge, AnUnknownWithFluxGivesItsAgesInMa) {
  const auto reduced = reduced_unknown();
  ASSERT_TRUE(reduced->arar && reduced->arar->ages) << reduced->reduction_error;
  const auto d = pp::derived_age(*reduced, pp::ReductionSettings{});
  ASSERT_TRUE(d);
  EXPECT_EQ(d->uuid, reduced->analysis->uuid);
  EXPECT_EQ(d->input_fingerprint, kFingerprint);
  ASSERT_TRUE(d->age && d->age_w_j);
  EXPECT_DOUBLE_EQ(d->age->nominal(), reduced->arar->ages->age.nominal());
  EXPECT_DOUBLE_EQ(d->age->std_dev(), reduced->arar->ages->age.std_dev());
  EXPECT_DOUBLE_EQ(d->age_w_j->nominal(), reduced->arar->ages->age_w_j_err.nominal());
  EXPECT_DOUBLE_EQ(d->age_w_j->std_dev(), reduced->arar->ages->age_w_j_err.std_dev());
  EXPECT_GT(d->age_w_j->std_dev(), d->age->std_dev());  // J's error is in the one and not in the other
  ASSERT_TRUE(reduced->arar->kca);
  ASSERT_TRUE(d->kca);
  EXPECT_DOUBLE_EQ(d->kca->nominal(), reduced->arar->kca->nominal());
}

// The cache and the browser's filter speak Ma, whatever a lab displays in.
TEST(DerivedAge, AgesAreInMaWhateverTheConstantsSay) {
  const auto ma = pp::derived_age(*reduced_unknown(), pp::ReductionSettings{});
  const auto in_ka = reduced_unknown(r::AgeUnits::ka);
  ASSERT_TRUE(in_ka->arar && in_ka->arar->ages) << in_ka->reduction_error;
  const auto d = pp::derived_age(*in_ka, pp::ReductionSettings{});
  ASSERT_TRUE(ma && ma->age && d && d->age && d->age_w_j);
  EXPECT_NEAR(in_ka->arar->ages->age.nominal(), 1000.0 * ma->age->nominal(), 1e-9 * in_ka->arar->ages->age.nominal());
  EXPECT_NEAR(d->age->nominal(), ma->age->nominal(), 1e-12 * ma->age->nominal());
  EXPECT_NEAR(d->age->std_dev(), ma->age->std_dev(), 1e-9 * ma->age->std_dev());
  EXPECT_NEAR(d->age_w_j->nominal(), ma->age_w_j->nominal(), 1e-12 * ma->age_w_j->nominal());
  // K/Ca is no age: untouched
  ASSERT_TRUE(d->kca && ma->kca);
  EXPECT_DOUBLE_EQ(d->kca->nominal(), ma->kca->nominal());
}

// "Current, and there is no age": the cache is told so, and does not come back to it.
TEST(DerivedAge, AnAnalysisThatReducesToNoAgeIsADerivedAgeWithoutOne) {
  auto air = make_air(0);
  air->input_fingerprint = kFingerprint;
  const auto reduced = pp::reduce_analysis(air, pp::ReductionSettings{});
  const auto d = pp::derived_age(*reduced, pp::ReductionSettings{});
  ASSERT_TRUE(d);
  EXPECT_EQ(d->uuid, air->uuid);
  EXPECT_EQ(d->input_fingerprint, kFingerprint);
  EXPECT_FALSE(d->age);
  EXPECT_FALSE(d->age_w_j);
  EXPECT_FALSE(d->kca);

  // an unknown with no flux, likewise
  auto unknown = make_unknown(1);
  unknown->input_fingerprint = kFingerprint;
  unknown->context.flux.reset();
  const auto no_flux = pp::derived_age(*pp::reduce_analysis(unknown, pp::ReductionSettings{}), pp::ReductionSettings{});
  ASSERT_TRUE(no_flux);
  EXPECT_FALSE(no_flux->age);
}

// A row of the cache is a number and its error: what is neither is left out.
TEST(DerivedAge, AValueThatIsNoNumberIsLeftOut) {
  auto a = make_unknown(0);
  a->input_fingerprint = kFingerprint;
  pp::ReducedAnalysis reduced = *pp::reduce_analysis(a, pp::ReductionSettings{});
  ASSERT_TRUE(reduced.arar && reduced.arar->ages && reduced.arar->kca);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  reduced.arar->kca = r::UFloat(nan);
  reduced.arar->ages->age_w_j_err = r::UFloat(std::numeric_limits<double>::infinity());
  auto d = pp::derived_age(reduced, pp::ReductionSettings{});
  ASSERT_TRUE(d);
  EXPECT_TRUE(d->age);
  EXPECT_FALSE(d->age_w_j);
  EXPECT_FALSE(d->kca);
  // and an age that is none is no age at all
  reduced.arar->ages->age = r::UFloat(nan);
  d = pp::derived_age(reduced, pp::ReductionSettings{});
  ASSERT_TRUE(d);
  EXPECT_FALSE(d->age);
  EXPECT_FALSE(d->age_w_j);
  EXPECT_FALSE(d->kca);
}

// A source that keeps no fingerprint (a record directory, memory) has no cache.
TEST(DerivedAge, WithoutAFingerprintThereIsNothingToCache) {
  auto a = make_unknown(0);
  ASSERT_TRUE(a->input_fingerprint.empty());
  EXPECT_FALSE(pp::derived_age(*pp::reduce_analysis(a, pp::ReductionSettings{}), pp::ReductionSettings{}));
  EXPECT_FALSE(pp::derived_age(pp::ReducedAnalysis{}, pp::ReductionSettings{}));  // no analysis at all
}

// The fingerprint covers the inputs and the reduction version, not a figure's
// settings: an age computed under other settings is that figure's own.
TEST(DerivedAge, OnlyAReductionWithTheDefaultSettingsIsCached) {
  for (pp::ReductionSettings settings : {pp::ReductionSettings{r::ConstantsPreset::Legacy, false, false},
                                         pp::ReductionSettings{r::ConstantsPreset::Default, true, false},
                                         pp::ReductionSettings{r::ConstantsPreset::Default, false, true}}) {
    auto a = make_unknown(0);
    a->input_fingerprint = kFingerprint;
    EXPECT_FALSE(pp::derived_age(*pp::reduce_analysis(a, settings), settings));
  }
}
