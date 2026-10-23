// What one reduction gives the store's derived cache (age cache spec 4.1, 4.4).

#include "pychron/arar/derived.hpp"

#include <gtest/gtest.h>

#include <limits>

#include "fixtures.hpp"
#include "pychron/arar/reduce.hpp"
#include "pychron/processing/reduced.hpp"

namespace pp = pychron::processing;
namespace r = pychron::reduction;
using pp::test::make_air;
using pp::test::make_unknown;
namespace ar = pychron::arar;

namespace {

const std::string kFingerprint(64, 'a');

// The value of `name` in `d`, when it has one.
std::optional<r::UFloat> dv(const pp::DerivedValues& d, std::string_view name) {
  const pp::DerivedValue* v = pp::find_derived(d, name);
  return v ? v->value : std::nullopt;
}

pp::ReducedPtr reduced_unknown(r::AgeUnits units = r::AgeUnits::Ma) {
  auto a = make_unknown(0);
  a->input_fingerprint = kFingerprint;
  if (units != r::AgeUnits::Ma) {
    auto c = r::constants_preset(r::ConstantsPreset::Default);
    c.age_units = units;
    a->context.constants = c;
  }
  return ar::reduce_with_arar(a);
}

// The Ar-Ar result of a reduction.
const ar::ArArReduced* res(const pp::ReducedPtr& reduced) { return ar::reduced_of(*reduced); }

}  // namespace

TEST(DerivedAge, AnUnknownWithFluxGivesItsAgesInMa) {
  const auto reduced = reduced_unknown();
  ASSERT_TRUE(res(reduced) && res(reduced)->arar.ages) << reduced->reduction_error;
  const auto d = pp::derived_age(*reduced);
  ASSERT_TRUE(d);
  EXPECT_EQ(d->uuid, reduced->analysis->uuid);
  EXPECT_EQ(d->input_fingerprint, kFingerprint);
  ASSERT_TRUE(dv(*d, "age") && dv(*d, "age_w_j"));
  EXPECT_DOUBLE_EQ(dv(*d, "age")->nominal(), res(reduced)->arar.ages->age.nominal());
  EXPECT_DOUBLE_EQ(dv(*d, "age")->std_dev(), res(reduced)->arar.ages->age.std_dev());
  EXPECT_DOUBLE_EQ(dv(*d, "age_w_j")->nominal(), res(reduced)->arar.ages->age_w_j_err.nominal());
  EXPECT_DOUBLE_EQ(dv(*d, "age_w_j")->std_dev(), res(reduced)->arar.ages->age_w_j_err.std_dev());
  EXPECT_GT(dv(*d, "age_w_j")->std_dev(), dv(*d, "age")->std_dev());  // J's error is in the one and not in the other
  ASSERT_TRUE(res(reduced)->arar.kca);
  ASSERT_TRUE(dv(*d, "kca"));
  EXPECT_DOUBLE_EQ(dv(*d, "kca")->nominal(), res(reduced)->arar.kca->nominal());
}

// The cache and the browser's filter speak Ma, whatever a lab displays in.
TEST(DerivedAge, AgesAreInMaWhateverTheConstantsSay) {
  const auto ma = pp::derived_age(*reduced_unknown());
  const auto in_ka = reduced_unknown(r::AgeUnits::ka);
  ASSERT_TRUE(res(in_ka) && res(in_ka)->arar.ages) << in_ka->reduction_error;
  const auto d = pp::derived_age(*in_ka);
  ASSERT_TRUE(ma && dv(*ma, "age") && d && dv(*d, "age") && dv(*d, "age_w_j"));
  EXPECT_NEAR(res(in_ka)->arar.ages->age.nominal(), 1000.0 * dv(*ma, "age")->nominal(), 1e-9 * res(in_ka)->arar.ages->age.nominal());
  EXPECT_NEAR(dv(*d, "age")->nominal(), dv(*ma, "age")->nominal(), 1e-12 * dv(*ma, "age")->nominal());
  EXPECT_NEAR(dv(*d, "age")->std_dev(), dv(*ma, "age")->std_dev(), 1e-9 * dv(*ma, "age")->std_dev());
  EXPECT_NEAR(dv(*d, "age_w_j")->nominal(), dv(*ma, "age_w_j")->nominal(), 1e-12 * dv(*ma, "age_w_j")->nominal());
  // K/Ca is no age: untouched
  ASSERT_TRUE(dv(*d, "kca") && dv(*ma, "kca"));
  EXPECT_DOUBLE_EQ(dv(*d, "kca")->nominal(), dv(*ma, "kca")->nominal());
}

