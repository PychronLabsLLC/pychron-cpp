// Single-analysis reduce() pipeline, K/Ca and error components (spec 3.1-3.7,
// 5.6, 6, 7; E1-E20; D2, D3, D4, D6; Q1, Q2, Q6, Q7).
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "golden.hpp"
#include "pychron/reduction/arar_reduction.hpp"
#include "reduce_golden.hpp"

using namespace pychron::reduction;
using pychron::Result;
namespace g = pychron::reduction::golden;

namespace {

bool has(const std::vector<Diagnostic>& d, Diagnostic x) {
  return std::find(d.begin(), d.end(), x) != d.end();
}

std::vector<VariableId> ids_tagged(const UFloat& x, std::string_view tag) {
  std::vector<VariableId> out;
  for (const UFloat::Term& t : x.terms()) {
    if (tag_name(t.tag) == tag) out.push_back(t.id);
  }
  return out;
}

// Same nominal, same variables, same partials (bitwise).
void expect_identical(const UFloat& got, const UFloat& want, const std::string& what) {
  EXPECT_EQ(got.nominal(), want.nominal()) << what;
  ASSERT_EQ(got.terms().size(), want.terms().size()) << what;
  for (std::size_t i = 0; i < got.terms().size(); ++i) {
    EXPECT_EQ(got.terms()[i].id, want.terms()[i].id) << what;
    EXPECT_EQ(got.terms()[i].deriv, want.terms()[i].deriv) << what;
  }
}

using g::expect_config_error;

// A valid analysis: legacy CalculateFTest-like intensities (Ar37 = 5, so ca37
// is well away from 0), legacy constants, 5 years since irradiation.
ReductionInput base_input() {
  ReductionInput in;
  const std::array<Measured, 5> v{{{1000.0, 1.0}, {100.0, 0.5}, {10.0, 0.05}, {5.0, 0.025},
                                   {2.0, 0.01}}};
  for (const ArgonIsotope iso : kArgonKeys) {
    MeasuredSignal m;
    m.intercept = v[index(iso)];
    m.blank = {0.01, 0.001};
    m.baseline = {0.002, 0.0005};
    in.isotopes[index(iso)] = make_signal(iso, m);
  }
  in.constants = constants_preset(ConstantsPreset::Legacy);
  ProductionRatios p;
  p.k4039 = {0.0002, 0.00001};
  p.k3839 = {0.013, 0.0001};
  p.k3739 = {0.01, 0.0001};
  p.ca3937 = {0.0007, 0.00001};
  p.ca3837 = {0.00019, 0.0};
  p.ca3637 = {0.00026, 0.00001};
  p.ca_k = Measured{2.0, 0.02};
  p.cl_k = Measured{0.1, 0.0};
  in.production = make_production_variables(p);
  in.irradiation.decay_days = 1825.0;
  in.j = make_j(Flux{{0.001, 1e-6}, 5e-7, std::nullopt});
  in.position_jerr = 5e-7;
  return in;
}

// ---- Golden input parsing ---------------------------------------------------

using g::build_input;
using g::Built;
using g::by_isotope;
using g::check_group;
using g::check_u;
using g::constants_of;
using g::known_keys;
using g::PipelineCounts;
using g::run_pipeline_case;

}  // namespace

// ---- Input validation (spec 6, Q7) -----------------------------------------

