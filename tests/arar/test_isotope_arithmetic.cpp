// Ar-Ar isotope arithmetic: E4 abundance sensitivity over the five argon
// isotopes and the isotope_arithmetic.json golden vectors (E1-E5). The
// per-signal cases are in tests/reduction/test_isotope_arithmetic.cpp.
#include "pychron/arar/arar_reduction.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <string>

#include "golden.hpp"

namespace golden = pychron::reduction::golden;
using namespace pychron::reduction;

namespace {

UFloat var(double v, double e) { return UFloat::variable(v, e); }

UFloat ufloat_of(const golden::Json& j, std::string_view tag = {}) {
  return UFloat::variable(j["v"].as_number(), j["e"].as_number(), tag);
}

void expect_ufloat(const UFloat& got, const golden::Json& want, const golden::Tol& t,
                   const std::string& what) {
  golden::expect_close(got.nominal(), want["v"].as_number(), t.rtol, t.atol, what + " v");
  golden::expect_close(got.std_dev(), want["e"].as_number(), t.rtol_err, t.atol_err,
                       what + " e");
}

// |got - want| > atol + rtol |want| for the value or the error.
bool differs(const UFloat& got, const golden::Json& want, const golden::Tol& t) {
  const double wv = want["v"].as_number();
  const double we = want["e"].as_number();
  return std::fabs(got.nominal() - wv) > t.atol + t.rtol * std::fabs(wv) ||
         std::fabs(got.std_dev() - we) > t.atol_err + t.rtol_err * std::fabs(we);
}

const golden::Json& find_case(const golden::Json& doc, std::string_view name) {
  for (const golden::Json& c : doc["cases"].as_array()) {
    if (c["name"].as_string() == name) return c;
  }
  ADD_FAILURE() << "golden case not found: " << name;
  return golden::Json::null_value();
}

}  // namespace

TEST(IsotopeArithmetic, AbundanceSensitivityNeighbours) {
  const std::array<UFloat, 5> s{var(1000.0, 1.0), var(100.0, 0.5), var(10.0, 0.05),
                                var(5.0, 0.01), var(2.0, 0.01)};
  const double s40 = 1000.0, s39 = 100.0, s38 = 10.0, s37 = 5.0, s36 = 2.0;
  const double a = 1e-4;
  const auto n = abundance_sensitivity_correction(s, a);
  EXPECT_DOUBLE_EQ(n[0].nominal(), s40 - a * (s39 + s39));  // 2*s39 (spec Q9)
  EXPECT_DOUBLE_EQ(n[1].nominal(), s39 - a * (s40 + s38));
  EXPECT_DOUBLE_EQ(n[2].nominal(), s38 - a * (s39 + s37));
  EXPECT_DOUBLE_EQ(n[3].nominal(), s37 - a * (s38 + s36));
  EXPECT_DOUBLE_EQ(n[4].nominal(), s36 - a * (s37 + s37));  // 2*s37 (spec Q9)
  EXPECT_DOUBLE_EQ(n[0].derivative(s[1].variable_id()), -2.0 * a);
  EXPECT_DOUBLE_EQ(n[4].derivative(s[3].variable_id()), -2.0 * a);
  EXPECT_EQ(n[0].derivative(s[2].variable_id()), 0.0);  // no 41 neighbour
  EXPECT_EQ(n[4].derivative(s[2].variable_id()), 0.0);  // no 35 neighbour
  EXPECT_DOUBLE_EQ(n[2].derivative(s[1].variable_id()), -a);
  EXPECT_DOUBLE_EQ(n[2].derivative(s[3].variable_id()), -a);

  const auto same = abundance_sensitivity_correction(s, 0.0);
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(same[i].nominal(), s[i].nominal()) << i;
    ASSERT_EQ(same[i].terms().size(), 1U) << i;
    EXPECT_EQ(same[i].terms()[0].id, s[i].variable_id()) << i;
    EXPECT_EQ(same[i].terms()[0].deriv, 1.0) << i;
  }
}

TEST(IsotopeArithmetic, DeadtimeLegacy6240Divergence) {
  const golden::Json doc = golden::load("isotope_arithmetic.json");
  const golden::Json& c = find_case(doc, "deadtime/legacy_6240");
  ASSERT_TRUE(c.is_object());
  const golden::Json& in = c["inputs"];
  const golden::Tol t = golden::tol_of(c);
  const UFloat s = ufloat_of(in["signal"]);
  const double tau = in["tau_s"].as_number();
  ASSERT_EQ(in["legacy_fa_to_cps"].as_number(), 6240.0);

  const auto legacy = deadtime_correct(s, tau, 6240.0);
  ASSERT_TRUE(legacy);
  expect_ufloat(*legacy, c["legacy_sentinel"]["corrected"], t, "6240 vs legacy_sentinel");

  const auto ours = deadtime_correct(s, tau);  // default 6241.509 (D4)
  ASSERT_TRUE(ours);
  expect_ufloat(*ours, c["expected"]["corrected"], t, "default vs expected");
  EXPECT_TRUE(differs(*ours, c["legacy_sentinel"]["corrected"], t))
      << "default factor should diverge from the legacy 6240 result";
}

