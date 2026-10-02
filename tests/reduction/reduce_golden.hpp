// Test-only: reduce() golden cases (pipeline.json, chlorine.json "reduce",
// correlation.json reduce_pair analyses) -> ReductionInput, and the shared
// ArArResult / FResult comparisons (spec 9.4). Used by test_reduce.cpp and
// test_chlorine.cpp.
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "golden.hpp"
#include "pychron/reduction/arar_reduction.hpp"

namespace pychron::reduction::golden {

inline std::vector<std::string> diagnostic_names(const std::vector<Diagnostic>& d) {
  std::vector<std::string> out;
  for (const Diagnostic x : d) out.emplace_back(to_string(x));
  return out;
}

inline bool contains(const std::vector<std::string>& v, std::string_view x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

// A Config error prefixed "reduction: " whose message names `needle`.
inline void expect_config_error(const Result<ArArResult>& r, std::string_view needle,
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
inline Built build_input(const Json& in, const ReductionConstants& c, const std::string& name) {
  Built b;
  b.ok = known_keys(in,
                    {"function", "constants", "isotopes", "production", "irradiation", "j",
                     "position_jerr", "lambda_k_total", "fixed_k3739"},
                    name + " inputs");
  b.in.constants = c;
  for (const ArgonIsotope iso : kArgonKeys) {
    const std::string key(to_string(iso));
    const Json& j = in["isotopes"][key];
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
  const Json& irr = in["irradiation"];
  b.ok = known_keys(irr, {"decay_days", "segments"}, name + " irradiation") && b.ok;
  b.in.irradiation.decay_days = irr["decay_days"].as_number();
  for (const Json& s : irr["segments"].as_array()) {
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

inline bool is_chlorine(const std::string& name) { return name.find("chlorine") != std::string::npos; }

// ---- Golden comparison ------------------------------------------------------

// Per-category tallies of the cases run_pipeline_case consumed.
struct PipelineCounts {
  std::size_t legacy = 0, prefs = 0, sentinel = 0, error = 0, diag = 0, cosmo = 0, no_j = 0,
              lambda_override = 0, components = 0;
  std::size_t kcl = 0;            // cases with expected kcl and clk
  std::size_t kcl_undefined = 0;  // cases expecting KClUndefined (kcl/clk sentinels)
};

inline void check_u(const UFloat& got, const Json& w, const Tol& t, const std::string& what) {
  expect_close(got.nominal(), w["v"].as_number(), t.rtol, t.atol, what + ".v");
  expect_close(got.std_dev(), w["e"].as_number(), t.rtol_err, t.atol_err, what + ".e");
}

inline void check_group(const Json& w, const std::map<std::string, const UFloat*>& got,
                        const Tol& t, const std::string& group) {
  if (w.size() != got.size()) {
    ADD_FAILURE() << group << ": expected key count " << w.size() << ", got " << got.size();
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

inline std::map<std::string, const UFloat*> by_isotope(const std::array<UFloat, 5>& a) {
  std::map<std::string, const UFloat*> out;
  for (const ArgonIsotope iso : kArgonKeys) out[std::string(to_string(iso))] = &a[index(iso)];
  return out;
}

// FResult as calculate_f.json writes it.
inline void check_f(const Json& w, const FResult& r, const Tol& t) {
  for (const auto& [key, v] : w.as_object()) {
    if (key == "f") {
      if (!r.f) {
        ADD_FAILURE() << "f absent";
        continue;
      }
      check_u(*r.f, v, t, "f.f");
    } else if (key == "f_err_wo_irrad") {
      expect_close(r.f_err_wo_irrad, v.as_number(), t.rtol_err, t.atol_err, "f." + key);
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
  if (!w.contains("cosmogenic")) { EXPECT_FALSE(r.cosmogenic.has_value()); }
  if (!w.contains("f")) { EXPECT_FALSE(r.f.has_value()); }
  if (!w.contains("radiogenic_yield")) { EXPECT_FALSE(r.radiogenic_yield.has_value()); }
}

// One reduce case (pipeline.json, chlorine.json "reduce"), every key checked;
// unhandled keys fail.
inline void run_pipeline_case(const Json& c, PipelineCounts& n) {
  const std::string name = c["name"].string;
  known_keys(c,
             {"name", "source", "inputs", "expected", "tol", "legacy_sentinel",
              "expect_diagnostics", "expect_error"},
             name);
  const Json& in = c["inputs"];
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
  const Tol t = tol_of(c);

  std::vector<std::string> want_diags;
  for (const Json& d : c["expect_diagnostics"].as_array()) want_diags.push_back(d.as_string());
  if (!want_diags.empty()) ++n.diag;
  if (contains(want_diags, "KClUndefined")) ++n.kcl_undefined;
  EXPECT_EQ(diagnostic_names(r->diagnostics), want_diags);

  const Json& sentinel = c["legacy_sentinel"];
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
        // Legacy kcl = 0 (clk unset) on ZeroDivisionError, arar_age.py:547-558.
        EXPECT_FALSE((key == "kcl" ? r->kcl : r->clk).has_value()) << key;
        EXPECT_TRUE(contains(want_diags, "KClUndefined"));
      } else {
        ADD_FAILURE() << name << ": unhandled legacy_sentinel key " << key;
      }
    }
  }

  const Json& expected = c["expected"];
  for (const auto& [key, w] : expected.as_object()) {
    if (key == "decay") {
      known_keys(w, {"df37", "df39"}, name + " decay");
      // Spec 4.7: decay factors 1e-13.
      expect_close(r->decay.df37, w["df37"].as_number(), 1e-13, 0.0, "decay.df37");
      expect_close(r->decay.df39, w["df39"].as_number(), 1e-13, 0.0, "decay.df39");
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
          expect_close(a.age_err_wo_irrad, av.as_number(), t.rtol_err, t.atol_err, ak);
        else if (ak == "age_err_wo_j_irrad")
          expect_close(a.age_err_wo_j_irrad, av.as_number(), t.rtol_err, t.atol_err, ak);
        else ADD_FAILURE() << name << ": unhandled ages key " << ak;
      }
      EXPECT_EQ(w.size(), 5u);
    } else if (key == "kca" || key == "cak" || key == "kcl" || key == "clk") {
      const std::optional<UFloat>& got = key == "kca"   ? r->kca
                                         : key == "cak" ? r->cak
                                         : key == "kcl" ? r->kcl
                                                        : r->clk;
      if (!got) {
        ADD_FAILURE() << name << ": " << key << " absent";
        continue;
      }
      check_u(*got, w, t, key);
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
        expect_close(it->second, pct.as_number(), 0.0, 1e-8, "age_error_components." + iso);
      }
    } else {
      ADD_FAILURE() << name << ": unhandled expected key " << key;
    }
  }
  if (!expected.contains("ages")) { EXPECT_FALSE(r->ages.has_value()); }
  if (!expected.contains("age_error_components")) { EXPECT_TRUE(r->age_error_components.empty()); }
  if (!expected.contains("kca")) { EXPECT_FALSE(r->kca.has_value()); }
  if (!expected.contains("cak")) { EXPECT_FALSE(r->cak.has_value()); }
  if (!expected.contains("kcl")) { EXPECT_FALSE(r->kcl.has_value()); }
  if (!expected.contains("clk")) { EXPECT_FALSE(r->clk.has_value()); }
  if (expected.contains("kcl") && expected.contains("clk")) ++n.kcl;
}

}  // namespace pychron::reduction::golden