TEST(Reduce, ValidatesInputs) {
  ASSERT_TRUE(reduce(base_input())) << "base input must be valid";

  {  // Q7: a NaN intercept is an error naming the slot, never coerced to 0.
    ReductionInput in = base_input();
    in.isotopes[index(ArgonIsotope::Ar36)].intercept = UFloat::variable(std::nan(""), 0.01, "Ar36");
    expect_config_error(reduce(in), "Ar36 intercept", "NaN intercept");
  }
  {
    ReductionInput in = base_input();
    in.isotopes[index(ArgonIsotope::Ar39)].blank = UFloat(HUGE_VAL);
    expect_config_error(reduce(in), "Ar39 blank", "inf blank");
  }
  {  // Negative sigmas (UFloat variables assert sigma >= 0, so through Measured).
    ReductionInput in = base_input();
    in.constants.atm4038.error = -2.0;
    expect_config_error(reduce(in), "atm4038", "negative sigma atm4038");
  }
  {
    ReductionInput in = base_input();
    in.constants.lambda_ar37.error = -1.0;
    expect_config_error(reduce(in), "lambda_ar37", "negative sigma lambda_ar37");
  }
  {
    ReductionInput in = base_input();
    in.lambda_k_total = Measured{5.5e-10, -1e-12};
    expect_config_error(reduce(in), "lambda_k_total", "negative sigma lambda_k_total");
  }
  {
    ReductionInput in = base_input();
    in.fixed_k3739 = Measured{0.01, -0.001};
    expect_config_error(reduce(in), "fixed_k3739", "negative sigma fixed_k3739");
  }
  {
    ReductionInput in = base_input();
    in.constants.abundance_sensitivity = -1e-6;
    expect_config_error(reduce(in), "abundance_sensitivity", "negative abundance_sensitivity");
  }
  {
    ReductionInput in = base_input();
    in.isotopes[index(ArgonIsotope::Ar40)].deadtime_tau_s = -1e-9;
    expect_config_error(reduce(in), "Ar40 deadtime_tau_s", "negative deadtime tau");
  }
  {
    ReductionInput in = base_input();
    in.constants.lambda_b = {0.0, 0.0};
    in.constants.lambda_e = {0.0, 0.0};
    expect_config_error(reduce(in), "lambda_b + lambda_e", "zero lambda_K");
  }
  {  // D1: a value-initialised record has no physics defaults.
    ReductionInput in = base_input();
    in.constants = ReductionConstants{};
    expect_config_error(reduce(in), "lambda_b + lambda_e", "value-initialised constants");
  }
  {
    ReductionInput in = base_input();
    in.j = UFloat::variable(std::nan(""), 1e-6, "J");
    expect_config_error(reduce(in), "J", "NaN J");
  }
  {
    ReductionInput in = base_input();
    in.position_jerr = -1e-7;
    expect_config_error(reduce(in), "position_jerr", "negative position_jerr");
  }
  {
    ReductionInput in = base_input();
    in.irradiation.decay_days = std::nan("");
    expect_config_error(reduce(in), "decay_days", "NaN decay_days");
  }
  {
    ReductionInput in = base_input();
    in.irradiation.segments.push_back({1.0, HUGE_VAL, 10.0});
    expect_config_error(reduce(in), "segment", "inf segment");
  }
  {  // A zero decay constant with segments would make E7 0/0.
    ReductionInput in = base_input();
    in.irradiation.segments.push_back({1.0, 1.0, 10.0});
    in.constants.lambda_ar39 = {0.0, 0.0};
    expect_config_error(reduce(in), "lambda_ar39", "zero lambda_ar39 with segments");
  }
  {
    ReductionInput in = base_input();
    in.production.k4039 = UFloat(std::nan(""));
    expect_config_error(reduce(in), "K4039", "NaN production ratio");
  }
}

// ---- D4: deadtime off unless a detector tau is given -----------------------

TEST(Reduce, DeadtimeOffByDefault) {
  EXPECT_FALSE(MeasuredSignal{}.deadtime_tau_s.has_value());
  EXPECT_FALSE(IsotopeSignal{}.deadtime_tau_s.has_value());
  const ReductionInput in = base_input();
  for (const IsotopeSignal& s : in.isotopes) ASSERT_FALSE(s.deadtime_tau_s.has_value());
  const auto r = reduce(in);
  ASSERT_TRUE(r) << r.error().what;
  // No abundance sensitivity, no segments: corrected is E1-E3 alone, exactly
  // the intercept's own variables (no deadtime step inserted).
  for (const ArgonIsotope iso : kArgonKeys) {
    expect_identical(r->corrected[index(iso)], corrected_intensity(in.isotopes[index(iso)]),
                     std::string(to_string(iso)));
  }

  // The step is real: a tau on Ar40 moves Ar40 only.
  ReductionInput with = base_input();
  with.isotopes[index(ArgonIsotope::Ar40)].deadtime_tau_s = 2e-8;
  const auto rt = reduce(with);
  ASSERT_TRUE(rt) << rt.error().what;
  EXPECT_GT(rt->corrected[0].nominal(), r->corrected[0].nominal() + 0.01);
  for (std::size_t i = 1; i < 5; ++i) {
    EXPECT_EQ(rt->corrected[i].nominal(), r->corrected[i].nominal());
  }
}