TEST(IsotopeArithmetic, Golden) {
  const golden::Json doc = golden::load("isotope_arithmetic.json");
  ASSERT_EQ(doc["cases"].size(), 22U);
  std::size_t n_isotope = 0, n_deadtime = 0, n_sensitivity = 0, n_error = 0, n_sentinel = 0;
  for (const golden::Json& c : doc["cases"].as_array()) {
    const std::string name = c["name"].as_string();
    SCOPED_TRACE(name);
    golden::known_keys(c,
                       {"name", "source", "inputs", "expected", "tol", "legacy_sentinel",
                        "expect_diagnostics", "expect_error"},
                       name);
    const golden::Json& in = c["inputs"];
    const golden::Json& ex = c["expected"];
    const golden::Tol t = golden::tol_of(c);
    const std::string fn = in["function"].as_string();
    // No step here raises diagnostics.
    if (c["expect_diagnostics"].size() != 0) {
      ADD_FAILURE() << "unhandled expect_diagnostics";
      continue;
    }
    // Only deadtime cases carry an expected error or a legacy sentinel.
    if (fn != "deadtime" && (!c["expect_error"].is_null() || !c["legacy_sentinel"].is_null())) {
      ADD_FAILURE() << "unhandled expect_error / legacy_sentinel for " << fn;
      continue;
    }
    ReductionConstants rc;
    if (!golden::constants_of(in["constants"], rc, name)) continue;

    if (fn == "isotope") {
      ++n_isotope;
      golden::known_keys(in,
                         {"function", "constants", "isotope", "intercept", "baseline", "blank",
                          "ic_factor", "discrimination", "include_baseline_error",
                          "correct_for_blank"},
                         name + " inputs");
      golden::known_keys(ex, {"baseline_corrected", "non_detector_corrected", "intensity"},
                         name + " expected");
      EXPECT_EQ(ex.size(), 3U);
      const std::string iso = in["isotope"].as_string();
      IsotopeSignal s;
      s.intercept = ufloat_of(in["intercept"], iso);
      s.baseline = ufloat_of(in["baseline"], iso + " bs");
      s.blank = ufloat_of(in["blank"], iso + " bk");
      s.ic_factor = ufloat_of(in["ic_factor"], iso + " IC");
      s.discrimination = ufloat_of(in["discrimination"]);
      s.include_baseline_error = in["include_baseline_error"].as_bool();
      s.correct_for_blank = in["correct_for_blank"].as_bool();
      expect_ufloat(baseline_corrected(s), ex["baseline_corrected"], t, "baseline_corrected");
      expect_ufloat(non_detector_corrected(s), ex["non_detector_corrected"], t,
                    "non_detector_corrected");
      expect_ufloat(corrected_intensity(s), ex["intensity"], t, "intensity");
    } else if (fn == "abundance_sensitivity") {
      ++n_sensitivity;
      golden::known_keys(in, {"function", "constants", "signals", "abundance_sensitivity"},
                         name + " inputs");
      static constexpr std::array<const char*, 5> kKeys{"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"};
      golden::known_keys(in["signals"], {"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"},
                         name + " signals");
      golden::known_keys(ex, {"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"}, name + " expected");
      EXPECT_EQ(ex.size(), 5U);
      const double alpha = in["abundance_sensitivity"].as_number();
      EXPECT_EQ(rc.abundance_sensitivity, alpha);
      std::array<UFloat, 5> s;
      for (std::size_t i = 0; i < 5; ++i) s[i] = ufloat_of(in["signals"][kKeys[i]], kKeys[i]);
      const auto n = abundance_sensitivity_correction(s, alpha);
      for (std::size_t i = 0; i < 5; ++i) expect_ufloat(n[i], ex[kKeys[i]], t, kKeys[i]);
    } else if (fn == "deadtime") {
      ++n_deadtime;
      golden::known_keys(in,
                         {"function", "constants", "signal", "tau_s", "fa_to_cps",
                          "legacy_fa_to_cps"},
                         name + " inputs");
      const UFloat s = ufloat_of(in["signal"]);
      const auto r = deadtime_correct(s, in["tau_s"].as_number(), in["fa_to_cps"].as_number());
      if (!c["expect_error"].is_null()) {
        ++n_error;
        EXPECT_EQ(ex.size(), 0U);
        if (r) {
          ADD_FAILURE() << "expected an error naming " << c["expect_error"].as_string();
          continue;
        }
        EXPECT_NE(r.error().what.find(c["expect_error"].as_string()), std::string::npos)
            << r.error().what;
        continue;
      }
      golden::known_keys(ex, {"corrected"}, name + " expected");
      if (!r) {
        ADD_FAILURE() << r.error().what;
        continue;
      }
      expect_ufloat(*r, ex["corrected"], t, "corrected");
      // A legacy sentinel always comes with the legacy factor, and vice versa.
      EXPECT_EQ(in.contains("legacy_fa_to_cps"), !c["legacy_sentinel"].is_null());
      if (in.contains("legacy_fa_to_cps")) {
        ++n_sentinel;
        golden::known_keys(c["legacy_sentinel"], {"corrected"}, name + " legacy_sentinel");
        const auto l = deadtime_correct(s, in["tau_s"].as_number(),
                                        in["legacy_fa_to_cps"].as_number());
        if (!l) {
          ADD_FAILURE() << l.error().what;
          continue;
        }
        expect_ufloat(*l, c["legacy_sentinel"]["corrected"], t, "legacy_sentinel");
      }
    } else {
      ADD_FAILURE() << "unknown function " << fn;
    }
  }
  EXPECT_EQ(n_isotope, 12U);
  EXPECT_EQ(n_deadtime, 6U);
  EXPECT_EQ(n_sensitivity, 4U);
  EXPECT_EQ(n_error, 1U);     // deadtime/saturated
  EXPECT_EQ(n_sentinel, 1U);  // deadtime/legacy_6240
}
