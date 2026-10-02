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

using namespace pychron::reduction;
using pychron::Result;
namespace g = pychron::reduction::golden;

namespace {

std::string fmt(const char* what, double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%g", v);
  return std::string(what) + buf;
}

std::vector<std::string> names_of(const std::vector<Diagnostic>& d) {
  std::vector<std::string> out;
  for (const Diagnostic x : d) out.emplace_back(to_string(x));
  return out;
}

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

void expect_config_error(const Result<ArArResult>& r, std::string_view needle,
                         const std::string& what) {
  if (r) {
    ADD_FAILURE() << what << ": expected an error naming '" << needle << "'";
    return;
  }
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config) << what;
  EXPECT_TRUE(r.error().what.starts_with("reduction: ")) << what << ": " << r.error().what;
  EXPECT_NE(r.error().what.find(needle), std::string::npos)
      << what << ": '" << r.error().what << "' does not name '" << needle << "'";
}

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

Measured measured_of(const g::Json& j) { return {j["v"].as_number(), j["e"].as_number()}; }

bool units_of(std::string_view s, AgeUnits& out) {
  if (s == "a") out = AgeUnits::a;
  else if (s == "ka") out = AgeUnits::ka;
  else if (s == "Ma") out = AgeUnits::Ma;
  else if (s == "Ga") out = AgeUnits::Ga;
  else return false;
  return true;
}

// Every constants key is read; an unknown key or value fails.
bool constants_of(const g::Json& k, ReductionConstants& rc, const std::string& name) {
  bool ok = true;
  for (const auto& [key, v] : k.as_object()) {
    if (key == "lambda_b") rc.lambda_b = measured_of(v);
    else if (key == "lambda_e") rc.lambda_e = measured_of(v);
    else if (key == "include_decay_error") rc.include_decay_error = v.as_bool();
    else if (key == "age_units") {
      if (!units_of(v.as_string(), rc.age_units)) {
        ADD_FAILURE() << name << ": unknown age_units " << v.string;
        ok = false;
      }
    } else if (key == "lambda_cl36") rc.lambda_cl36 = measured_of(v);
    else if (key == "lambda_ar37") rc.lambda_ar37 = measured_of(v);
    else if (key == "lambda_ar39") rc.lambda_ar39 = measured_of(v);
    else if (key == "atm4036") rc.atm4036 = measured_of(v);
    else if (key == "atm4038") rc.atm4038 = measured_of(v);
    else if (key == "fixed_k3739") rc.fixed_k3739 = measured_of(v);
    else if (key == "allow_negative_ca_correction") rc.allow_negative_ca_correction = v.as_bool();
    else if (key == "abundance_sensitivity") rc.abundance_sensitivity = v.as_number();
    else if (key == "use_irradiation_endtime") rc.use_irradiation_endtime = v.as_bool();
    else if (key == "k3739_mode") {
      if (v.as_string() == "Fixed") {
        rc.k3739_mode = K3739Mode::Fixed;
      } else if (v.as_string() != "Normal") {
        ADD_FAILURE() << name << ": unknown k3739_mode " << v.string;
        ok = false;
      }
    } else if (key == "cosmogenic") {
      if (!v.is_null()) {
        rc.cosmogenic = CosmogenicRatios{measured_of(v["solar3836"]), measured_of(v["cosmo3836"])};
      }
    } else {
      ADD_FAILURE() << name << ": unhandled constants key " << key;
      ok = false;
    }
  }
  return ok;
}

bool known_keys(const g::Json& obj, std::initializer_list<std::string_view> keys,
                const std::string& what) {
  bool ok = true;
  for (const auto& [key, v] : obj.as_object()) {
    bool found = false;
    for (const std::string_view k : keys) found = found || key == k;
    if (!found) {
      ADD_FAILURE() << what << ": unhandled key " << key;
      ok = false;
    }
  }
  return ok;
}

struct Built {
  ReductionInput in;
  std::string missing;  // first ARGON_KEYS slot absent from the JSON, "" if none
  bool ok = true;
};

