// Spec 8.2 parity between the live path (compute_arar, libs/reduction) and
// the spec'd reduction (reduce()): to_live_constants and the golden vectors.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <string>

#include "golden.hpp"
#include "pychron/arar/arar_reduction.hpp"
#include "pychron/arar/live_parity.hpp"
#include "pychron/reduction/arar.hpp"
#include "reduce_golden.hpp"

using namespace pychron::reduction;
namespace g = pychron::reduction::golden;
using pychron::Result;

// Spec 8.2 parity: for complete, blank-free pipeline.json cases, the live path
// on the corrected intensities (E1-E3, before E4/E8) with to_live_constants
// equals reduce()'s nominal values for every key both produce (rtol 1e-12).
// The live path does not model abundance sensitivity (E4) or the cosmogenic
// split (E13), so those cases are counted and skipped.
TEST(ArAr, MatchesReduceNominal) {
  const g::Json doc = g::load("pipeline.json");
  const g::Json& cases = doc["cases"];
  ASSERT_EQ(cases.size(), 54u);
  std::size_t compared = 0, incomplete = 0, blanks = 0, cosmogenic = 0, sensitivity = 0;
  std::size_t chlorine_keys = 0, fixed = 0, keys = 0;
  for (const g::Json& c : cases.as_array()) {
    const std::string name = c["name"].string;
    SCOPED_TRACE(name);
    const g::Json& in = c["inputs"];
    ReductionConstants rc;
    if (!g::constants_of(in["constants"], rc, name)) {
      ADD_FAILURE() << "constants";
      continue;
    }
    g::Built b = g::build_input(in, rc, name);
    if (!b.ok) {
      ADD_FAILURE() << "inputs";
      continue;
    }
    if (!b.missing.empty() || !c["expect_error"].is_null()) {
      ++incomplete;
      continue;
    }
    bool has_blank = false;
    for (const IsotopeSignal& s : b.in.isotopes) {
      has_blank = has_blank || s.blank.nominal() != 0.0 || s.blank.std_dev() != 0.0;
    }
    if (has_blank) {
      ++blanks;
      continue;
    }
    if (rc.cosmogenic) {
      ++cosmogenic;
      continue;
    }
    if (rc.abundance_sensitivity != 0.0) {
      ++sensitivity;
      continue;
    }
    const Result<ArArResult> r = reduce(b.in);
    if (!r) {
      ADD_FAILURE() << r.error().what;
      continue;
    }

    ArArIntensities li;
    const std::array<std::optional<double>*, 5> slots{&li.ar40, &li.ar39, &li.ar38, &li.ar37,
                                                      &li.ar36};
    for (const ArgonIsotope iso : kArgonKeys) {
      *slots[index(iso)] = corrected_intensity(b.in.isotopes[index(iso)]).nominal();
    }
    std::map<std::string, Measured, std::less<>> rows;
    for (const auto& [key, m] : in["production"].as_object()) rows[key] = g::measured_of(m);
    const Result<ProductionRatios> pr = production_from_rows(rows);
    if (!pr) {
      ADD_FAILURE() << pr.error().what;
      continue;
    }
    Flux flux;
    if (b.in.j) flux.j = {b.in.j->nominal(), b.in.j->std_dev()};
    flux.lambda_k_total = b.in.lambda_k_total;
    ArArConstants lc = to_live_constants(rc, *pr, flux, r->decay, b.in.irradiation.decay_days);
    if (b.in.fixed_k3739) {
      lc.analysis_fixed_k3739 = b.in.fixed_k3739->value;
      ++fixed;
    }
    const std::map<std::string, double> live = compute_arar(li, lc);

    std::map<std::string, double> want;
    const FResult& f = r->f;
    want["rad40"] = f.rad40.nominal();
    want["atm40"] = f.atm40.nominal();
    want["k39"] = f.interference.k39.nominal();
    want["ca37"] = f.interference.ca37.nominal();
    want["ca39"] = f.interference.ca39.nominal();
    want["ca36"] = f.interference.ca36.nominal();
    want["cl36"] = f.atmospheric.cl36.nominal();
    if (f.radiogenic_yield) {
      want["radiogenic_yield"] = want["rad40_percent"] = f.radiogenic_yield->nominal();
    }
    // Live ages are in Ma.
    if (r->ages) want["age"] = r->ages->age.nominal() / age_scale(AgeUnits::Ma, rc.age_units);
    if (r->kca) want["kca"] = r->kca->nominal();
    if (r->cak) want["cak"] = r->cak->nominal();
    if (r->kcl) want["kcl"] = r->kcl->nominal();
    if (r->clk) want["clk"] = r->clk->nominal();

    for (const auto& [key, v] : live) {
      const auto it = want.find(key);
      if (it == want.end()) {
        ADD_FAILURE() << "live key " << key << " not produced by reduce";
        continue;
      }
      g::expect_close(v, it->second, 1e-12, 0.0, key);
      ++keys;
      if (key == "kcl" || key == "clk" || key == "cl36") ++chlorine_keys;
    }
    // The core keys are always live for complete inputs.
    for (const char* key : {"rad40", "atm40", "k39", "ca37", "ca39", "ca36", "cl36"}) {
      EXPECT_TRUE(live.contains(key)) << key;
    }
    // Apart from the legacy zero-divisor omissions (k39 == 0 drops kca/cak/kcl
    // /clk), every reduce key is live too.
    if (f.interference.k39.nominal() != 0.0) {
      for (const auto& [key, v] : want) EXPECT_TRUE(live.contains(key)) << key;
    }
    ++compared;
  }
  EXPECT_EQ(incomplete, 2u);   // missing_isotope
  EXPECT_EQ(blanks, 6u);       // baselines_blanks_ic, blank_type_no_blank_correction, negative_ar36
  EXPECT_EQ(cosmogenic, 2u);
  EXPECT_EQ(sensitivity, 2u);
  EXPECT_EQ(compared, 42u);
  EXPECT_EQ(fixed, 2u);        // fixed_k3739_by_analysis
  EXPECT_GT(chlorine_keys, 100u);
  EXPECT_GT(keys, 500u);
}