// ---- Q1 / D6: constants are minted per analysis ----------------------------

TEST(Reduce, ConstantsNotSharedAcrossAnalyses) {
  ReductionInput a = base_input();
  ReductionInput b = base_input();
  a.constants.include_decay_error = true;
  b.constants = a.constants;  // the same record
  const auto ra = reduce(a);
  const auto rb = reduce(b);
  ASSERT_TRUE(ra && rb);
  ASSERT_TRUE(ra->f.f && rb->f.f);
  const UFloat& fa = *ra->f.f;
  const UFloat& fb = *rb->f.f;
  // trapped_4036 (E14) reaches F; atm3836 (E12) reaches atm38 (F only with
  // Cl3638 != 0, which this input does not have).
  const UFloat& atm38a = ra->f.atmospheric.atm38;
  const UFloat& atm38b = rb->f.atmospheric.atm38;
  for (const auto& [xa, xb, tag] :
       {std::tuple{&fa, &fb, std::string_view("trapped_4036")},
        std::tuple{&atm38a, &atm38b, std::string_view("atm3836")}}) {
    const auto ia = ids_tagged(*xa, tag);
    const auto ib = ids_tagged(*xb, tag);
    EXPECT_EQ(ia.size(), 1u) << tag;
    EXPECT_EQ(ib.size(), 1u) << tag;
    if (ia.size() != 1u || ib.size() != 1u) continue;
    EXPECT_NE(ia[0], ib[0]) << tag;
    EXPECT_EQ(xb->derivative(ia[0]), 0.0) << tag;
  }
  // E14's trapped_4036 and E12's atm3836 are distinct variables, and no
  // atm4036/atm4038-tagged variable reaches F.
  const auto trapped_a = ids_tagged(fa, "trapped_4036");
  const auto atm3836_a = ids_tagged(atm38a, "atm3836");
  ASSERT_EQ(trapped_a.size(), 1u);
  ASSERT_EQ(atm3836_a.size(), 1u);
  EXPECT_NE(trapped_a[0], atm3836_a[0]);
  EXPECT_EQ(fa.derivative(atm3836_a[0]), 0.0);
  EXPECT_EQ(covariance(atm38a, atm38b), 0.0);
  EXPECT_TRUE(ids_tagged(fa, "atm4036").empty());
  EXPECT_TRUE(ids_tagged(fa, "atm4038").empty());
  EXPECT_EQ(covariance(fa, fb), 0.0);

  // lambda_K (include_decay_error) is per analysis too.
  ASSERT_TRUE(ra->ages && rb->ages);
  const auto la = ids_tagged(ra->ages->age_w_j_err, "lambda_k");
  const auto lb = ids_tagged(rb->ages->age_w_j_err, "lambda_k");
  ASSERT_EQ(la.size(), 1u);
  ASSERT_EQ(lb.size(), 1u);
  EXPECT_NE(la[0], lb[0]);
  // Without an override each J variant reads lambda_K afresh (Task 10).
  EXPECT_NE(ids_tagged(ra->ages->age, "lambda_k")[0], la[0]);
  EXPECT_EQ(covariance(ra->ages->age, rb->ages->age), 0.0);
}