// Pipeline inputs (pipeline.json, correlation.json reduce_pair analyses) ->
// ReductionInput. Isotopes go through make_signal (fresh variables, legacy
// tags). The isotope key mapping is the caller's job (spec 5.2): a slot
// missing from the JSON is reported in `missing` and left with a NaN
// intercept, which reduce() must reject naming the slot.
Built build_input(const g::Json& in, const ReductionConstants& c, const std::string& name) {
  Built b;
  b.ok = known_keys(in,
                    {"function", "constants", "isotopes", "production", "irradiation", "j",
                     "position_jerr", "lambda_k_total", "fixed_k3739"},
                    name + " inputs");
  b.in.constants = c;
  for (const ArgonIsotope iso : kArgonKeys) {
    const std::string key(to_string(iso));
    const g::Json& j = in["isotopes"][key];
    if (!j.is_object()) {
      if (b.missing.empty()) b.missing = key;
      b.in.isotopes[index(iso)].intercept = UFloat(std::nan(""));
      continue;
    }
    b.ok = known_keys(j,
                      {"intercept", "baseline", "blank", "ic_factor", "include_baseline_error",
                       "correct_for_blank"},
                      name + " isotope " + key) &&
           b.ok;
    MeasuredSignal m;
    m.intercept = measured_of(j["intercept"]);
    m.baseline = measured_of(j["baseline"]);
    m.blank = measured_of(j["blank"]);
    m.ic_factor = measured_of(j["ic_factor"]);
    m.include_baseline_error = j["include_baseline_error"].as_bool();
    m.correct_for_blank = j["correct_for_blank"].as_bool();
    b.in.isotopes[index(iso)] = make_signal(iso, m);
  }
  std::map<std::string, Measured, std::less<>> rows;
  for (const auto& [key, m] : in["production"].as_object()) rows[key] = measured_of(m);
  auto pr = production_from_rows(rows);
  if (!pr) {
    ADD_FAILURE() << name << ": " << pr.error().what;
    b.ok = false;
  } else {
    b.in.production = make_production_variables(*pr);
  }
  const g::Json& irr = in["irradiation"];
  b.ok = known_keys(irr, {"decay_days", "segments"}, name + " irradiation") && b.ok;
  b.in.irradiation.decay_days = irr["decay_days"].as_number();
  for (const g::Json& s : irr["segments"].as_array()) {
    b.ok = known_keys(s, {"power", "duration_days", "dt_days"}, name + " segment") && b.ok;
    b.in.irradiation.segments.push_back(
        {s["power"].as_number(), s["duration_days"].as_number(), s["dt_days"].as_number()});
  }
  if (!in["j"].is_null()) {
    const Measured jm = measured_of(in["j"]);
    b.in.j = make_j(Flux{jm, 0.0, std::nullopt});
  }
  b.in.position_jerr = in["position_jerr"].as_number();
  if (!in["lambda_k_total"].is_null()) b.in.lambda_k_total = measured_of(in["lambda_k_total"]);
  if (!in["fixed_k3739"].is_null()) b.in.fixed_k3739 = measured_of(in["fixed_k3739"]);
  return b;
}

bool is_chlorine(const std::string& name) { return name.find("chlorine") != std::string::npos; }

// ---- Golden comparison ------------------------------------------------------

struct Counts {
  std::size_t legacy = 0, prefs = 0, sentinel = 0, error = 0, diag = 0, cosmo = 0, no_j = 0,
              lambda_override = 0, kcl_skipped = 0, kcl_diag_skipped = 0, components = 0;
};

void check_u(const UFloat& got, const g::Json& w, const g::Tol& t, const std::string& what) {
  g::expect_close(got.nominal(), w["v"].as_number(), t.rtol, t.atol, what + ".v");
  g::expect_close(got.std_dev(), w["e"].as_number(), t.rtol_err, t.atol_err, what + ".e");
}

void check_group(const g::Json& w, const std::map<std::string, const UFloat*>& got,
                 const g::Tol& t, const std::string& group) {
  if (w.size() != got.size()) {
    ADD_FAILURE() << group << fmt(": expected key count ", static_cast<double>(w.size()));
  }
  for (const auto& [key, v] : w.as_object()) {
    const auto it = got.find(key);
    if (it == got.end()) {
      ADD_FAILURE() << "unhandled key " << group << "." << key;
      continue;
    }
    check_u(*it->second, v, t, group + "." + key);
  }
}

std::map<std::string, const UFloat*> by_isotope(const std::array<UFloat, 5>& a) {
  std::map<std::string, const UFloat*> out;
  for (const ArgonIsotope iso : kArgonKeys) out[std::string(to_string(iso))] = &a[index(iso)];
  return out;
}