// "Current, and there is no age": the cache is told so, and does not come back to it.
TEST(DerivedAge, AnAnalysisThatReducesToNoAgeIsADerivedAgeWithoutOne) {
  auto air = make_air(0);
  air->input_fingerprint = kFingerprint;
  const auto reduced = ar::reduce_with_arar(air);
  const auto d = pp::derived_age(*reduced);
  ASSERT_TRUE(d);
  EXPECT_EQ(d->uuid, air->uuid);
  EXPECT_EQ(d->input_fingerprint, kFingerprint);
  EXPECT_FALSE(dv(*d, "age"));
  EXPECT_FALSE(dv(*d, "age_w_j"));
  EXPECT_FALSE(dv(*d, "kca"));

  // an unknown with no flux, likewise
  auto unknown = make_unknown(1);
  unknown->input_fingerprint = kFingerprint;
  unknown->context.flux.reset();
  const auto no_flux = pp::derived_age(*ar::reduce_with_arar(unknown));
  ASSERT_TRUE(no_flux);
  EXPECT_FALSE(dv(*no_flux, "age"));
}

// A row of the cache is a number and its error: what is neither is left out.
TEST(DerivedAge, AValueThatIsNoNumberIsLeftOut) {
  auto a = make_unknown(0);
  a->input_fingerprint = kFingerprint;
  pp::ReducedAnalysis reduced = *ar::reduce_with_arar(a);
  ASSERT_NE(ar::reduced_of(reduced), nullptr);
  auto changed = std::make_shared<ar::ArArReduced>(*ar::reduced_of(reduced));
  ASSERT_TRUE(changed->arar.ages && changed->arar.kca);
  reduced.domain = changed;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  changed->arar.kca = r::UFloat(nan);
  changed->arar.ages->age_w_j_err = r::UFloat(std::numeric_limits<double>::infinity());
  auto d = pp::derived_age(reduced);
  ASSERT_TRUE(d);
  EXPECT_TRUE(dv(*d, "age"));
  EXPECT_FALSE(dv(*d, "age_w_j"));
  EXPECT_FALSE(dv(*d, "kca"));
  // and an age that is none is no age at all
  changed->arar.ages->age = r::UFloat(nan);
  d = pp::derived_age(reduced);
  ASSERT_TRUE(d);
  EXPECT_FALSE(dv(*d, "age"));
  EXPECT_FALSE(dv(*d, "age_w_j"));
  EXPECT_FALSE(dv(*d, "kca"));
}

// A source that keeps no fingerprint (a record directory, memory) has no cache.
TEST(DerivedAge, WithoutAFingerprintThereIsNothingToCache) {
  auto a = make_unknown(0);
  ASSERT_TRUE(a->input_fingerprint.empty());
  EXPECT_FALSE(pp::derived_age(*ar::reduce_with_arar(a)));
  EXPECT_FALSE(pp::derived_age(pp::ReducedAnalysis{}));  // no analysis at all
}

// The fingerprint covers the inputs and the reduction version, not a figure's
// settings: an age computed under other settings is that figure's own.
TEST(DerivedAge, OnlyAReductionWithTheDefaultSettingsIsCached) {
  for (const ar::ArArSettings settings : {ar::ArArSettings{r::ConstantsPreset::Legacy, false, false},
                                          ar::ArArSettings{r::ConstantsPreset::Default, true, false},
                                          ar::ArArSettings{r::ConstantsPreset::Default, false, true}}) {
    auto a = make_unknown(0);
    a->input_fingerprint = kFingerprint;
    EXPECT_FALSE(pp::derived_age(*ar::reduce_with_arar(a, settings), settings));
  }
}