TEST(ArAr, LiveDefaultsEqualDefaultPreset) {
  EXPECT_EQ(ArArConstants{}.atm4036, constants_preset(ConstantsPreset::Default).atm4036.value);
  const ArArConstants c;
  EXPECT_EQ(c.k3739, 0.0);
  EXPECT_EQ(c.k3839, 0.0);
  EXPECT_EQ(c.ca3837, 0.0);
  EXPECT_TRUE(c.allow_negative_ca_correction);
  EXPECT_FALSE(c.chlorine.has_value());
  const LiveChlorine cl;
  EXPECT_EQ(cl.cl3638, 0.0);
  EXPECT_EQ(cl.lambda_cl36, 6.308e-9);
  EXPECT_EQ(cl.decay_days, 0.0);
  EXPECT_EQ(cl.atm4038, 1575.0);
  EXPECT_EQ(cl.cl_k_factor, 1.0);
}

TEST(ArAr, ToLiveConstantsFromReductionInputs) {
  ReductionConstants rc = constants_preset(ConstantsPreset::Legacy);
  rc.allow_negative_ca_correction = false;
  ProductionRatios p;
  p.k4039 = {1e-3, 1e-5};
  p.k3839 = {0.013, 0};
  p.k3739 = {0.0002, 0};
  p.ca3937 = {7e-4, 0};
  p.ca3837 = {3e-5, 0};
  p.ca3637 = {2.7e-4, 0};
  p.cl3638 = {250.0, 0};
  p.ca_k = Measured{2.0, 0.1};
  p.cl_k = Measured{0.0, 0.0};
  Flux flux;
  flux.j = {0.01, 1e-5};
  const DecayFactors df{1.5, 1.01};
  ArArConstants c = to_live_constants(rc, p, flux, df);
  EXPECT_EQ(c.lambda_total, rc.lambda_b.value + rc.lambda_e.value);
  EXPECT_EQ(c.atm4036, rc.atm4036.value);
  EXPECT_EQ(c.k4039, 1e-3);
  EXPECT_EQ(c.k3839, 0.013);
  EXPECT_EQ(c.k3739, 0.0002);
  EXPECT_EQ(c.ca3937, 7e-4);
  EXPECT_EQ(c.ca3837, 3e-5);
  EXPECT_EQ(c.ca3637, 2.7e-4);
  EXPECT_EQ(c.j, 0.01);
  EXPECT_EQ(c.df37, 1.5);
  EXPECT_EQ(c.df39, 1.01);
  EXPECT_EQ(c.kca_factor, 0.5);
  EXPECT_FALSE(c.allow_negative_ca_correction);
  EXPECT_FALSE(c.chlorine.has_value());  // no decay_days
  EXPECT_EQ(c.k3739_mode, rc.k3739_mode);
  EXPECT_EQ(c.fixed_k3739, rc.fixed_k3739.value);

  flux.lambda_k_total = Measured{5.5305e-10, 0.0};
  p.ca_k.reset();
  c = to_live_constants(rc, p, flux, df, 365.0);
  EXPECT_EQ(c.lambda_total, 5.5305e-10);
  EXPECT_EQ(c.kca_factor, 1.0);
  ASSERT_TRUE(c.chlorine.has_value());
  EXPECT_EQ(c.chlorine->cl3638, 250.0);
  EXPECT_EQ(c.chlorine->lambda_cl36, rc.lambda_cl36.value);
  EXPECT_EQ(c.chlorine->decay_days, 365.0);
  EXPECT_EQ(c.chlorine->atm4038, rc.atm4038.value);
  EXPECT_EQ(c.chlorine->cl_k_factor, 1.0);  // Cl_K == 0: factor 1
}
