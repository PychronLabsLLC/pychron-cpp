#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "ooxml.hpp"
#include "pychron/core/error.hpp"
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
  EXPECT_EQ(kNo, "No");
  EXPECT_EQ(kMilligrams, "mg");
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

// ---- A value against one of the service's lists ------------------------------

using Kind = ListMatch::Kind;

// The flux monitors as the Ar-Ar template lists them (sheet `Lookup Tables`,
// C2:C17, the range its ArArAgeCalc sheet validates `fluxMonitor` against).
std::vector<std::string> template_monitors() {
  auto book = ooxml::Workbook::open(std::filesystem::path(PYCHRON_EARTHBANK_TEMPLATE_DIR) /
                                    "ArArDataPoint_Template2026.xlsx");
  if (!book) throw std::runtime_error(to_string(book.error()));
  auto values = book->column_values("Lookup Tables", 3, 2, 17);
  if (!values) throw std::runtime_error(to_string(values.error()));
  return *values;
}

void expect_match(const ListMatch& match, Kind kind, std::string_view entry, const std::string& what) {
  EXPECT_EQ(static_cast<int>(match.kind), static_cast<int>(kind)) << what;
  EXPECT_EQ(match.entry, entry) << what;
}

TEST(Vocab, AValueIsFoundInAListWhateverItsCaseAndSpaces) {
  const std::vector<std::string> list = {"Unknown", "Biotite", "Renne et al. 2011 ", "Total fusion - laser",
                                         "Total Fusion - undefined", "vial", "Vial", "", "  Padded  entry "};
  const auto match = [&](std::string_view value) { return match_list_entry(value, list); };

  // As the list has it: written as it is.
  expect_match(match("Biotite"), Kind::Exact, "Biotite", "exact");
  expect_match(match("Renne et al. 2011 "), Kind::Exact, "Renne et al. 2011 ", "exact, the list's space and all");
  // The list's entry less the spaces around it is no other spelling.
  expect_match(match("Renne et al. 2011"), Kind::Exact, "Renne et al. 2011", "the list has a space after it");
  // Case and the spaces around: the list's spelling, trimmed.
  expect_match(match("biotite"), Kind::Respelled, "Biotite", "case");
  expect_match(match("BIOTITE"), Kind::Respelled, "Biotite", "case");
  expect_match(match("  Biotite\t"), Kind::Respelled, "Biotite", "spaces");
  expect_match(match("renne ET AL. 2011"), Kind::Respelled, "Renne et al. 2011", "case, trimmed");
  expect_match(match("total fusion - LASER"), Kind::Respelled, "Total fusion - laser", "case");
  expect_match(match("padded  entry"), Kind::Respelled, "Padded  entry", "the spaces inside are the entry's");
  // Nothing else is the same entry: not a part, not other spaces inside.
  expect_match(match("Biotit"), Kind::None, "", "a part");
  expect_match(match("Biotite mica"), Kind::None, "", "more");
  expect_match(match("Total fusion-laser"), Kind::None, "", "spaces inside");
  expect_match(match("Total fusion - undefined"), Kind::Respelled, "Total Fusion - undefined", "case inside");
  expect_match(match("Bi\xC3\xB6tite"), Kind::None, "", "another letter");
  expect_match(match(""), Kind::None, "", "nothing");
  expect_match(match("   "), Kind::None, "", "spaces");
  // Two entries that differ only in case: the one written exactly is meant;
  // another case could be either, and is rewritten to neither.
  expect_match(match("vial"), Kind::Exact, "vial", "one of two");
  expect_match(match("Vial"), Kind::Exact, "Vial", "the other");
  expect_match(match(" Vial "), Kind::Respelled, "Vial", "the other, trimmed");
  expect_match(match("VIAL"), Kind::Ambiguous, "", "either");

  expect_match(match_list_entry("Biotite", {}), Kind::None, "", "an empty list");
  // An entry twice in a list is one entry.
  const std::vector<std::string> twice = {"Other", "Other ", "other (add comment)"};
  expect_match(match_list_entry("other", twice), Kind::Respelled, "Other", "listed twice");
}

