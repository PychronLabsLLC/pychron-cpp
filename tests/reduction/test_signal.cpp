// Neutral isotope signals (noble gas spec 3.3): tags carry the label passed in.
#include "pychron/reduction/signal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <set>
#include <string>
#include <vector>

#include "pychron/reduction/arar_types.hpp"

using namespace pychron::reduction;

namespace {

MeasuredSignal sample() {
  MeasuredSignal m;
  m.intercept = {12.5, 0.4};
  m.baseline = {0.3, 0.05};
  m.blank = {0.2, 0.03};
  m.ic_factor = {1.01, 0.002};
  m.include_baseline_error = true;
  m.correct_for_blank = true;
  return m;
}

std::set<std::string> tags_of(const UFloat& x) {
  std::set<std::string> out;
  for (const auto& [tag, sigma] : error_components(x)) out.emplace(tag_name(tag));
  return out;
}

}  // namespace

TEST(Signal, TagsCarryTheLabel) {
  const IsotopeSignal s = make_signal("He3", sample());
  EXPECT_EQ(tags_of(s.intercept), (std::set<std::string>{"He3"}));
  EXPECT_EQ(tags_of(s.baseline), (std::set<std::string>{"He3 bs"}));
  EXPECT_EQ(tags_of(s.blank), (std::set<std::string>{"He3 bk"}));
  EXPECT_EQ(tags_of(s.ic_factor), (std::set<std::string>{"He3 IC"}));

  // Every stage of the correction names the label and none names an argon isotope.
  const std::set<std::string> want{"He3", "He3 bs", "He3 bk", "He3 IC"};
  EXPECT_EQ(tags_of(corrected_intensity(s)), want);
  for (const std::string& t : tags_of(corrected_intensity(s))) {
    EXPECT_EQ(t.find("Ar40"), std::string::npos) << t;
  }
}

TEST(Signal, ArgonOverloadIsBitEqualToTheLabelOverload) {
  const MeasuredSignal m = sample();
  const IsotopeSignal a = make_signal(ArgonIsotope::Ar40, m);
  const IsotopeSignal b = make_signal("Ar40", m);
  const auto same = [](const UFloat& x, const UFloat& y) {
    return std::bit_cast<unsigned long long>(x.nominal()) ==
               std::bit_cast<unsigned long long>(y.nominal()) &&
           std::bit_cast<unsigned long long>(x.std_dev()) ==
               std::bit_cast<unsigned long long>(y.std_dev());
  };
  EXPECT_TRUE(same(a.intercept, b.intercept));
  EXPECT_TRUE(same(a.baseline, b.baseline));
  EXPECT_TRUE(same(a.blank, b.blank));
  EXPECT_TRUE(same(a.ic_factor, b.ic_factor));
  EXPECT_TRUE(same(corrected_intensity(a), corrected_intensity(b)));
  EXPECT_EQ(tags_of(corrected_intensity(a)), tags_of(corrected_intensity(b)));
  EXPECT_EQ(tags_of(a.intercept), (std::set<std::string>{"Ar40"}));
}