// The lambda_k_total override (pipeline.json lambda_k_override case,
// dvc/dvc.py:2303-2305): legacy sets it once on the analysis' constants, so the
// three J variants of one analysis share one lambda_k variable. Across
// analyses legacy can share it: with frozen fluxes the flux dict, and its
// lambda_k ufloat, is looked up per identifier (dvc/dvc.py:2270-2273), so
// analyses of one identifier get the same variable. Here it is minted fresh
// per reduce() call; ReductionInput takes it as a Measured, so a caller cannot
// share a lambda_k UFloat between analyses. Sharing is the phase 2
// shared-constants mode (spec Q1, D6).
TEST(Reduce, LambdaKOverrideSharedWithinAnalysis) {
  const g::Json doc = g::load("pipeline.json");
  std::size_t seen = 0;
  for (const g::Json& c : doc["cases"].as_array()) {
    const std::string name = c["name"].string;
    if (!name.starts_with("pipeline/lambda_k_override@")) continue;
    SCOPED_TRACE(name);
    ++seen;
    ReductionConstants rc;
    if (!constants_of(c["inputs"]["constants"], rc, name)) continue;
    EXPECT_TRUE(rc.include_decay_error);
    Built b1 = build_input(c["inputs"], rc, name);
    Built b2 = build_input(c["inputs"], rc, name);
    if (!b1.ok || !b2.ok) continue;
    EXPECT_TRUE(b1.in.lambda_k_total.has_value());
    const auto r1 = reduce(b1.in);
    const auto r2 = reduce(b2.in);
    if (!r1 || !r2 || !r1->ages || !r2->ages) {
      ADD_FAILURE() << name << ": no ages";
      continue;
    }
    const auto l_age = ids_tagged(r1->ages->age, "lambda_k");
    const auto l_j = ids_tagged(r1->ages->age_w_j_err, "lambda_k");
    const auto l_pos = ids_tagged(r1->ages->age_w_position_err, "lambda_k");
    EXPECT_EQ(l_age.size(), 1u);
    EXPECT_EQ(l_j.size(), 1u);
    EXPECT_EQ(l_pos.size(), 1u);
    if (l_age.empty() || l_j.empty() || l_pos.empty()) continue;
    EXPECT_EQ(l_age[0], l_j[0]);
    EXPECT_EQ(l_age[0], l_pos[0]);
    const auto other = ids_tagged(r2->ages->age, "lambda_k");
    EXPECT_EQ(other.size(), 1u);
    if (!other.empty()) { EXPECT_NE(other[0], l_age[0]); }
  }
  EXPECT_EQ(seen, 2u);
}

// ---- D2: both legacy default sets ----------------------------------------

TEST(Reduce, BothLegacyPresetsGolden) {
  const g::Json doc = g::load("pipeline.json");
  const ReductionConstants legacy = constants_preset(ConstantsPreset::Legacy);
  const ReductionConstants prefs = constants_preset(ConstantsPreset::LegacyPreferences);
  std::set<std::string> base_legacy, base_prefs;
  PipelineCounts n;
  for (const g::Json& c : doc["cases"].as_array()) {
    const std::string name = c["name"].string;
    SCOPED_TRACE(name);
    const bool is_legacy = name.ends_with("@legacy");
    const bool is_prefs = name.ends_with("@legacy_preferences");
    if (!is_legacy && !is_prefs) {
      ADD_FAILURE() << name << ": no preset suffix";
      continue;
    }
    const std::string base = name.substr(0, name.rfind('@'));
    (is_legacy ? base_legacy : base_prefs).insert(base);
    // The fields in which the two legacy sets differ (spec 5.3 table, Q19)
    // come from the case and equal that set; never read from a preset.
    ReductionConstants rc;
    if (!constants_of(c["inputs"]["constants"], rc, name)) continue;
    const ReductionConstants& want = is_legacy ? legacy : prefs;
    EXPECT_EQ(rc.atm4036.value, want.atm4036.value);
    EXPECT_EQ(rc.atm4036.error, want.atm4036.error);
    EXPECT_EQ(rc.lambda_b.error, want.lambda_b.error);
    EXPECT_EQ(rc.lambda_e.error, want.lambda_e.error);
    EXPECT_EQ(rc.fixed_k3739.error, want.fixed_k3739.error);
    EXPECT_EQ(rc.allow_negative_ca_correction, want.allow_negative_ca_correction);
    run_pipeline_case(c, n);
  }
  EXPECT_EQ(base_legacy, base_prefs);
  EXPECT_EQ(base_legacy.size(), 27u);
  EXPECT_EQ(n.legacy, 27u);
  EXPECT_EQ(n.prefs, 27u);
}

// ---- Step order (spec 3.1-3.2, arar_age.py:568-599) ------------------------

