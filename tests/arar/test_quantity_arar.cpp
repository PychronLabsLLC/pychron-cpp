// Quantities of the Ar-Ar domain's values and stages (domain seam spec 3.4):
// they evaluate through the ArArReduced that arar_reduce attaches.

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "fixtures.hpp"
#include "pychron/arar/reduce.hpp"
#include "pychron/processing/quantity.hpp"

namespace pp = pychron::processing;
using pp::test::make_air;
using pp::test::make_unknown;
using pychron::arar::reduce_with_arar;

namespace {

pp::Quantity q(const char* text) {
  auto out = pp::Quantity::parse(text);
  EXPECT_TRUE(out) << text << ": " << (out ? "" : out.error().what);
  return *out;
}

}  // namespace

TEST(ArArQuantity, ReducedValuesNeedAFlux) {
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

TEST(ArArQuantity, Available) {
  std::vector<pp::ReducedPtr> v{reduce_with_arar(make_air(0)), reduce_with_arar(make_unknown(1))};
  const auto names = pp::available_quantities(v);
  auto has = [&](const char* n) { return std::find(names.begin(), names.end(), n) != names.end(); };
  EXPECT_TRUE(has("age"));
  EXPECT_TRUE(has("kca"));
  EXPECT_TRUE(has("Ar40/Ar36"));
  EXPECT_TRUE(has("Ar40/Ar39"));
  EXPECT_FALSE(has("Ar36/Ar40"));
  EXPECT_TRUE(has("gain.H1"));
  EXPECT_TRUE(has("Ar36.intercept"));
  for (const auto& n : names) EXPECT_TRUE(pp::Quantity::parse(n)) << n;
}