TEST(Vocab, AFluxMonitorIsFoundByItsNameAndItsAge) {
  const std::vector<std::string> list = template_monitors();
  ASSERT_EQ(list.size(), 16u);
  const auto match = [&](std::string_view value) { return match_flux_monitor(value, list); };
  const char* const kuiper = "Fish Canyon Tuff Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)";
  const char* const renne = "Fish Canyon Tuff Sanidine (FCTs) from Renne et al. 2011 (28.294 Ma)";

  // What the mapping writes: the monitor as pychron names it and its age.
  expect_match(match("FC-2 28.201 Ma"), Kind::Respelled, kuiper, "FC-2");
  expect_match(match(flux_monitor_query("FC-2", 28.201)), Kind::Respelled, kuiper, "the mapping's query");
  expect_match(match("FC-2 28.294 Ma"), Kind::Respelled, renne, "the age decides");
  expect_match(match("FCs 28.201"), Kind::Respelled, kuiper, "FCs, no unit");
  expect_match(match("fct 28.2010 Ma"), Kind::Respelled, kuiper, "FCT, a zero after the age");
  expect_match(match("FCT-2 28.201Ma"), Kind::Respelled, kuiper, "a split's number, the unit run on");
  expect_match(match("Fish Canyon sanidine (28.201 Ma)"), Kind::Respelled, kuiper, "by name");
  expect_match(match("Fish Canyon Tuff sanidine, Kuiper et al. 2008, 28.201 Ma"), Kind::Respelled, kuiper, "a citation");
  expect_match(match("FC-2 28.175 Ma"), Kind::Respelled,
               "Fish Canyon Tuff Sanidine (FCTs) from Phillips et al. 2022 (28.175 Ma)", "Phillips");
  expect_match(match("ACs 1.1851 Ma"), Kind::Respelled,
               "Alder Creek Rhyolite Sanidine (ACs) from Schaen et al. 2020 (1.1851 Ma)", "ACs");
  expect_match(match("Alder Creek 1.1834"), Kind::Respelled,
               "Alder Creek Rhyolite Sanidine (ACs) from Phillips et al. 2022 (1.1834 Ma)", "Alder Creek");
  expect_match(match("GA-1550 99.738 Ma"), Kind::Respelled,
               "Mt Domedary Biotite (GA-1550) using Renne et al. 2011 (99.738 Ma)", "GA-1550");
  expect_match(match("GA1550 99.44 Ma"), Kind::Respelled,
               "Mt Domedary Biotite (GA-1550) from Jourdan and Renne 2007 (99.44 Ma)", "GA1550, not MD-2 of that age");
  expect_match(match("MD-2 99.44 Ma"), Kind::Respelled,
               "Mt Domedary Biotite (MD-2) from Jourdan and Renne 2007 (99.44 Ma)", "MD-2, not GA-1550 of that age");
  expect_match(match("Hb3gr 1081 Ma"), Kind::Respelled, "Hornblende (Hb3gr) using Renne et al. 2011 (1081 Ma)", "Hb3gr");
  expect_match(match("HB3GR 1081.5"), Kind::Respelled, "Hornblende (Hb3gr) from Jourdan and Renne 2007 (1081.5 Ma)",
               "Hb3gr, the other age");
  expect_match(match("TCs 28.447 Ma"), Kind::Respelled,
               "Taylor Creek Rhyolite Saniditine (TCs) from Fleck et al. 2019 (28.447 Ma)", "TCs");
  expect_match(match("Taylor Creek Rhyolite sanidine 28.447 Ma"), Kind::Respelled,
               "Taylor Creek Rhyolite Saniditine (TCs) from Fleck et al. 2019 (28.447 Ma)", "Taylor Creek");
  expect_match(match("WA1ms 2613 Ma"), Kind::Respelled,
               "WA1ms (Western Australia Muscovite) from Jourdan et al. (2014) (2613 Ma)", "WA1ms");
  // Without one of pychron's names for it: every word of the name must be the
  // entry's, and one of them a word of three letters or more.
  expect_match(match("Kuiper 2008 28.201 Ma"), Kind::Respelled, kuiper, "the citation alone");
  expect_match(match("Western Australia muscovite 2613 Ma"), Kind::Respelled,
               "WA1ms (Western Australia Muscovite) from Jourdan et al. (2014) (2613 Ma)", "by words");

  // An entry as the list has it needs no age to be found.
  expect_match(match(kuiper), Kind::Exact, kuiper, "the entry itself");
  expect_match(match(std::string(" ") + renne), Kind::Respelled, renne, "the entry, a space before it");

  // No match, and so nothing rewritten:
  expect_match(match("FC-2 28.02 Ma"), Kind::None, "", "an age the list has not for it");
  expect_match(match("FC-2 28.2 Ma"), Kind::None, "", "an age cut short is another age");
  expect_match(match("FC-2 28.20 Ma"), Kind::None, "", "likewise");
  expect_match(match("FC-2 28.2014 Ma"), Kind::None, "", "and one with more digits");
  expect_match(match("FC-2"), Kind::None, "", "no age");
  expect_match(match("28.201 Ma"), Kind::None, "", "no name");
  expect_match(match("28.201"), Kind::None, "", "a number");
  expect_match(match(""), Kind::None, "", "nothing");
  expect_match(match("ACs 28.201 Ma"), Kind::None, "", "another monitor's age");
  expect_match(match("FC-2 1.1851 Ma"), Kind::None, "", "likewise");
  expect_match(match("BT-1 sanidine 28.201 Ma"), Kind::None, "", "a monitor the list has not, of a listed age");
  expect_match(match("sanidine 28.201 Ma"), Kind::Respelled, kuiper, "a mineral and an age only one entry has");
  expect_match(match("FC-2 or ACs 28.201 Ma"), Kind::None, "", "two monitors named");
  expect_match(match("Bishop Tuff sanidine 0.7666 Ma"), Kind::None, "", "not listed at all");
  expect_match(match("GA-1550 MD-2 99.44 Ma"), Kind::None, "", "two monitors of one age");
  // A mineral named that is not the entry's: the monitor is another one,
  // whatever else of the name fits. FCT-3 is the biotite of the Fish Canyon
  // Tuff, and the list has only its sanidine.
  expect_match(match("FCT-3 biotite 28.201 Ma"), Kind::None, "", "a biotite, by a name of the sanidine's");
  expect_match(match("Fish Canyon biotite 28.201 Ma"), Kind::None, "", "likewise, by the tuff's name");
  expect_match(match("FC-2 Biotite 28.201 Ma"), Kind::None, "", "whatever the case");
  expect_match(match("FCT-3 28.201 Ma"), Kind::None, "", "the biotite's designation, no mineral said");
  expect_match(match("FCT3 28.201 Ma"), Kind::None, "", "likewise, run together");
  expect_match(match("biotite 28.201 Ma"), Kind::None, "", "a mineral no entry of that age has");
  expect_match(match("sanidine 99.44 Ma"), Kind::None, "", "likewise: the entries of that age are biotites");
  expect_match(match("GA-1550 sanidine 99.738 Ma"), Kind::None, "", "a sanidine, by a biotite's name");
  expect_match(match("Hb3gr biotite 1081 Ma"), Kind::None, "", "a biotite, by a hornblende's name");
  expect_match(match("WA1ms sanidine 2613 Ma"), Kind::None, "", "a sanidine, by a muscovite's name");
  expect_match(match("FC-2 sanidine and biotite 28.201 Ma"), Kind::None, "", "two minerals");
  // A material the list has no monitor of at all, and the short forms a lab
  // writes: the same.
  expect_match(match("FC-2 plagioclase 28.201 Ma"), Kind::None, "", "a plagioclase");
  expect_match(match("Fish Canyon K-feldspar 28.201 Ma"), Kind::None, "", "a feldspar");
  expect_match(match("Fish Canyon kspar 28.201 Ma"), Kind::None, "", "likewise");
  expect_match(match("Fish Canyon K-spar 28.201 Ma"), Kind::None, "", "likewise, as two words");
  expect_match(match("FCT bt 28.201 Ma"), Kind::None, "", "a biotite, in short");
  expect_match(match("FC-2 bio 28.201 Ma"), Kind::None, "", "likewise");
  expect_match(match("Fish Canyon biotites 28.201 Ma"), Kind::None, "", "biotites are biotite");
  expect_match(match("Fish Canyon plagioclases 28.201 Ma"), Kind::None, "", "and a plural of the others");
  expect_match(match("FC-2 whole rock 28.201 Ma"), Kind::None, "", "two words");
  expect_match(match("FC-2 Whole-Rock 28.201 Ma"), Kind::None, "", "two words, hyphened");
  expect_match(match("FC-2 wholerock 28.201 Ma"), Kind::None, "", "or one");
  expect_match(match("FC-2 groundmass 28.201 Ma"), Kind::None, "", "groundmass");
  expect_match(match("GA-1550 hbl 99.44 Ma"), Kind::None, "", "a hornblende, by a biotite's name");
  expect_match(match("Hb3gr ms 1081 Ma"), Kind::None, "", "a muscovite, by a hornblende's name");
  expect_match(match("FC-2 san plag 28.201 Ma"), Kind::None, "", "two, one of them the entry's");
  expect_match(match("FC-2 rock 28.201 Ma"), Kind::Respelled, kuiper, "`rock` alone is no material");
  // A short form or a plural of the entry's own mineral is that mineral.
  expect_match(match("FC-2 san 28.201 Ma"), Kind::Respelled, kuiper, "san is sanidine");
  expect_match(match("FCs sanidines 28.201 Ma"), Kind::Respelled, kuiper, "sanidines are sanidine");
  expect_match(match("GA-1550 bt 99.738 Ma"), Kind::Respelled,
               "Mt Domedary Biotite (GA-1550) using Renne et al. 2011 (99.738 Ma)", "bt is biotite");
  expect_match(match("Hb3gr hbl 1081 Ma"), Kind::Respelled, "Hornblende (Hb3gr) using Renne et al. 2011 (1081 Ma)",
               "hbl is hornblende");
  expect_match(match("WA1ms musc 2613 Ma"), Kind::Respelled,
               "WA1ms (Western Australia Muscovite) from Jourdan et al. (2014) (2613 Ma)", "musc is muscovite");
  // The same mineral is no obstacle, in the list's misspelling either.
  expect_match(match("FC-2 sanidine 28.201 Ma"), Kind::Respelled, kuiper, "the entry's mineral");
  expect_match(match("TCs sanidine 28.447 Ma"), Kind::Respelled,
               "Taylor Creek Rhyolite Saniditine (TCs) from Fleck et al. 2019 (28.447 Ma)", "Saniditine is sanidine");
  expect_match(match("GA-1550 biotite 99.44 Ma"), Kind::Respelled,
               "Mt Domedary Biotite (GA-1550) from Jourdan and Renne 2007 (99.44 Ma)", "the entry's mineral");
  // Ambiguous, and so not rewritten: the age is two entries' and the name
  // says no more.
  expect_match(match("Mt Domedary Biotite 99.44 Ma"), Kind::Ambiguous, "", "GA-1550 or MD-2");
  expect_match(match("Jourdan and Renne 2007 99.44"), Kind::Ambiguous, "", "likewise");
  const std::vector<std::string> twins = {"Fish Canyon Tuff Sanidine (FCTs) from A (28.201 Ma)",
                                          "Fish Canyon Tuff Sanidine (FCTs) from B (28.201 Ma)"};
  expect_match(match_flux_monitor("FC-2 28.201 Ma", twins), Kind::Ambiguous, "", "two entries of one age");
  // The same entry twice is one entry.
  const std::vector<std::string> same = {std::string(kuiper), std::string(kuiper) + " "};
  expect_match(match_flux_monitor("FC-2 28.201 Ma", same), Kind::Respelled, kuiper, "listed twice");
  expect_match(match_flux_monitor("FC-2 28.201 Ma", {}), Kind::None, "", "an empty list");
}