TEST(Reduce, StepOrder) {
  ReductionInput in = base_input();
  in.constants.abundance_sensitivity = 1e-4;
  in.irradiation.segments = {{1.0, 1.0, 365.0}, {0.8, 0.5, 300.0}};
  in.isotopes[index(ArgonIsotope::Ar40)].deadtime_tau_s = 1e-8;
  const auto r = reduce(in);
  ASSERT_TRUE(r) << r.error().what;

  // E5 on the intercept, then E1-E3 per isotope.
  std::array<UFloat, 5> s;
  for (const ArgonIsotope iso : kArgonKeys) {
    IsotopeSignal sig = in.isotopes[index(iso)];
    const auto dt = deadtime_corrected_intercept(sig);
    ASSERT_TRUE(dt);
    sig.intercept = *dt;
    s[index(iso)] = corrected_intensity(sig);
  }
  // E4 before E7/E8.
  const std::array<UFloat, 5> n = abundance_sensitivity_correction(s, 1e-4);
  const auto df = decay_factors(in.constants.lambda_ar37.value, in.constants.lambda_ar39.value,
                                in.irradiation.segments);
  ASSERT_TRUE(df);
  EXPECT_GT(df->df37, 10.0);
  EXPECT_GT(df->df39, 1.0);
  EXPECT_EQ(r->decay.df37, df->df37);
  EXPECT_EQ(r->decay.df39, df->df39);
  // E8: 37 and 39 only.
  expect_identical(r->corrected[0], n[0], "Ar40");
  expect_identical(r->corrected[1], n[1] * df->df39, "Ar39");
  expect_identical(r->corrected[2], n[2], "Ar38");
  expect_identical(r->corrected[3], n[3] * df->df37, "Ar37");
  expect_identical(r->corrected[4], n[4], "Ar36");
  // Decay before abundance sensitivity would move Ar38 and Ar36 (their
  // neighbours include 37).
  std::array<UFloat, 5> decayed_first = s;
  decayed_first[3] = s[3] * df->df37;
  decayed_first[1] = s[1] * df->df39;
  const auto wrong = abundance_sensitivity_correction(decayed_first, 1e-4);
  EXPECT_NE(r->corrected[2].nominal(), wrong[2].nominal());
  EXPECT_NE(r->corrected[4].nominal(), wrong[4].nominal());
  // calculate_f received exactly `corrected`.
  expect_identical(r->f.interference_corrected[3], r->corrected[3], "a37 into calculate_f");
  expect_identical(r->f.interference_corrected[2], r->corrected[2], "a38 into calculate_f");

  // `corrected` equals legacy corrected_intensities wherever E4 or E7 act.
  const g::Json doc = g::load("pipeline.json");
  std::size_t checked = 0;
  for (const g::Json& c : doc["cases"].as_array()) {
    const std::string name = c["name"].string;
    const g::Json& in_j = c["inputs"];
    if (!c["expect_error"].is_null()) continue;
    if (in_j["irradiation"]["segments"].size() == 0 &&
        in_j["constants"]["abundance_sensitivity"].as_number() == 0.0) {
      continue;
    }
    SCOPED_TRACE(name);
    ReductionConstants rc;
    if (!constants_of(in_j["constants"], rc, name)) continue;
    Built b = build_input(in_j, rc, name);
    const auto rr = reduce(b.in);
    if (!rr) {
      ADD_FAILURE() << rr.error().what;
      continue;
    }
    ++checked;
    check_group(c["expected"]["corrected"], by_isotope(rr->corrected), g::tol_of(c), "corrected");
  }
  // segments, multi_segments, abundance_sensitivity, chlorine_with_segments x 2 presets
  EXPECT_EQ(checked, 8u);
}

// ---- Ages need J ---------------------------------------------------------

TEST(Reduce, NoJNoAges) {
  ReductionInput in = base_input();
  in.j.reset();
  const auto r = reduce(in);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_TRUE(r->f.f.has_value());
  EXPECT_FALSE(r->ages.has_value());
  EXPECT_TRUE(r->age_error_components.empty());
  EXPECT_FALSE(has(r->diagnostics, Diagnostic::AgeUndefined));
  EXPECT_TRUE(r->kca.has_value());  // K/Ca does not need J
}

// ---- E19 K/Ca (arar_age.py:534-545, :560-566) -------------------------------

