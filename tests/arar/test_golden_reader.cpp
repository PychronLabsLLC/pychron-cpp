// Golden-vector reader (spec 9.4) and the shape of the committed golden files
// produced by tools/reduction_golden/generate.py (spec 9.1-9.3).
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>

#include "golden.hpp"

namespace golden = pychron::reduction::golden;

namespace {

constexpr std::array<std::string_view, 11> kGoldenFiles{
    "ufloat.json",      "constants.json", "isotope_arithmetic.json", "decay_factors.json",
    "interference.json", "atmospheric.json", "calculate_f.json",      "age.json",
    "pipeline.json",    "chlorine.json",  "correlation.json"};

constexpr std::array<std::string_view, 5> kPresetSensitive{
    "interference.json", "calculate_f.json", "age.json", "pipeline.json", "chlorine.json"};

constexpr std::array<std::string_view, 8> kMeasuredConstants{
    "lambda_b",    "lambda_e", "lambda_cl36", "lambda_ar37",
    "lambda_ar39", "atm4036",  "atm4038",     "fixed_k3739"};

constexpr std::array<std::string_view, 7> kOtherConstants{
    "k3739_mode",         "abundance_sensitivity", "allow_negative_ca_correction",
    "use_irradiation_endtime", "cosmogenic",       "include_decay_error",
    "age_units"};

bool contains(std::span<const std::string_view> list, std::string_view s) {
  for (auto x : list) {
    if (x == s) return true;
  }
  return false;
}

}  // namespace

