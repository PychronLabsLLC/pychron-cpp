// Data model, constants presets, stored-row resolution (spec 5.1-5.5).
#include "pychron/reduction/arar_types.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <string>

#include "golden.hpp"

namespace golden = pychron::reduction::golden;
using namespace pychron::reduction;

namespace {

using golden::measured_of;

void expect_measured(const Measured& got, const Measured& want, const std::string& what) {
  EXPECT_EQ(got.value, want.value) << what;
  EXPECT_EQ(got.error, want.error) << what;
}

AgeUnits units_of(const std::string& s) {
  if (s == "a") return AgeUnits::a;
  if (s == "ka") return AgeUnits::ka;
  if (s == "Ga") return AgeUnits::Ga;
  return AgeUnits::Ma;
}

void expect_matches_golden(const ReductionConstants& c, const golden::Json& g,
                           const std::string& w) {
  expect_measured(c.lambda_b, measured_of(g["lambda_b"]), w + " lambda_b");
  expect_measured(c.lambda_e, measured_of(g["lambda_e"]), w + " lambda_e");
  expect_measured(c.lambda_cl36, measured_of(g["lambda_cl36"]), w + " lambda_cl36");
  expect_measured(c.lambda_ar37, measured_of(g["lambda_ar37"]), w + " lambda_ar37");
  expect_measured(c.lambda_ar39, measured_of(g["lambda_ar39"]), w + " lambda_ar39");
  expect_measured(c.atm4036, measured_of(g["atm4036"]), w + " atm4036");
  expect_measured(c.atm4038, measured_of(g["atm4038"]), w + " atm4038");
  expect_measured(c.fixed_k3739, measured_of(g["fixed_k3739"]), w + " fixed_k3739");
  EXPECT_EQ(c.k3739_mode == K3739Mode::Fixed, g["k3739_mode"].as_string() == "Fixed") << w;
  EXPECT_EQ(c.abundance_sensitivity, g["abundance_sensitivity"].as_number()) << w;
  EXPECT_EQ(c.allow_negative_ca_correction, g["allow_negative_ca_correction"].as_bool()) << w;
  EXPECT_EQ(c.use_irradiation_endtime, g["use_irradiation_endtime"].as_bool()) << w;
  EXPECT_EQ(c.include_decay_error, g["include_decay_error"].as_bool()) << w;
  EXPECT_EQ(c.age_units, units_of(g["age_units"].as_string())) << w;
  EXPECT_EQ(c.cosmogenic.has_value(), !g["cosmogenic"].is_null()) << w;
}

}  // namespace

TEST(ArArTypes, ConstantsHaveNoHiddenDefaults) {
  const ReductionConstants c{};
  for (const Measured* m : {&c.lambda_b, &c.lambda_e, &c.lambda_cl36, &c.lambda_ar37,
                            &c.lambda_ar39, &c.atm4036, &c.atm4038, &c.fixed_k3739}) {
    EXPECT_EQ(m->value, 0.0);
    EXPECT_EQ(m->error, 0.0);
  }
  EXPECT_FALSE(c.allow_negative_ca_correction);
  EXPECT_EQ(c.abundance_sensitivity, 0.0);
  EXPECT_FALSE(c.cosmogenic.has_value());
}

TEST(ArArTypes, LegacyPresetsMatchLegacy) {
  const golden::Json doc = golden::load("constants.json");
  int seen = 0;
  for (const golden::Json& c : doc["cases"].as_array()) {
    const std::string& name = c["name"].as_string();
    ConstantsPreset p;
    if (name == "preset/legacy") p = ConstantsPreset::Legacy;
    else if (name == "preset/legacy_preferences") p = ConstantsPreset::LegacyPreferences;
    else if (name == "preset/default") p = ConstantsPreset::Default;
    else continue;
    ++seen;
    const ReductionConstants rc = constants_preset(p);
    expect_matches_golden(rc, c["expected"]["constants"], name);
    const Measured lk = lambda_k(rc);
    const Measured want = measured_of(c["expected"]["lambda_k"]);
    EXPECT_NEAR(lk.value, want.value, 1e-12 * want.value) << name;
    EXPECT_NEAR(lk.error, want.error, 1e-10 * want.error) << name;
  }
  EXPECT_EQ(seen, 3);
  const Measured lk = lambda_k(constants_preset(ConstantsPreset::Legacy));
  EXPECT_NEAR(lk.value, 5.543e-10, 1e-22);
  EXPECT_NEAR(lk.error, std::hypot(9.3e-13, 1.6e-13), 1e-24);
}