TEST(Reduce, KCaUsesClampedCa37AndCaK) {
  {
    const ReductionInput in = base_input();
    const auto r = reduce(in);
    ASSERT_TRUE(r && r->kca && r->cak);
    const auto& ic = r->f.interference;
    ASSERT_TRUE(in.production.ca_k);
    // Legacy order: k / ca * (1 / Ca_K); Ca_K is the analysis' production
    // variable, so its error propagates.
    expect_identical(*r->kca, ic.k39 / ic.ca37 * (1.0 / *in.production.ca_k), "kca");
    expect_identical(*r->cak, 1.0 / *r->kca, "cak");
    EXPECT_EQ(ids_tagged(*r->kca, "Ca_K").size(), 1u);
  }
  for (const bool zero : {false, true}) {  // missing or zero Ca_K -> factor 1
    ReductionInput in = base_input();
    if (zero) in.production.ca_k = UFloat(0.0);
    else in.production.ca_k.reset();
    const auto r = reduce(in);
    ASSERT_TRUE(r && r->kca);
    const auto& ic = r->f.interference;
    expect_identical(*r->kca, ic.k39 / ic.ca37, zero ? "zero Ca_K" : "missing Ca_K");
  }
  {  // Clamped ca37 (E11) is exact 0: kca, cak absent with KCaUndefined.
    ReductionInput in = base_input();
    in.constants.allow_negative_ca_correction = false;
    MeasuredSignal m;
    m.intercept = {0.5, 0.01};  // k37 = K3739 k39 ~ 1 > a37
    in.isotopes[index(ArgonIsotope::Ar37)] = make_signal(ArgonIsotope::Ar37, m);
    const auto r = reduce(in);
    ASSERT_TRUE(r) << r.error().what;
    EXPECT_EQ(r->f.interference.ca37.nominal(), 0.0);
    EXPECT_FALSE(r->kca.has_value());
    EXPECT_FALSE(r->cak.has_value());
    EXPECT_EQ(g::diagnostic_names(r->diagnostics),
              (std::vector<std::string>{"CaClampedToZero", "KCaUndefined"}));
    // Without the clamp the negative ca37 is used as is.
    in.constants.allow_negative_ca_correction = true;
    const auto rn = reduce(in);
    ASSERT_TRUE(rn && rn->kca);
    EXPECT_LT(rn->kca->nominal(), 0.0);
    EXPECT_TRUE(rn->diagnostics.empty());
  }
}

// kca == 0 with ca37 != 0 (k39 == 0, FUndefined). Legacy assigns kca, then
// 1 / kca raises ZeroDivisionError and kca is reset to 0 +- 0 (arar_age.py:534-545).
// Ruling (Task 11 review): kca kept as computed (0 +- e), cak absent,
// KCaUndefined.
TEST(Reduce, KCaZeroWithNonzeroCa37) {
  ReductionInput in = base_input();
  MeasuredSignal m;
  m.intercept = {0.0, 0.5};  // no blank, no baseline: corrected Ar39 is exactly 0
  in.isotopes[index(ArgonIsotope::Ar39)] = make_signal(ArgonIsotope::Ar39, m);
  in.production.ca3937 = UFloat(0.0);  // ca39 = 0, so k39 = a39 - ca39 = 0 exactly
  const auto r = reduce(in);
  ASSERT_TRUE(r) << r.error().what;
  const auto& ic = r->f.interference;
  EXPECT_EQ(ic.k39.nominal(), 0.0);
  EXPECT_NE(ic.ca37.nominal(), 0.0);
  ASSERT_TRUE(r->kca.has_value());
  ASSERT_TRUE(in.production.ca_k.has_value());
  expect_identical(*r->kca, ic.k39 / ic.ca37 * (1.0 / *in.production.ca_k), "kca");
  EXPECT_EQ(r->kca->nominal(), 0.0);
  EXPECT_GT(r->kca->std_dev(), 0.0);
  EXPECT_FALSE(r->cak.has_value());
  EXPECT_FALSE(r->f.f.has_value());
  EXPECT_TRUE(has(r->diagnostics, Diagnostic::FUndefined));
  EXPECT_EQ(std::count(r->diagnostics.begin(), r->diagnostics.end(), Diagnostic::KCaUndefined),
            1);
  EXPECT_FALSE(has(r->diagnostics, Diagnostic::NonFiniteResult));
}

// ---- E20 error components ----------------------------------------------