// FResult as calculate_f.json writes it.
void check_f(const g::Json& w, const FResult& r, const g::Tol& t) {
  for (const auto& [key, v] : w.as_object()) {
    if (key == "f") {
      if (!r.f) {
        ADD_FAILURE() << "f absent";
        continue;
      }
      check_u(*r.f, v, t, "f.f");
    } else if (key == "f_err_wo_irrad") {
      g::expect_close(r.f_err_wo_irrad, v.as_number(), t.rtol_err, t.atol_err, "f." + key);
    } else if (key == "radiogenic_yield") {
      if (!r.radiogenic_yield) {
        ADD_FAILURE() << "radiogenic_yield absent";
        continue;
      }
      check_u(*r.radiogenic_yield, v, t, "f." + key);
    } else if (key == "atm40") {
      check_u(r.atm40, v, t, "f." + key);
    } else if (key == "k40") {
      check_u(r.k40, v, t, "f." + key);
    } else if (key == "rad40") {
      check_u(r.rad40, v, t, "f." + key);
    } else if (key == "interference") {
      const auto& x = r.interference;
      check_group(v,
                  {{"k37", &x.k37}, {"k38", &x.k38}, {"k39", &x.k39}, {"ca36", &x.ca36},
                   {"ca37", &x.ca37}, {"ca38", &x.ca38}, {"ca39", &x.ca39}},
                  t, "f." + key);
    } else if (key == "atmospheric") {
      const auto& x = r.atmospheric;
      check_group(
          v, {{"atm36", &x.atm36}, {"atm38", &x.atm38}, {"cl36", &x.cl36}, {"cl38", &x.cl38}},
          t, "f." + key);
    } else if (key == "cosmogenic") {
      if (!r.cosmogenic) {
        ADD_FAILURE() << "cosmogenic absent";
        continue;
      }
      const auto& x = *r.cosmogenic;
      check_group(v,
                  {{"cosmo36", &x.cosmo36}, {"cosmo38", &x.cosmo38},
                   {"noncosmo36", &x.noncosmo36}, {"noncosmo38", &x.noncosmo38}},
                  t, "f." + key);
    } else if (key == "interference_corrected") {
      check_group(v, by_isotope(r.interference_corrected), t, "f." + key);
    } else {
      ADD_FAILURE() << "unhandled expected key f." << key;
    }
  }
  if (!w.contains("cosmogenic")) EXPECT_FALSE(r.cosmogenic.has_value());
  if (!w.contains("f")) EXPECT_FALSE(r.f.has_value());
  if (!w.contains("radiogenic_yield")) EXPECT_FALSE(r.radiogenic_yield.has_value());
}