TEST(ArArTypes, DefaultPresetMatchesSpec) {
  const ReductionConstants c = constants_preset(ConstantsPreset::Default);
  EXPECT_EQ(c.atm4036.value, 298.56);
  EXPECT_EQ(c.atm4036.error, 0.31);
  EXPECT_EQ(c.lambda_b.error, 9.3e-13);
  EXPECT_EQ(c.fixed_k3739.error, 0.01);
  EXPECT_FALSE(c.allow_negative_ca_correction);
  EXPECT_EQ(to_string(ConstantsPreset::Default), "default");
  EXPECT_EQ(to_string(ConstantsPreset::Legacy), "legacy");
  EXPECT_EQ(to_string(ConstantsPreset::LegacyPreferences), "legacy_preferences");
}

TEST(ArArTypes, ResolveAppliesManualAndModifier) {
  StoredValue r;
  r.value = 1.0;
  r.error = 0.1;
  r.manual_value = 5.0;
  r.manual_error = 0.5;
  r.modifier_error = std::nullopt;
  Measured m = resolve(r);
  EXPECT_EQ(m.value, 1.0);
  EXPECT_EQ(m.error, 0.1);
  r.use_manual_value = true;
  m = resolve(r);
  EXPECT_EQ(m.value, 5.0);
  EXPECT_EQ(m.error, 0.1);
  r.use_manual_value = false;
  r.use_manual_error = true;
  m = resolve(r);
  EXPECT_EQ(m.value, 1.0);
  EXPECT_EQ(m.error, 0.5);
  r.use_manual_value = true;
  m = resolve(r);
  EXPECT_EQ(m.value, 5.0);
  EXPECT_EQ(m.error, 0.5);
  r.modifier_error = 0.9;
  m = resolve(r);
  EXPECT_EQ(m.value, 5.0);
  EXPECT_EQ(m.error, 0.9);
}

TEST(ArArTypes, MakeSignalUsesLegacyTagsAndFreshVariables) {
  MeasuredSignal m;
  m.intercept = {10.0, 0.1};
  m.baseline = {1.0, 0.01};
  m.blank = {0.5, 0.02};
  m.ic_factor = {1.01, 0.001};
  const IsotopeSignal a = make_signal(ArgonIsotope::Ar40, m);
  const IsotopeSignal b = make_signal(ArgonIsotope::Ar40, m);
  EXPECT_EQ(tag_name(a.intercept.terms()[0].tag), "Ar40");
  EXPECT_EQ(tag_name(a.baseline.terms()[0].tag), "Ar40 bs");
  EXPECT_EQ(tag_name(a.blank.terms()[0].tag), "Ar40 bk");
  EXPECT_EQ(tag_name(a.ic_factor.terms()[0].tag), "Ar40 IC");
  std::set<VariableId> ids{a.intercept.variable_id(), a.baseline.variable_id(),
                           a.blank.variable_id(),     a.ic_factor.variable_id(),
                           b.intercept.variable_id(), b.baseline.variable_id(),
                           b.blank.variable_id(),     b.ic_factor.variable_id()};
  EXPECT_EQ(ids.size(), 8u);
  EXPECT_EQ(ids.count(0), 0u);

  MeasuredSignal zm;
  zm.intercept = {3.0, 0.0};
  zm.baseline = {1.0, 0.0};
  zm.blank = {0.5, 0.0};
  const IsotopeSignal z = make_signal(ArgonIsotope::Ar36, zm);
  EXPECT_TRUE(z.intercept.is_exact());
  EXPECT_EQ(z.intercept.nominal(), 3.0);
  EXPECT_EQ(z.baseline.nominal(), 1.0);
  EXPECT_EQ(z.blank.nominal(), 0.5);
  EXPECT_EQ(z.ic_factor.nominal(), 1.0);
  EXPECT_EQ(index(ArgonIsotope::Ar36), 4u);
  EXPECT_EQ(to_string(ArgonIsotope::Ar37), "Ar37");
}