TEST(Reduce, ErrorComponentsSumWithJ) {
  const ReductionInput in = base_input();
  const auto r = reduce(in);
  ASSERT_TRUE(r && r->ages) << (r ? "no ages" : r.error().what);
  const UFloat& age = r->ages->age_w_j_err;
  std::set<std::string> keys;
  double isotopes = 0.0;
  for (const auto& [k, v] : r->age_error_components) {
    keys.insert(k);
    isotopes += v;
    EXPECT_GE(v, 0.0) << k;
  }
  EXPECT_EQ(keys, (std::set<std::string>{"Ar36", "Ar37", "Ar38", "Ar39", "Ar40"}));
  const double j = variance_percent(age, intern_tag("J"));
  EXPECT_GT(j, 0.0);
  double rest = 0.0;
  for (const auto& [tag, sd] : error_components(age)) {
    const std::string_view nm = tag_name(tag);
    if (keys.contains(std::string(nm)) || nm == "J") continue;
    rest += 100.0 * sd * sd / age.variance();
  }
  EXPECT_GT(rest, 0.0);  // blanks, ratios, trapped_4036, atm3836
  EXPECT_NEAR(isotopes + j + rest, 100.0, 1e-8);
  // Each isotope component is its intercept tag's share (E20).
  for (const auto& [k, v] : r->age_error_components) {
    EXPECT_NEAR(v, variance_percent(age, intern_tag(k)), 1e-12) << k;
  }
}

// ---- Correlation through shared UFloats (Q2, owner requirement) ------------

TEST(Reduce, SharedBlankCorrelation) {
  const g::Json doc = g::load("correlation.json");
  std::size_t n_pair = 0, n_j = 0, n_blank = 0, n_indep = 0;
  for (const g::Json& c : doc["cases"].as_array()) {
    const g::Json& in = c["inputs"];
    if (in["function"].as_string() != "reduce_pair") continue;  // age_equation_pair: test_age
    const std::string name = c["name"].string;
    SCOPED_TRACE(name);
    ++n_pair;
    known_keys(c,
               {"name", "source", "inputs", "expected", "tol", "legacy_sentinel",
                "expect_diagnostics", "expect_error"},
               name);
    known_keys(in, {"function", "analyses", "constants", "shared"}, name + " inputs");
    if (!c["legacy_sentinel"].is_null() || c["expect_diagnostics"].size() != 0 ||
        !c["expect_error"].is_null()) {
      ADD_FAILURE() << name << ": unhandled sentinel/diagnostics/error";
    }
    ReductionConstants rc;
    if (!constants_of(in["constants"], rc, name)) continue;
    Built a = build_input(in["analyses"]["A"], rc, name + " A");
    Built b = build_input(in["analyses"]["B"], rc, name + " B");
    if (!a.ok || !b.ok) continue;
    const g::Json& shared = in["shared"];
    if (shared.size() == 0) ++n_indep;
    for (const auto& [key, v] : shared.as_object()) {
      if (key == "j") {
        ++n_j;
        EXPECT_TRUE(v.as_bool()) << name << ": shared j";
        if (!v.as_bool()) continue;
        b.in.j = a.in.j;  // one J UFloat for both
      } else if (key == "blank") {
        ++n_blank;
        bool found = false;
        for (const ArgonIsotope iso : kArgonKeys) {
          if (to_string(iso) != v.as_string()) continue;
          b.in.isotopes[index(iso)].blank = a.in.isotopes[index(iso)].blank;  // one blank UFloat
          found = true;
        }
        EXPECT_TRUE(found) << v.string;
      } else {
        ADD_FAILURE() << name << ": unhandled shared key " << key;
      }
    }
    const auto ra = reduce(a.in);
    const auto rb = reduce(b.in);
    if (!ra || !rb || !ra->f.f || !rb->f.f || !ra->ages || !rb->ages) {
      ADD_FAILURE() << name << ": reduce failed or values absent";
      continue;
    }
    const std::map<std::string, const UFloat*> got{
        {"A.f", &*ra->f.f},
        {"B.f", &*rb->f.f},
        {"A.age", &ra->ages->age},
        {"B.age", &rb->ages->age},
        {"A.age_w_j_err", &ra->ages->age_w_j_err},
        {"B.age_w_j_err", &rb->ages->age_w_j_err}};
    const g::Tol t = g::tol_of(c);
    std::size_t n_cov = 0;
    for (const auto& [key, w] : c["expected"].as_object()) {
      if (key == "cov") {
        for (const g::Json& e : w.as_array()) {
          const auto ia = got.find(e[0].as_string());
          const auto ib = got.find(e[1].as_string());
          if (ia == got.end() || ib == got.end()) {
            ADD_FAILURE() << name << ": unhandled cov key " << e[0].string << "/" << e[1].string;
            continue;
          }
          ++n_cov;
          g::expect_close(covariance(*ia->second, *ib->second), e[2].as_number(), t.rtol_err,
                          t.atol_err, "cov " + e[0].string + "," + e[1].string);
        }
        continue;
      }
      const auto it = got.find(key);
      if (it == got.end()) {
        ADD_FAILURE() << name << ": unhandled expected key " << key;
        continue;
      }
      check_u(*it->second, w, t, key);
    }
    EXPECT_EQ(n_cov, 21u);
  }
  EXPECT_EQ(n_pair, 3u);
  EXPECT_EQ(n_j, 1u);
  EXPECT_EQ(n_blank, 1u);
  EXPECT_EQ(n_indep, 1u);

  // make_signal mints fresh blanks per analysis: no correlation; sharing the
  // blank UFloat correlates F.
  ReductionInput a = base_input();
  ReductionInput b = base_input();
  const auto fresh_a = reduce(a);
  const auto fresh_b = reduce(b);
  ASSERT_TRUE(fresh_a && fresh_b && fresh_a->f.f && fresh_b->f.f);
  EXPECT_EQ(covariance(*fresh_a->f.f, *fresh_b->f.f), 0.0);
  b.isotopes[index(ArgonIsotope::Ar36)].blank = a.isotopes[index(ArgonIsotope::Ar36)].blank;
  const auto sa = reduce(a);
  const auto sb = reduce(b);
  ASSERT_TRUE(sa && sb && sa->f.f && sb->f.f);
  EXPECT_GT(covariance(*sa->f.f, *sb->f.f), 0.0);
}