// The minerals the rule knows are the list's: every entry names exactly one
// of them, and each of them is some entry's.
TEST(Vocab, TheMonitorMineralsAreOfTheTemplatesList) {
  const std::vector<std::string> list = template_monitors();
  const auto minerals = flux_monitor_minerals();
  ASSERT_FALSE(minerals.empty());
  const auto words_of = [](const std::string& text) {
    std::vector<std::string> out(1);
    for (const char c : text) {
      if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
        out.back() += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      } else if (!out.back().empty()) {
        out.emplace_back();
      }
    }
    return out;
  };
  std::set<std::string> used, named;
  for (const std::string& entry : list) {
    std::set<std::string> of_entry;
    for (const std::string& word : words_of(entry)) {
      for (const MonitorMineral& mineral : minerals) {
        if (word != mineral.word) continue;
        of_entry.insert(std::string(mineral.mineral));
        used.insert(std::string(mineral.word));
      }
    }
    EXPECT_EQ(of_entry.size(), 1u) << entry;
    named.insert(of_entry.begin(), of_entry.end());
  }
  EXPECT_EQ(used.size(), minerals.size()) << "a word of the table is in no entry";
  EXPECT_EQ(named, (std::set<std::string>{"biotite", "hornblende", "muscovite", "sanidine"}));
  // The list's misspelling is the mineral it means.
  EXPECT_TRUE(std::any_of(minerals.begin(), minerals.end(), [](const MonitorMineral& m) {
    return m.word == "saniditine" && m.mineral == "sanidine";
  }));
}

