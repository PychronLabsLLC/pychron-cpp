#include <gtest/gtest.h>

#include <cmath>

#include "pychron/reduction/arar.hpp"

using namespace pychron::reduction;

TEST(ArAr, HandComputedFixture) {
  ArArConstants c;
  c.atm4036 = 298.56;
  c.ca3637 = 2.7e-4;
  c.ca3937 = 7.0e-4;
  c.k4039 = 1e-3;
  c.j = 0.01;
  c.df37 = 2.0;
  c.kca_factor = 0.5;
  ArArIntensities in{1.0, 10.0, std::nullopt, 1000.0, 50000.0};
  auto v = compute_arar(in, c);
  const double ca37 = 20.0, ca36 = 2.7e-4 * 20, ca39 = 7e-4 * 20;
  const double k39 = 1000 - ca39, atm40 = (1.0 - ca36) * 298.56;
  const double rad40 = 50000 - atm40 - 1e-3 * k39;
  EXPECT_DOUBLE_EQ(v.at("ca37"), ca37);
  EXPECT_DOUBLE_EQ(v.at("ca36"), ca36);
  EXPECT_DOUBLE_EQ(v.at("k39"), k39);
  EXPECT_DOUBLE_EQ(v.at("atm40"), atm40);
  EXPECT_DOUBLE_EQ(v.at("rad40"), rad40);
  EXPECT_DOUBLE_EQ(v.at("radiogenic_yield"), 100 * rad40 / 50000);
  EXPECT_DOUBLE_EQ(v.at("rad40_percent"), v.at("radiogenic_yield"));
  EXPECT_DOUBLE_EQ(v.at("age"), std::log(1 + 0.01 * rad40 / k39) / 5.543e-10 / 1e6);
  EXPECT_DOUBLE_EQ(v.at("kca"), 0.5 * k39 / ca37);
  EXPECT_DOUBLE_EQ(v.at("cak"), 1 / v.at("kca"));
  EXPECT_FALSE(v.contains("kcl"));
}

TEST(ArAr, OmitsWhatCannotBeComputed) {
  ArArConstants c;  // no J: no age
  auto v = compute_arar(ArArIntensities{1.0, std::nullopt, std::nullopt, 10.0, 400.0}, c);
  EXPECT_FALSE(v.contains("age"));
  EXPECT_FALSE(v.contains("kca"));
  EXPECT_FALSE(v.contains("ca37"));
  EXPECT_NEAR(v.at("radiogenic_yield"), 100 * (400 - 298.56) / 400, 1e-12);
  EXPECT_TRUE(compute_arar(ArArIntensities{}, c).empty());
  // Air: radiogenic yield ~ 0.
  auto air = compute_arar(ArArIntensities{1.0, std::nullopt, std::nullopt, std::nullopt, 298.56}, c);
  EXPECT_NEAR(air.at("radiogenic_yield"), 0.0, 1e-12);
}
