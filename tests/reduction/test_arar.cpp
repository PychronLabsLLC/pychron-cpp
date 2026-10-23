#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <string>

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

// ---- Task 13: compute_arar on the shared kernels (spec 8.2) -----------------

TEST(ArAr, ChlorineKeysOnlyWhenConfigured) {
  ArArConstants c;
  c.ca3637 = 2.7e-4;
  c.ca3937 = 7.0e-4;
  c.k4039 = 1e-3;
  c.j = 0.01;
  const ArArIntensities in{1.0, 10.0, 5.0, 1000.0, 50000.0};
  auto v = compute_arar(in, c);
  for (const char* key : {"kcl", "clk", "cl36"}) EXPECT_FALSE(v.contains(key)) << key;

  c.chlorine = LiveChlorine{};
  c.chlorine->cl3638 = 250.0;
  c.chlorine->decay_days = 365.0;
  c.chlorine->cl_k_factor = 0.5;
  v = compute_arar(in, c);
  for (const char* key : {"kcl", "clk", "cl36"}) EXPECT_TRUE(v.contains(key)) << key;
  // E12 by hand (legacy operand order), E19 K/Cl.
  const double ca37 = 10.0, ca36 = 2.7e-4 * ca37, k39 = 1000.0 - 7.0e-4 * ca37;
  const double m = 250.0 * 6.308e-9 * 365.0, r3836 = 298.56 / 1575.0;
  const double atm36 = (1.0 - ca36 - m * (5.0 - 0.0 - 0.0)) / (1.0 - m * r3836);
  const double cl38 = 5.0 - r3836 * atm36 - 0.0 - 0.0;
  EXPECT_DOUBLE_EQ(v.at("cl36"), cl38 * m);
  EXPECT_DOUBLE_EQ(v.at("atm40"), atm36 * 298.56);
  EXPECT_DOUBLE_EQ(v.at("kcl"), k39 / cl38 * 0.5);
  EXPECT_DOUBLE_EQ(v.at("clk"), 1.0 / v.at("kcl"));

  // Ar38 absent: no chlorine keys (and no chlorine-corrected atm36).
  const ArArIntensities no38{1.0, 10.0, std::nullopt, 1000.0, 50000.0};
  v = compute_arar(no38, c);
  for (const char* key : {"kcl", "clk", "cl36", "atm40", "rad40", "age"}) {
    EXPECT_FALSE(v.contains(key)) << key;
  }
  EXPECT_TRUE(v.contains("k39"));
}

TEST(ArAr, ChlorineNoOpDoesNotRequireAr38) {
  ArArConstants c;
  c.ca3637 = 2.7e-4;
  c.ca3937 = 7.0e-4;
  c.k4039 = 1e-3;
  c.j = 0.01;
  c.lambda_total = 5.543e-4;
  c.chlorine = LiveChlorine{};
  c.chlorine->atm4038 = 1575.0;
  const ArArIntensities no38{1.0, 10.0, std::nullopt, 1000.0, 50000.0};
  const ArArIntensities with38{1.0, 10.0, 5.0, 1000.0, 50000.0};

  // Cl3638 == 0 (m == 0): chlorine is a no-op, so the ages survive without Ar38.
  c.chlorine->cl3638 = 0.0;
  c.chlorine->lambda_cl36 = 6.308e-9;
  c.chlorine->decay_days = 365.0;
  auto v = compute_arar(no38, c);
  for (const char* key : {"atm40", "rad40", "age"}) EXPECT_TRUE(v.contains(key)) << key;
  for (const char* key : {"kcl", "clk", "cl36"}) EXPECT_FALSE(v.contains(key)) << key;
  // Same nominal age as the chlorine-free path.
  ArArConstants plain = c;
  plain.chlorine.reset();
  EXPECT_DOUBLE_EQ(v.at("age"), compute_arar(no38, plain).at("age"));

  // decay_days == 0 (instant age): also a no-op.
  c.chlorine->cl3638 = 250.0;
  c.chlorine->decay_days = 0.0;
  v = compute_arar(no38, c);
  EXPECT_TRUE(v.contains("age"));

  // m != 0 still needs Ar38.
  c.chlorine->decay_days = 365.0;
  v = compute_arar(no38, c);
  EXPECT_FALSE(v.contains("age"));
  EXPECT_TRUE(compute_arar(with38, c).contains("age"));
}

TEST(ArAr, K3739NeedsAr39) {
  ArArConstants c;
  c.ca3637 = 2.7e-4;
  c.ca3937 = 7.0e-4;
  c.k3739 = 0.01;
  auto v = compute_arar(ArArIntensities{1.0, 10.0, std::nullopt, std::nullopt, 400.0}, c);
  for (const char* key : {"ca37", "ca36", "ca39", "k39", "kca"}) {
    EXPECT_FALSE(v.contains(key)) << key;
  }
  EXPECT_TRUE(v.contains("atm40"));
  // With Ar39, E9: k39 = (a39 - Ca3937 a37) / (1 - K3739 Ca3937), ca37 = a37 - K3739 k39.
  v = compute_arar(ArArIntensities{1.0, 10.0, std::nullopt, 1000.0, 400.0}, c);
  const double k39 = (1000.0 - 7.0e-4 * 10.0) / (1.0 - 0.01 * 7.0e-4);
  const double ca37 = 10.0 - 0.01 * k39;
  EXPECT_DOUBLE_EQ(v.at("k39"), k39);
  EXPECT_DOUBLE_EQ(v.at("ca37"), ca37);
  EXPECT_DOUBLE_EQ(v.at("ca36"), 2.7e-4 * ca37);
  EXPECT_DOUBLE_EQ(v.at("ca39"), 7.0e-4 * ca37);
}

// E10 takes ca37 from Ar39 alone, so with a fixed K3739 and no Ar37 the Ca
// components are still live and k39 / atm40 use them consistently.
TEST(ArAr, FixedK3739WithoutAr37) {
  ArArConstants c;
  c.ca3637 = 2.7e-4;
  c.ca3937 = 7.0e-4;
  c.analysis_fixed_k3739 = 0.05;
  const auto v = compute_arar(ArArIntensities{1.0, std::nullopt, std::nullopt, 1000.0, 400.0}, c);
  const double x = 0.05, y = 1.0 / 7.0e-4;
  const double ca37 = (1000.0 * x * y) / (x + y);
  const double ca39 = 7.0e-4 * ca37, ca36 = 2.7e-4 * ca37;
  EXPECT_EQ(v.at("ca37"), ca37);
  EXPECT_EQ(v.at("ca39"), ca39);
  EXPECT_EQ(v.at("ca36"), ca36);
  EXPECT_EQ(v.at("k39"), 1000.0 - ca39);
  EXPECT_EQ(v.at("atm40"), (1.0 - ca36 - 0.0 * (0.0 - 0.0 - 0.0)) / 1.0 * 298.56);
  EXPECT_TRUE(v.contains("kca"));
  // Without Ar39 nothing Ca-derived is live.
  const auto w = compute_arar(ArArIntensities{1.0, 10.0, std::nullopt, std::nullopt, 400.0}, c);
  for (const char* key : {"ca37", "ca36", "ca39", "k39", "kca"}) EXPECT_FALSE(w.contains(key)) << key;
}