// The words a lab writes for a material that are not the list's own: none of
// them, nor its plural, is a word of an alias or of an entry, where it would
// be taken for a material and is none. (The list's own mineral words are held
// to the list by the test above.)
TEST(Vocab, TheOtherMaterialWordsAreNoWordOfAnAliasOrAnEntry) {
  const auto others = flux_monitor_other_materials();
  ASSERT_FALSE(others.empty());
  const auto words_of = [](std::string_view text) {
    std::vector<std::string> out(1);
    for (const char c : text) {
      if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
        out.back() += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      } else if (!out.back().empty()) {
        out.emplace_back();
      }
    }
    if (out.back().empty()) out.pop_back();
    return out;
  };
  std::set<std::string> taken;
  for (const MonitorAlias& alias : flux_monitor_aliases()) {
    for (const std::string& word : words_of(alias.alias)) taken.insert(word);
    for (const std::string& word : words_of(alias.listed)) taken.insert(word);
  }
  for (const std::string& entry : template_monitors()) {
    for (const std::string& word : words_of(entry)) taken.insert(word);
  }
  std::set<std::string> listed;
  for (const MonitorMineral& mineral : flux_monitor_minerals()) listed.insert(std::string(mineral.mineral));
  for (const MonitorMineral& other : others) {
    const std::vector<std::string> words = words_of(other.word);
    ASSERT_FALSE(words.empty());
    EXPECT_EQ(other.word.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ"), std::string_view::npos) << other.word;
    // A term of one word must not be taken; one of two is told by its run,
    // and neither `whole` nor `rock` is in an entry all the same.
    for (const std::string& word : words) {
      EXPECT_FALSE(taken.contains(word)) << other.word;
      EXPECT_FALSE(taken.contains(word + "s")) << other.word << ", in the plural";
    }
    // A short form of a mineral the list has is that mineral; any other
    // word is of a material the list has not.
    const bool of_the_list = listed.contains(std::string(other.mineral));
    const bool is_short = other.word == "san" || other.word == "bt" || other.word == "bio" || other.word == "hbl" ||
                          other.word == "ms" || other.word == "musc";
    EXPECT_EQ(of_the_list, is_short) << other.word << " -> " << other.mineral;
  }
  // Every word the lab was said to write.
  for (const char* word :
       {"plagioclase", "feldspar", "kfeldspar", "kspar", "spar", "orthoclase", "anorthoclase", "adularia", "microcline",
        "amphibole", "phlogopite", "sericite", "glauconite", "alunite", "jarosite", "glass", "groundmass",
        "whole rock", "wholerock", "san", "bt", "bio", "hbl", "ms", "musc", "plag", "kfs"}) {
    EXPECT_TRUE(std::any_of(others.begin(), others.end(), [&](const MonitorMineral& m) { return m.word == word; }))
        << word;
  }
}