TEST(ArArTypes, CorrectsForBlank) {
  for (const char* t : {"blank_unknown", "blank_air", "detector_ic", "background"})
    EXPECT_FALSE(corrects_for_blank(t)) << t;
  for (const char* t : {"unknown", "air", "cocktail"}) EXPECT_TRUE(corrects_for_blank(t)) << t;
}

TEST(ArArTypes, ProductionFromRows) {
  std::map<std::string, Measured, std::less<>> rows{
      {"K4039", {0.01, 0.001}},  {"K3839", {0.013, 0.0}},  {"K3739", {0.0002, 0.0}},
      {"Ca3937", {0.0007, 0.0001}}, {"Ca3837", {0.00016, 0.0}}, {"Ca3637", {0.00026, 0.00001}},
      {"Cl3638", {250.0, 5.0}},  {"Ca_K", {1.0, 0.1}},     {"Cl_K", {2.0, 0.2}}};
  auto r = production_from_rows(rows);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->k4039.value, 0.01);
  EXPECT_EQ(r->k4039.error, 0.001);
  EXPECT_EQ(r->cl3638.value, 250.0);
  ASSERT_TRUE(r->ca_k.has_value());
  EXPECT_EQ(r->ca_k->error, 0.1);
  ASSERT_TRUE(r->cl_k.has_value());
  EXPECT_EQ(r->cl_k->value, 2.0);

  rows.erase("Ca3837");
  rows.erase("Ca_K");
  r = production_from_rows(rows);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->ca3837.value, 0.0);
  EXPECT_EQ(r->ca3837.error, 0.0);
  EXPECT_FALSE(r->ca_k.has_value());

  rows["K4139"] = {1.0, 0.0};
  r = production_from_rows(rows);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
  EXPECT_NE(r.error().what.find("K4139"), std::string::npos);
  EXPECT_EQ(r.error().what.rfind("reduction: ", 0), 0u);

  rows.erase("K4139");
  rows["K3739"] = {std::nan(""), 0.0};
  r = production_from_rows(rows);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().what.find("K3739"), std::string::npos);
}

TEST(ArArTypes, ProductionVariablesTaggedOnce) {
  ProductionRatios p;
  p.k4039 = {0.01, 0.001};
  p.k3839 = {0.013, 0.001};
  p.k3739 = {0.0002, 0.00001};
  p.ca3937 = {0.0007, 0.0001};
  p.ca3837 = {0.00016, 0.00001};
  p.ca3637 = {0.00026, 0.00001};
  p.cl3638 = {250.0, 5.0};
  p.ca_k = Measured{1.0, 0.1};
  const ProductionVariables v = make_production_variables(p);
  EXPECT_EQ(tag_name(v.k4039.terms()[0].tag), "K4039");
  EXPECT_EQ(tag_name(v.k3839.terms()[0].tag), "K3839");
  EXPECT_EQ(tag_name(v.k3739.terms()[0].tag), "K3739");
  EXPECT_EQ(tag_name(v.ca3937.terms()[0].tag), "Ca3937");
  EXPECT_EQ(tag_name(v.ca3837.terms()[0].tag), "Ca3837");
  EXPECT_EQ(tag_name(v.ca3637.terms()[0].tag), "Ca3637");
  EXPECT_EQ(tag_name(v.cl3638.terms()[0].tag), "Cl3638");
  ASSERT_TRUE(v.ca_k.has_value());
  EXPECT_EQ(tag_name(v.ca_k->terms()[0].tag), "Ca_K");
  EXPECT_FALSE(v.cl_k.has_value());
  const auto ids = v.interference_ids();
  const std::set<VariableId> distinct(ids.begin(), ids.end());
  EXPECT_EQ(distinct.size(), 7u);
  EXPECT_EQ(distinct.count(0), 0u);
  EXPECT_EQ(distinct.count(v.ca_k->variable_id()), 0u);
}

TEST(ArArTypes, MakeJTagged) {
  Flux f;
  f.j = {0.0015, 1e-6};
  const UFloat j = make_j(f);
  EXPECT_EQ(j.nominal(), 0.0015);
  EXPECT_EQ(j.std_dev(), 1e-6);
  ASSERT_EQ(j.terms().size(), 1u);
  EXPECT_EQ(tag_name(j.terms()[0].tag), "J");
}
