#include "pychron/systems/spectrometer/molecular_weights.hpp"

#include <gtest/gtest.h>

namespace ps = pychron::spectrometer;
using pychron::ErrorKind;

TEST(MolecularWeights, DefaultsCoverArgon) {
  const auto& mw = ps::MolecularWeights::defaults();
  for (const char* iso : {"Ar36", "Ar37", "Ar38", "Ar39", "Ar40"}) EXPECT_TRUE(mw.contains(iso)) << iso;
  EXPECT_NEAR(*mw.mass("Ar40"), 39.9623831, 1e-6);
  EXPECT_NEAR(*mw.mass("Ar36"), 35.9675451, 1e-6);
  EXPECT_TRUE(mw.contains("He4"));
  EXPECT_TRUE(mw.contains("Kr84"));
  EXPECT_TRUE(mw.contains("Xe132"));
}

TEST(MolecularWeights, UnknownIsotopeIsConfigError) {
  auto m = ps::MolecularWeights::defaults().mass("Ar41x");
  ASSERT_FALSE(m.has_value());
  EXPECT_EQ(m.error().kind, ErrorKind::Config);
}

TEST(MolecularWeights, ParsesFlatToml) {
  auto mw = ps::parse_molecular_weights("Ar40 = 39.9623831\nHCl = 36\n");
  ASSERT_TRUE(mw.has_value()) << pychron::to_string(mw.error());
  EXPECT_DOUBLE_EQ(*mw->mass("Ar40"), 39.9623831);
  EXPECT_DOUBLE_EQ(*mw->mass("HCl"), 36.0);  // integers accepted
}

TEST(MolecularWeights, RejectsNonNumericAndMalformed) {
  EXPECT_EQ(ps::parse_molecular_weights("Ar40 = \"heavy\"").error().kind, ErrorKind::Config);
  EXPECT_EQ(ps::parse_molecular_weights("Ar40 = ").error().kind, ErrorKind::Config);
  EXPECT_EQ(ps::parse_molecular_weights("Ar40 = -1.0").error().kind, ErrorKind::Config);
}

TEST(MolecularWeights, TomlRoundTrip) {
  auto back = ps::parse_molecular_weights(ps::to_toml(ps::MolecularWeights::defaults()));
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->entries(), ps::MolecularWeights::defaults().entries());
}

TEST(MolecularWeights, MergedOverridesAndExtends) {
  ps::MolecularWeights extra({{"Ar40", 40.0}, {"Foo", 12.5}});
  auto m = ps::MolecularWeights::defaults().merged(extra);
  EXPECT_DOUBLE_EQ(*m.mass("Ar40"), 40.0);
  EXPECT_DOUBLE_EQ(*m.mass("Foo"), 12.5);
  EXPECT_TRUE(m.contains("Ar36"));
}