// ---- Spec 5.6 / 7: NaN from valid inputs is flagged once, not an error -----

TEST(Reduce, NonFiniteFlaggedOnce) {
  ReductionInput in = base_input();
  // E9 divisor 1 - K3739 Ca3937 == 0 exactly.
  in.production.k3739 = UFloat(1.0 / 0.5);
  in.production.ca3937 = UFloat(0.5);
  const auto r = reduce(in);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(std::count(r->diagnostics.begin(), r->diagnostics.end(), Diagnostic::NonFiniteResult),
            1);
  EXPECT_FALSE(r->ages.has_value());  // no age from a non-finite F
  EXPECT_FALSE(has(r->diagnostics, Diagnostic::AgeUndefined));

  // A legacy crash case stays an error (E13 zero divisor rc == rs, Q16).
  ReductionInput bad = base_input();
  bad.constants.cosmogenic = CosmogenicRatios{{0.18, 0.001}, {0.18, 0.01}};
  expect_config_error(reduce(bad), "zero divisor", "cosmogenic rc == rs");
}

// ---- Golden: every pipeline.json case ----------------------------------------
// The four chlorine cases are also run by Chlorine.Golden (test_chlorine.cpp).

TEST(Reduce, Golden) {
  const g::Json doc = g::load("pipeline.json");
  const g::Json& cases = doc["cases"];
  ASSERT_EQ(cases.size(), 54u);
  PipelineCounts n;
  std::size_t chlorine = 0;
  for (const g::Json& c : cases.as_array()) {
    const std::string name = c["name"].string;
    if (g::is_chlorine(name)) ++chlorine;
    SCOPED_TRACE(name);
    run_pipeline_case(c, n);
  }
  EXPECT_EQ(chlorine, 4u);     // chlorine_cl3638, chlorine_with_segments
  EXPECT_EQ(n.legacy, 27u);
  EXPECT_EQ(n.prefs, 27u);
  EXPECT_EQ(n.error, 2u);      // missing_isotope
  EXPECT_EQ(n.sentinel, 8u);   // zero_ar37, zero_ar40, zero_k39, age_undefined
  EXPECT_EQ(n.diag, 8u);
  EXPECT_EQ(n.cosmo, 2u);
  EXPECT_EQ(n.no_j, 2u);
  EXPECT_EQ(n.lambda_override, 4u);  // lambda_k_override, lambda_k_zero_ignored
  EXPECT_EQ(n.components, 46u);
  EXPECT_EQ(n.kcl, 50u);             // all but missing_isotope and zero_k39
  EXPECT_EQ(n.kcl_undefined, 2u);    // zero_k39: cl38 == 0 exactly
}