bool contains(const std::vector<std::string>& v, std::string_view x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

// One non-chlorine pipeline.json case, every key checked; unhandled keys fail.
// kcl / clk (and KClUndefined) are Task 12: skipped here for the non-chlorine
// cases, which still carry legacy kcl/clk from the residual cl38.
void run_pipeline_case(const g::Json& c, Counts& n) {
  const std::string name = c["name"].string;
  known_keys(c,
             {"name", "source", "inputs", "expected", "tol", "legacy_sentinel",
              "expect_diagnostics", "expect_error"},
             name);
  const g::Json& in = c["inputs"];
  if (in["function"].as_string() != "reduce") {
    ADD_FAILURE() << name << ": unhandled function";
    return;
  }
  if (name.ends_with("@legacy")) ++n.legacy;
  else if (name.ends_with("@legacy_preferences")) ++n.prefs;
  else ADD_FAILURE() << name << ": no preset suffix";

  ReductionConstants rc;
  if (!constants_of(in["constants"], rc, name)) return;
  if (rc.cosmogenic) ++n.cosmo;
  Built b = build_input(in, rc, name);
  if (!b.ok) return;
  if (!b.in.j) ++n.no_j;
  if (b.in.lambda_k_total) ++n.lambda_override;

  const Result<ArArResult> r = reduce(b.in);
  if (!c["expect_error"].is_null()) {
    ++n.error;
    // Legacy refuses a missing isotope (arar_age.py:568-581); the C++ caller
    // maps keys to slots (spec 5.2) and reduce() rejects the unfilled slot.
    const std::string want = c["expect_error"].as_string();
    EXPECT_EQ(b.missing, want);
    expect_config_error(r, want, name);
    EXPECT_EQ(c["expected"].size(), 0u);
    return;
  }
  EXPECT_TRUE(b.missing.empty());
  if (!r) {
    ADD_FAILURE() << name << ": " << r.error().what;
    return;
  }
  const g::Tol t = g::tol_of(c);

  std::vector<std::string> want_diags;
  for (const g::Json& d : c["expect_diagnostics"].as_array()) {
    if (d.as_string() == "KClUndefined") {
      ++n.kcl_diag_skipped;  // Task 12
      continue;
    }
    want_diags.push_back(d.as_string());
  }
  if (c["expect_diagnostics"].size() > 0) ++n.diag;
  EXPECT_EQ(names_of(r->diagnostics), want_diags);

  const g::Json& sentinel = c["legacy_sentinel"];
  if (!sentinel.is_null()) {
    ++n.sentinel;
    for (const auto& [key, v] : sentinel.as_object()) {
      // Sentinel "f" / "radiogenic_yield" are values inside expected.f.
      const bool in_f = key == "f" || key == "radiogenic_yield";
      if ((in_f ? c["expected"]["f"] : c["expected"]).contains(key)) {
        ADD_FAILURE() << name << ": sentinel key also expected";
      }
      if (key == "f") {
        EXPECT_FALSE(r->f.f.has_value()) << "legacy F sentinel " << v["v"].as_number();
        EXPECT_TRUE(contains(want_diags, "FUndefined"));
      } else if (key == "radiogenic_yield") {
        EXPECT_FALSE(r->f.radiogenic_yield.has_value());
        EXPECT_TRUE(contains(want_diags, "YieldUndefined"));
      } else if (key == "ages") {
        EXPECT_FALSE(r->ages.has_value()) << "legacy age sentinel " << v["age"]["v"].as_number();
        EXPECT_TRUE(contains(want_diags, "AgeUndefined") || contains(want_diags, "FUndefined"));
      } else if (key == "age_error_components") {
        EXPECT_TRUE(r->age_error_components.empty());
      } else if (key == "kca" || key == "cak") {
        EXPECT_FALSE((key == "kca" ? r->kca : r->cak).has_value()) << key;
        EXPECT_TRUE(contains(want_diags, "KCaUndefined"));
      } else if (key == "kcl" || key == "clk") {
        ++n.kcl_skipped;  // Task 12
      } else {
        ADD_FAILURE() << name << ": unhandled legacy_sentinel key " << key;
      }
    }
  }

  const g::Json& expected = c["expected"];
  for (const auto& [key, w] : expected.as_object()) {
    if (key == "decay") {
      known_keys(w, {"df37", "df39"}, name + " decay");
      // Spec 4.7: decay factors 1e-13.
      g::expect_close(r->decay.df37, w["df37"].as_number(), 1e-13, 0.0, "decay.df37");
      g::expect_close(r->decay.df39, w["df39"].as_number(), 1e-13, 0.0, "decay.df39");
    } else if (key == "corrected") {
      check_group(w, by_isotope(r->corrected), t, "corrected");
    } else if (key == "f") {
      check_f(w, r->f, t);
    } else if (key == "ages") {
      if (!r->ages) {
        ADD_FAILURE() << name << ": ages absent";
        continue;
      }
      const AgeSet& a = *r->ages;
      for (const auto& [ak, av] : w.as_object()) {
        if (ak == "age") check_u(a.age, av, t, "ages.age");
        else if (ak == "age_w_j_err") check_u(a.age_w_j_err, av, t, "ages.age_w_j_err");
        else if (ak == "age_w_position_err")
          check_u(a.age_w_position_err, av, t, "ages.age_w_position_err");
        else if (ak == "age_err_wo_irrad")
          g::expect_close(a.age_err_wo_irrad, av.as_number(), t.rtol_err, t.atol_err, ak);
        else if (ak == "age_err_wo_j_irrad")
          g::expect_close(a.age_err_wo_j_irrad, av.as_number(), t.rtol_err, t.atol_err, ak);
        else ADD_FAILURE() << name << ": unhandled ages key " << ak;
      }
      EXPECT_EQ(w.size(), 5u);
    } else if (key == "kca" || key == "cak") {
      const std::optional<UFloat>& got = key == "kca" ? r->kca : r->cak;
      if (!got) {
        ADD_FAILURE() << name << ": " << key << " absent";
        continue;
      }
      check_u(*got, w, t, key);
    } else if (key == "kcl" || key == "clk") {
      ++n.kcl_skipped;  // Task 12
    } else if (key == "age_error_components") {
      ++n.components;
      EXPECT_EQ(w.size(), 5u);
      EXPECT_EQ(r->age_error_components.size(), 5u);
      for (const auto& [iso, pct] : w.as_object()) {
        const auto it = r->age_error_components.find(iso);
        if (it == r->age_error_components.end()) {
          ADD_FAILURE() << name << ": component " << iso << " absent";
          continue;
        }
        // Spec 4.7: percentages atol 1e-8 percentage points.
        g::expect_close(it->second, pct.as_number(), 0.0, 1e-8, "age_error_components." + iso);
      }
    } else {
      ADD_FAILURE() << name << ": unhandled expected key " << key;
    }
  }
  if (!expected.contains("ages")) EXPECT_FALSE(r->ages.has_value());
  if (!expected.contains("age_error_components")) EXPECT_TRUE(r->age_error_components.empty());
  if (!expected.contains("kca")) EXPECT_FALSE(r->kca.has_value());
  if (!expected.contains("cak")) EXPECT_FALSE(r->cak.has_value());
}

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
    ASSERT_EQ(ia.size(), 1u) << tag;
    ASSERT_EQ(ib.size(), 1u) << tag;
    EXPECT_NE(ia[0], ib[0]) << tag;
    EXPECT_EQ(xb->derivative(ia[0]), 0.0) << tag;
  }
  // E14's trapped_4036 and E12's atm3836 are distinct variables, and no
  // atm4036/atm4038-tagged variable reaches F.
  EXPECT_NE(ids_tagged(fa, "trapped_4036")[0], ids_tagged(atm38a, "atm3836")[0]);
  EXPECT_EQ(fa.derivative(ids_tagged(atm38a, "atm3836")[0]), 0.0);
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
// analyses legacy mints it per get_flux_from_positions call
// (dvc/meta_repo.py:688-691), i.e. fresh per analysis; reduce() takes the
// override as a Measured and mints it once per call, so it is never shared
// between analyses (no caller-shared lambda_k UFloat in this API).
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
    if (!other.empty()) EXPECT_NE(other[0], l_age[0]);
  }
  EXPECT_EQ(seen, 2u);
}

