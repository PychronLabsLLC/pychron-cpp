#include <algorithm>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "pychron/earthbank/model.hpp"
#include "pychron/earthbank/vocab.hpp"
#include "pychron/reduction/arar_types.hpp"
#include "pychron/reduction/stats.hpp"
#include "records_support.hpp"

namespace pychron::earthbank {
namespace {

using reduction::ConstantsPreset;
using reduction::ReductionConstants;
using testing::all_fields;

bool ends_with(std::string_view text, std::string_view tail) {
  return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
}

TEST(Vocab, ArMethod) {
  EXPECT_EQ(ar_method(false, std::nullopt), "Total fusion - laser");
  EXPECT_EQ(ar_method(false, 1200.0), "Total fusion - laser");
  EXPECT_EQ(ar_method(true, 50.0), "Step-heating - laser");
  EXPECT_EQ(ar_method(true, 0.5), "Step-heating - laser");
  EXPECT_EQ(ar_method(true, 300.0), "Step-heating - furnace");
  EXPECT_EQ(ar_method(true, 1400.0), "Step-heating - furnace");
  EXPECT_EQ(ar_method(true, 51.0), "Step-heating - undefined");
  EXPECT_EQ(ar_method(true, 299.0), "Step-heating - undefined");
  EXPECT_EQ(ar_method(true, std::nullopt), "Step-heating - undefined");
}

TEST(Vocab, UncertaintyType) {
  for (const char* one : {"SE", "SEM", "SD", "MSEM", "1 sigma", "1s", "1"}) {
    EXPECT_EQ(uncertainty_type_id(one), 1) << one;
  }
  for (const char* two : {"2 sigma", "2s", "2"}) {
    EXPECT_EQ(uncertainty_type_id(two), 2) << two;
  }
  EXPECT_EQ(uncertainty_type_id("3 sigma"), std::nullopt);
  EXPECT_EQ(uncertainty_type_id(""), std::nullopt);
  EXPECT_EQ(uncertainty_type_id("sigma"), std::nullopt);
  EXPECT_EQ(uncertainty_type_id("12"), std::nullopt);
  // Case and punctuation are ignored.
  EXPECT_EQ(uncertainty_type_id("se"), 1);
  EXPECT_EQ(uncertainty_type_id("Sem"), 1);
  EXPECT_EQ(uncertainty_type_id("1-Sigma"), 1);
  EXPECT_EQ(uncertainty_type_id(" 1  SIGMA "), 1);
  EXPECT_EQ(uncertainty_type_id("2_sigma."), 2);
  EXPECT_EQ(uncertainty_type_id("(2S)"), 2);
}

TEST(Vocab, DecayConstantAndAirRatioByValue) {
  EXPECT_EQ(decay_constant_name(5.543e-10), "Steiger and Jager, 1977");
  EXPECT_EQ(decay_constant_name(5.463e-10), "Min et al. 2000");
  // Renne et al. 2011 give 5.5305e-10; rounded to four figures it is the same entry.
  EXPECT_EQ(decay_constant_name(5.5305e-10), "Renne et al. 2011");
  EXPECT_EQ(decay_constant_name(5.531e-10), "Renne et al. 2011");
  EXPECT_EQ(air_ratio_name(298.56), "Lee et al. 2006");
  EXPECT_EQ(air_ratio_name(295.5), "Nier 1950");

  // Half the tolerance away, either side, is still the value.
  for (const double value : {5.543e-10, 5.463e-10, 5.5305e-10}) {
    EXPECT_FALSE(decay_constant_name(value * (1.0 + 5e-5)).empty()) << value;
    EXPECT_FALSE(decay_constant_name(value * (1.0 - 5e-5)).empty()) << value;
    EXPECT_EQ(decay_constant_name(value * (1.0 + 2e-4)), "") << value;
    EXPECT_EQ(decay_constant_name(value * (1.0 - 2e-4)), "") << value;
  }
  for (const double value : {298.56, 295.5}) {
    EXPECT_FALSE(air_ratio_name(value * (1.0 + 5e-5)).empty()) << value;
    EXPECT_FALSE(air_ratio_name(value * (1.0 - 5e-5)).empty()) << value;
    EXPECT_EQ(air_ratio_name(value * (1.0 + 2e-4)), "") << value;
    EXPECT_EQ(air_ratio_name(value * (1.0 - 2e-4)), "") << value;
  }

  EXPECT_EQ(decay_constant_name(0.0), "");
  EXPECT_EQ(decay_constant_name(-5.543e-10), "");
  EXPECT_EQ(decay_constant_name(std::numeric_limits<double>::quiet_NaN()), "");
  EXPECT_EQ(air_ratio_name(0.0), "");
  EXPECT_EQ(air_ratio_name(std::numeric_limits<double>::quiet_NaN()), "");
}

// What the reduction's presets are called on EarthBank. Each carries
// lambda_b = 4.962e-10 and lambda_e = 5.81e-11, so every one of them is
// Steiger and Jager; the air ratio is what tells them apart.
TEST(Vocab, PresetsMapToNames) {
  struct Expected {
    ConstantsPreset preset;
    double lambda_total, atm4036;
    std::string_view decay, air;
  };
  const std::vector<Expected> expected = {
      {ConstantsPreset::Default, 5.543e-10, 298.56, "Steiger and Jager, 1977", "Lee et al. 2006"},
      {ConstantsPreset::Legacy, 5.543e-10, 295.5, "Steiger and Jager, 1977", "Nier 1950"},
      {ConstantsPreset::LegacyPreferences, 5.543e-10, 295.5, "Steiger and Jager, 1977", "Nier 1950"},
  };
  for (const Expected& e : expected) {
    const ReductionConstants c = reduction::constants_preset(e.preset);
    const double lambda_total = c.lambda_b.value + c.lambda_e.value;
    const std::string_view preset = reduction::to_string(e.preset);
    EXPECT_NEAR(lambda_total, e.lambda_total, 1e-12 * e.lambda_total) << preset;
    EXPECT_EQ(lambda_total, reduction::lambda_k(c).value) << preset;
    EXPECT_EQ(c.atm4036.value, e.atm4036) << preset;
    EXPECT_EQ(decay_constant_name(lambda_total), e.decay) << preset;
    EXPECT_EQ(air_ratio_name(c.atm4036.value), e.air) << preset;
  }
}

TEST(Vocab, NormalizeCitation) {
  EXPECT_EQ(normalize_citation("Min (2008)", true), "Min et al. 2000");
  EXPECT_EQ(normalize_citation("Min et al. 2008", true), "Min et al. 2000");
  EXPECT_EQ(normalize_citation("Min et al. (2000)", true), "Min et al. 2000");
  EXPECT_EQ(normalize_citation("Steiger and Jager (1977)", true), "Steiger and Jager, 1977");
  EXPECT_EQ(normalize_citation("Steiger and Jager, 1977", true), "Steiger and Jager, 1977");
  EXPECT_EQ(normalize_citation("Renne et al. (2011)", true), "Renne et al. 2011");
  EXPECT_EQ(normalize_citation("renne 2010", true), "Renne et al. 2011");
  EXPECT_EQ(normalize_citation("Nier (1950)", false), "Nier 1950");
  EXPECT_EQ(normalize_citation("Lee (2006)", false), "Lee et al. 2006");
  EXPECT_EQ(normalize_citation("Lee et al. 2006", false), "Lee et al. 2006");

  // Unknown strings pass through as they came.
  EXPECT_EQ(normalize_citation("Carter et al. 2025", true), "Carter et al. 2025");
  EXPECT_EQ(normalize_citation("Min (1999)", true), "Min (1999)");
  EXPECT_EQ(normalize_citation("Min", true), "Min");
  EXPECT_EQ(normalize_citation("  some lab's own value ", true), "  some lab's own value ");
  EXPECT_EQ(normalize_citation("", true), "");
  EXPECT_EQ(normalize_citation("(2008)", true), "(2008)");
  // Each table answers only for its own kind.
  EXPECT_EQ(normalize_citation("Nier (1950)", true), "Nier (1950)");
  EXPECT_EQ(normalize_citation("Min (2008)", false), "Min (2008)");
  // The author is the first word outside parentheses; the year may be inside them.
  EXPECT_EQ(normalize_citation("(after) Renne 2011", true), "Renne et al. 2011");
  EXPECT_EQ(normalize_citation("Min (Min et al., 2008", true), "Min et al. 2000");
}

TEST(Vocab, FluxMonitorQuery) {
  EXPECT_EQ(flux_monitor_query("FC-2", 28.201), "FC-2 28.201 Ma");
  EXPECT_EQ(flux_monitor_query("Fish Canyon Kuiper 2008", 28.0), "Fish Canyon Kuiper 2008 28 Ma");
  EXPECT_EQ(flux_monitor_query("FC-2", std::nullopt), "FC-2");
  EXPECT_EQ(flux_monitor_query("", 1.1851), "1.1851 Ma");
  EXPECT_EQ(flux_monitor_query("", std::nullopt), "");
}

// From the `Age Type` list of the vendor's Ar-Ar template.
TEST(Vocab, AgeTypeNames) {
  EXPECT_EQ(age_type_name(AgeKind::Plateau), "Plateau Age");
  EXPECT_EQ(age_type_name(AgeKind::WeightedMean), "Weighted Mean Age");
  EXPECT_EQ(age_type_name(AgeKind::Isochron), "Inverse Isochron Age");
  EXPECT_EQ(age_type_name(AgeKind::Integrated), "Total Gas Age");
}

// The words pychron-cpp has for the error of a mean each have an id: they
// are what the mapping writes as an uncertainty type.
TEST(Vocab, MeanErrorKindsAreUncertaintyTypes) {
  for (const reduction::MeanErrorKind kind :
       {reduction::MeanErrorKind::Sd, reduction::MeanErrorKind::Sem, reduction::MeanErrorKind::Msem}) {
    EXPECT_EQ(uncertainty_type_id(reduction::to_string(kind)), 1) << reduction::to_string(kind);
  }
  EXPECT_EQ(reduction::to_string(reduction::MeanErrorKind::Sd), "sd");
  EXPECT_EQ(reduction::to_string(reduction::MeanErrorKind::Sem), "sem");
  EXPECT_EQ(reduction::to_string(reduction::MeanErrorKind::Msem), "msem");
}

// From the template's lists: `Uncertainty Unit`, `Uncertainty Type`, `Yes/No`,
// `Data Interpretation` and `Age Interpretation`.
TEST(Vocab, FixedTerms) {
  EXPECT_EQ(kIntensityUnits, "fA");
  EXPECT_EQ(kAbsoluteUncertainty, "abs.");
  EXPECT_EQ(kOneSigma, "1 sigma");
  EXPECT_EQ(uncertainty_type_id(kOneSigma), 1);
  EXPECT_EQ(kYes, "Yes");
  EXPECT_EQ(kInterpretationTool, "Pychron");
  EXPECT_EQ(kUnknownInterpretation, "Unknown");
}

TEST(Vocab, LookupSpecsCoverEveryNameField) {
  // Names that are not terms of any vocabulary the service is asked about: an
  // aliquot's own name, the name given to the sample's location, and the
  // lithology, which the legacy integration sent by name and never looked up.
  const std::set<std::string> free_text = {"aliquotName", "locationName", "lithologyName"};

  std::set<std::string> name_fields, all_keys;
  for (const nlohmann::json& record :
       {all_fields<Sample>(), all_fields<Location>(), all_fields<DataPoint>(), all_fields<Aliquot>(),
        all_fields<Measurement>(), all_fields<AgeCalc>(), all_fields<AgeSummary>()}) {
    for (const auto& item : record.items()) {
      all_keys.insert(item.key());
      if (ends_with(item.key(), "Name") && !free_text.contains(item.key())) name_fields.insert(item.key());
    }
  }

  std::set<std::string> looked_up, fixed;
  for (const LookupSpec& spec : lookup_specs()) {
    EXPECT_TRUE(looked_up.insert(std::string(spec.name_field)).second) << spec.name_field;
  }
  for (const LookupSpec& spec : uncertainty_type_specs()) {
    EXPECT_TRUE(fixed.insert(std::string(spec.name_field)).second) << spec.name_field;
    EXPECT_TRUE(spec.endpoint.empty()) << spec.name_field;
    EXPECT_FALSE(looked_up.contains(std::string(spec.name_field))) << spec.name_field;
  }
  EXPECT_EQ(lookup_specs().size(), 24u);  // the legacy table, entry for entry
  EXPECT_EQ(uncertainty_type_specs().size(), 6u);

  // Every name field is in exactly one table, and the tables name nothing else.
  std::set<std::string> covered = looked_up;
  covered.insert(fixed.begin(), fixed.end());
  EXPECT_EQ(covered, name_fields);

  for (const std::span<const LookupSpec> specs : {lookup_specs(), uncertainty_type_specs()}) {
    for (const LookupSpec& spec : specs) {
      // The id field is the name field with Id for Name, and is a field of a record.
      const std::string stem(spec.name_field.substr(0, spec.name_field.size() - 4));
      EXPECT_EQ(spec.id_field, stem + "Id");
      EXPECT_TRUE(all_keys.contains(std::string(spec.id_field))) << spec.id_field;
      EXPECT_TRUE(spec.endpoint.empty() || spec.endpoint.starts_with("/api/")) << spec.name_field;
    }
  }
}

TEST(Vocab, LookupEndpoints) {
  const auto endpoint_of = [](std::string_view name_field) -> std::string {
    const auto specs = lookup_specs();
    const auto it = std::find_if(specs.begin(), specs.end(),
                                 [&](const LookupSpec& spec) { return spec.name_field == name_field; });
    return it == specs.end() ? "<none>" : std::string(it->endpoint);
  };
  EXPECT_EQ(endpoint_of("arMethodName"), "/api/arar/LArArMethod");
  EXPECT_EQ(endpoint_of("ageCalcTypeName"), "/api/arar/LAgeType");
  EXPECT_EQ(endpoint_of("decayConstantName"), "/api/arar/DecayConstant");
  EXPECT_EQ(endpoint_of("airRatioName"), "/api/arar/AirRatio");
  EXPECT_EQ(endpoint_of("fluxMonitorName"), "/api/arar/FluxMonitor");
  EXPECT_EQ(endpoint_of("mineralName"), "/api/core/materials");
  EXPECT_EQ(endpoint_of("materialName"), "/api/core/materials");
  EXPECT_EQ(endpoint_of("jvalueUncertaintyUnitName"), "/api/arar/LUncertaintyUnit");
  EXPECT_EQ(endpoint_of("calculatedAgeUncertaintyTypeUnitsName"), "/api/arar/LUncertaintyUnit");
  EXPECT_EQ(endpoint_of("stratographicUnitName"), "/api/core/stratigraphic-units");
  // No list to ask: sent by name.
  EXPECT_EQ(endpoint_of("plateauStepName"), "");
  EXPECT_EQ(endpoint_of("inverseIsoStepName"), "");
  EXPECT_EQ(endpoint_of("weightedMeanStepName"), "");
  // Not a term that is looked up at all.
  EXPECT_EQ(endpoint_of("lithologyName"), "<none>");
  EXPECT_EQ(endpoint_of("aliquotName"), "<none>");
}

}  // namespace
}  // namespace pychron::earthbank