TEST(GoldenReader, ParsesScalarsArraysObjectsAndNonFinite) {
  const std::string text = R"({
    "b": {"c": [1, -2.5e-3, 0.0, 1E+2, -0], "d": null},
    "t": true, "f": false,
    "nan": "nan", "inf": "inf", "ninf": "-inf",
    "s": "q\"b\\s\nl\u00b5 µ \/ \t",
    "a": [],
    "o": {}
  })";
  std::string error;
  const golden::Json j = golden::parse(text, &error);
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_TRUE(j.is_object());
  EXPECT_EQ(j.size(), 9U);

  const golden::Json& c = j["b"]["c"];
  ASSERT_TRUE(c.is_array());
  ASSERT_EQ(c.size(), 5U);
  EXPECT_EQ(c[0].as_number(), 1.0);
  EXPECT_EQ(c[1].as_number(), -2.5e-3);
  EXPECT_EQ(c[2].as_number(), 0.0);
  EXPECT_EQ(c[3].as_number(), 100.0);
  EXPECT_TRUE(std::signbit(c[4].as_number()));
  EXPECT_TRUE(j["b"]["d"].is_null());
  EXPECT_TRUE(j["b"].contains("d"));
  EXPECT_FALSE(j["b"].contains("missing"));
  EXPECT_TRUE(j["b"]["missing"].is_null());

  EXPECT_TRUE(j["t"].as_bool());
  EXPECT_FALSE(j["f"].as_bool());
  EXPECT_TRUE(std::isnan(j["nan"].as_number()));
  EXPECT_EQ(j["inf"].as_number(), HUGE_VAL);
  EXPECT_EQ(j["ninf"].as_number(), -HUGE_VAL);
  EXPECT_EQ(j["s"].as_string(), "q\"b\\s\nl\xC2\xB5 \xC2\xB5 / \t");
  EXPECT_TRUE(j["a"].is_array());
  EXPECT_EQ(j["a"].size(), 0U);
  EXPECT_TRUE(j["o"].is_object());

  // Shortest round-trip doubles survive exactly (generator writes repr()).
  const golden::Json r = golden::parse("[0.1, 4.962e-10, 1e-300, 1.7976931348623157e308]",
                                       &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(r[0].as_number(), 0.1);
  EXPECT_EQ(r[1].as_number(), 4.962e-10);
  EXPECT_EQ(r[2].as_number(), 1e-300);
  EXPECT_EQ(r[3].as_number(), 1.7976931348623157e308);

  // Surrogate pair -> 4-byte UTF-8.
  const golden::Json u = golden::parse(R"("\ud83d\ude00")", &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(u.as_string(), "\xF0\x9F\x98\x80");
}

TEST(GoldenReader, RejectsMalformed) {
  const std::string bad_cases[] = {
      "[1, 2,]",            // trailing comma in array
      "{\"a\": 1,}",        // trailing comma in object
      "\"unterminated",     // unterminated string
      "[NaN]",              // bare NaN
      "{\"a\": Infinity}",  // bare infinity
      "[1] 2",              // trailing garbage
      "{\"a\" 1}",          // missing colon
      "[01]",               // leading zero
      "[1.]",               // no fraction digits
      "[\"a\tb\"]",         // raw control character in string
      R"(["\x"])",          // bad escape
      "",                   // empty
  };
  for (const std::string& bad : bad_cases) {
    std::string error;
    (void)golden::parse(bad, &error);
    EXPECT_FALSE(error.empty()) << "accepted: " << bad;
  }
}

TEST(GoldenReader, EveryGoldenFileLoadsWithHeader) {
  for (std::string_view file : kGoldenFiles) {
    SCOPED_TRACE(std::string(file));
    const golden::Json doc = golden::load(file);
    ASSERT_TRUE(doc.is_object());
    EXPECT_EQ(doc["schema"].as_number(), 1.0);
    const std::string& commit = doc["legacy_commit"].as_string();
    EXPECT_EQ(commit.size(), 40U);
    for (char ch : commit) {
      EXPECT_TRUE((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')) << commit;
    }
    EXPECT_TRUE(doc["legacy_dirty"].is_bool());
    EXPECT_TRUE(doc["packages"]["uncertainties"].is_string());

    const golden::Json& cases = doc["cases"];
    ASSERT_TRUE(cases.is_array());
    ASSERT_GT(cases.size(), 0U);
    std::set<std::string> names;
    for (const golden::Json& c : cases.as_array()) {
      const std::string& name = c["name"].as_string();
      EXPECT_FALSE(name.empty());
      EXPECT_TRUE(names.insert(name).second) << "duplicate case " << name;
      for (std::string_view key : {"source", "inputs", "expected", "tol", "legacy_sentinel",
                                   "expect_diagnostics", "expect_error"}) {
        EXPECT_TRUE(c.contains(key)) << name << " lacks " << key;
      }
      const golden::Tol t = golden::tol_of(c);
      EXPECT_GT(t.rtol, 0.0) << name;
      EXPECT_GT(t.rtol_err, 0.0) << name;
      EXPECT_GE(t.atol, 0.0) << name;
      if (t.atol > 0.0 || t.atol_err > 0.0) {
        EXPECT_FALSE(c["tol"]["why"].as_string().empty()) << name << ": atol without why";
      }
    }
  }
}

TEST(GoldenReader, ConstantsAlwaysExplicitAndBothLegacySetsPresent) {
  for (std::string_view file : kGoldenFiles) {
    if (file == "ufloat.json" || file == "decay_factors.json") continue;
    SCOPED_TRACE(std::string(file));
    const golden::Json doc = golden::load(file);
    const bool preset_sensitive = contains(kPresetSensitive, file);
    std::map<std::string, std::set<std::string>> variants;  // base name -> presets
    for (const golden::Json& c : doc["cases"].as_array()) {
      const std::string& name = c["name"].as_string();
      const golden::Json& k = c["inputs"]["constants"];
      ASSERT_TRUE(k.is_object()) << name;
      EXPECT_EQ(k.size(), kMeasuredConstants.size() + kOtherConstants.size()) << name;
      for (std::string_view m : kMeasuredConstants) {
        EXPECT_TRUE(k[m]["v"].is_number()) << name << " " << m;
        EXPECT_TRUE(k[m]["e"].is_number()) << name << " " << m;
      }
      for (std::string_view o : kOtherConstants) {
        EXPECT_TRUE(k.contains(o)) << name << " " << o;
      }
      EXPECT_TRUE(k["k3739_mode"].is_string()) << name;
      EXPECT_TRUE(k["abundance_sensitivity"].is_number()) << name;
      EXPECT_TRUE(k["allow_negative_ca_correction"].is_bool()) << name;
      EXPECT_TRUE(k["cosmogenic"].is_null() || k["cosmogenic"].is_object()) << name;
      EXPECT_TRUE(k["age_units"].is_string()) << name;

      if (!preset_sensitive) continue;
      const auto at = name.rfind('@');
      ASSERT_NE(at, std::string::npos) << name << " has no preset suffix";
      const std::string preset = name.substr(at + 1);
      EXPECT_TRUE(preset == "legacy" || preset == "legacy_preferences") << name;
      variants[name.substr(0, at)].insert(preset);
      // The legacy_preferences set (spec 5.3) is visible in the constants.
      if (preset == "legacy_preferences") {
        EXPECT_FALSE(k["allow_negative_ca_correction"].as_bool()) << name;
        EXPECT_EQ(k["lambda_b"]["e"].as_number(), 0.0) << name;
        EXPECT_EQ(k["fixed_k3739"]["e"].as_number(), 0.01) << name;
        // Pane defaults (arar_constants_preferences.py:144-149) zero these errors.
        EXPECT_EQ(k["atm4036"]["v"].as_number(), 295.5) << name;
        EXPECT_EQ(k["atm4036"]["e"].as_number(), 0.0) << name;
        EXPECT_EQ(k["lambda_e"]["v"].as_number(), 5.81e-11) << name;
        EXPECT_EQ(k["lambda_e"]["e"].as_number(), 0.0) << name;
      } else {
        EXPECT_TRUE(k["allow_negative_ca_correction"].as_bool()) << name;
        EXPECT_EQ(k["lambda_b"]["e"].as_number(), 9.3e-13) << name;
        EXPECT_EQ(k["fixed_k3739"]["e"].as_number(), 0.0001) << name;
        EXPECT_EQ(k["atm4036"]["e"].as_number(), 0.5) << name;
        EXPECT_EQ(k["lambda_e"]["e"].as_number(), 1.6e-13) << name;
      }
    }
    if (preset_sensitive) {
      EXPECT_FALSE(variants.empty());
      for (const auto& [base, presets] : variants) {
        EXPECT_EQ(presets.size(), 2U) << base << " lacks a legacy preset variant";
      }
    }
  }
}

TEST(GoldenReader, ExpectCloseHonoursTolerances) {
  golden::expect_close(1.0 + 1e-13, 1.0, 1e-12, 0.0, "within rtol");
  golden::expect_close(1e-13, 0.0, 0.0, 1e-12, "within atol");
  golden::expect_close(std::nan(""), std::nan(""), 1e-12, 0.0, "nan matches nan");
  golden::expect_close(HUGE_VAL, HUGE_VAL, 1e-12, 0.0, "inf matches inf");
  EXPECT_NONFATAL_FAILURE(golden::expect_close(1.0 + 1e-11, 1.0, 1e-12, 0.0, "outside"),
                          "outside");
  EXPECT_NONFATAL_FAILURE(golden::expect_close(1.0, std::nan(""), 1e-12, 0.0, "want nan"),
                          "want nan");
  EXPECT_NONFATAL_FAILURE(golden::expect_close(-HUGE_VAL, HUGE_VAL, 1e-12, 0.0, "sign"),
                          "sign");
}