// ---- D2: both legacy default sets ----------------------------------------

TEST(Reduce, BothLegacyPresetsGolden) {
  const g::Json doc = g::load("pipeline.json");
  const ReductionConstants legacy = constants_preset(ConstantsPreset::Legacy);
  const ReductionConstants prefs = constants_preset(ConstantsPreset::LegacyPreferences);
  std::set<std::string> base_legacy, base_prefs;
  Counts n;
  for (const g::Json& c : doc["cases"].as_array()) {
    const std::string name = c["name"].string;
    if (is_chlorine(name)) continue;
    SCOPED_TRACE(name);
    const bool is_legacy = name.ends_with("@legacy");
    const bool is_prefs = name.ends_with("@legacy_preferences");
    ASSERT_TRUE(is_legacy || is_prefs);
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
  EXPECT_EQ(base_legacy.size(), 25u);
  EXPECT_EQ(n.legacy, 25u);
  EXPECT_EQ(n.prefs, 25u);
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
    if (is_chlorine(name) || !c["expect_error"].is_null()) continue;
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
  EXPECT_EQ(checked, 6u);  // segments, multi_segments, abundance_sensitivity x 2 presets
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
    EXPECT_EQ(names_of(r->diagnostics),
              (std::vector<std::string>{"CaClampedToZero", "KCaUndefined"}));
    // Without the clamp the negative ca37 is used as is.
    in.constants.allow_negative_ca_correction = true;
    const auto rn = reduce(in);
    ASSERT_TRUE(rn && rn->kca);
    EXPECT_LT(rn->kca->nominal(), 0.0);
    EXPECT_TRUE(rn->diagnostics.empty());
  }
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
        ASSERT_TRUE(v.as_bool());
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

// ---- Golden: every non-chlorine pipeline.json case -------------------------

TEST(Reduce, Golden) {
  const g::Json doc = g::load("pipeline.json");
  const g::Json& cases = doc["cases"];
  ASSERT_EQ(cases.size(), 54u);
  Counts n;
  std::size_t chlorine = 0;
  for (const g::Json& c : cases.as_array()) {
    const std::string name = c["name"].string;
    if (is_chlorine(name)) {
      ++chlorine;  // Task 12 (test_chlorine.cpp)
      continue;
    }
    SCOPED_TRACE(name);
    run_pipeline_case(c, n);
  }
  EXPECT_EQ(chlorine, 4u);
  EXPECT_EQ(n.legacy, 25u);
  EXPECT_EQ(n.prefs, 25u);
  EXPECT_EQ(n.error, 2u);      // missing_isotope
  EXPECT_EQ(n.sentinel, 8u);   // zero_ar37, zero_ar40, zero_k39, age_undefined
  EXPECT_EQ(n.diag, 8u);
  EXPECT_EQ(n.cosmo, 2u);
  EXPECT_EQ(n.no_j, 2u);
  EXPECT_EQ(n.lambda_override, 4u);  // lambda_k_override, lambda_k_zero_ignored
  EXPECT_EQ(n.components, 42u);
  EXPECT_EQ(n.kcl_diag_skipped, 2u);  // zero_k39: Task 12
  EXPECT_GT(n.kcl_skipped, 0u);
}