// The table of pychron's names for monitors is of this list: each name it
// maps to marks at least one entry, and every entry is reached by some name.
TEST(Vocab, TheMonitorAliasesAreOfTheTemplatesList) {
  const std::vector<std::string> list = template_monitors();
  const auto aliases = flux_monitor_aliases();
  ASSERT_FALSE(aliases.empty());
  std::set<std::string> reached;
  for (const MonitorAlias& alias : aliases) {
    int marked = 0;
    for (const std::string& entry : list) {
      // The mark is in the entry as a word of its own: in parentheses, or
      // first.
      const std::string mark(alias.listed);
      if (entry.find("(" + mark + ")") == std::string::npos && entry.rfind(mark + " ", 0) != 0) continue;
      ++marked;
      // With the entry's own age the alias finds exactly that entry, unless
      // the monitor has two entries of that age.
      const std::size_t open = entry.rfind('(');
      const std::string age = entry.substr(open + 1, entry.rfind(" Ma)") - open - 1);
      const ListMatch found = match_flux_monitor(std::string(alias.alias) + " " + age + " Ma", list);
      EXPECT_EQ(static_cast<int>(found.kind), static_cast<int>(Kind::Respelled)) << alias.alias << " " << age;
      EXPECT_EQ(found.entry, entry) << alias.alias << " " << age;
      reached.insert(entry);
    }
    EXPECT_GT(marked, 0) << alias.alias << " -> " << alias.listed << " marks no entry of the list";
  }
  EXPECT_EQ(reached.size(), list.size());
  // No name of the table is a designation of another mineral's split.
  for (const MonitorAlias& alias : aliases) {
    EXPECT_NE(alias.alias, "FCT-3");
    EXPECT_NE(alias.alias, "FCT3");
  }
  for (const char* name : {"FC-2", "FCs", "FCT", "Fish Canyon", "ACs", "Alder Creek", "GA-1550", "Hb3gr", "TCs",
                           "Taylor Creek"}) {
    EXPECT_TRUE(std::any_of(aliases.begin(), aliases.end(), [&](const MonitorAlias& a) { return a.alias == name; }))
        << name;
  }
}

}  // namespace
}  // namespace pychron::earthbank
