#include "pychron/earthbank/vocab.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace pychron::earthbank {
namespace {

using namespace std::string_view_literals;

constexpr std::string_view kUncertaintyUnit = "/api/arar/LUncertaintyUnit";
constexpr std::string_view kMaterials = "/api/core/materials";

// Legacy LOOKUP_ENDPOINTS, entry for entry.
constexpr std::array kLookupSpecs = {
    LookupSpec{"analysisScaleName", "analysisScaleId", "/api/arar/LAnalysisScale"},
    LookupSpec{"analyticalUncertaintyUnitName", "analyticalUncertaintyUnitId", kUncertaintyUnit},
    LookupSpec{"apparentAgeUncertaintyUnitName", "apparentAgeUncertaintyUnitId", kUncertaintyUnit},
    LookupSpec{"jvalueUncertaintyUnitName", "jvalueUncertaintyUnitId", kUncertaintyUnit},
    LookupSpec{"arMethodName", "arMethodId", "/api/arar/LArArMethod"},
    LookupSpec{"ageCalcTypeName", "ageCalcTypeId", "/api/arar/LAgeType"},
    LookupSpec{"calculatedAgeUncertaintyTypeUnitsName", "calculatedAgeUncertaintyTypeUnitsId",
               kUncertaintyUnit},
    LookupSpec{"dataInterpretationToolName", "dataInterpretationToolId",
               "/api/arar/LArArDataInterpretationTool"},
    LookupSpec{"interpretationName", "interpretationId", "/api/arar/LArArInterpretation"},
    LookupSpec{"decayConstantName", "decayConstantId", "/api/arar/DecayConstant"},
    LookupSpec{"airRatioName", "airRatioId", "/api/arar/AirRatio"},
    LookupSpec{"fluxMonitorName", "fluxMonitorId", "/api/arar/FluxMonitor"},
    LookupSpec{"mineralName", "mineralId", kMaterials},
    // Yes, No or N/A, kept by the server as an enumeration.
    LookupSpec{"plateauStepName", "plateauStepId", ""},
    LookupSpec{"inverseIsoStepName", "inverseIsoStepId", ""},
    LookupSpec{"weightedMeanStepName", "weightedMeanStepId", ""},
    // The sample.
    LookupSpec{"sampleKindName", "sampleKindId", "/api/core/l-sample-kinds"},
    LookupSpec{"sampleMethodName", "sampleMethodId", "/api/core/l-sample-methods"},
    LookupSpec{"materialName", "materialId", kMaterials},
    LookupSpec{"locationKindName", "locationKindId", "/api/core/l-location-kinds"},
    LookupSpec{"archiveName", "archiveId", "/api/core/archives"},
    LookupSpec{"stratographicUnitName", "stratographicUnitId", "/api/core/stratigraphic-units"},
    // Not fields of the data point's record: the ids go into link tables.
    LookupSpec{"fundingName", "fundingId", "/api/core/fundings"},
    LookupSpec{"literatureName", "literatureId", "/api/core/literature"},
};

// Legacy STATIC_LOOKUPS.
constexpr std::array kUncertaintyTypeSpecs = {
    LookupSpec{"analyticalUncertaintyTypeName", "analyticalUncertaintyTypeId", ""},
    LookupSpec{"apparentAgeUncertaintyTypeName", "apparentAgeUncertaintyTypeId", ""},
    LookupSpec{"apparentAgeUncertainityTypeName", "apparentAgeUncertainityTypeId", ""},
    LookupSpec{"jvalueUncertaintyTypeName", "jvalueUncertaintyTypeId", ""},
    LookupSpec{"ageUncertaintyTypeName", "ageUncertaintyTypeId", ""},
    LookupSpec{"calculatedAgeUncertaintyTypeName", "calculatedAgeUncertaintyTypeId", ""},
};

struct NamedValue {
  double value;
  std::string_view name;
};

// Total 40K decay constants, per year. The service lists a fourth, `Carter
// et al. 2025`, which no preset of pychron-cpp uses; it is reached by name
// (PublishOptions), not by value.
constexpr std::array kDecayConstants = {
    NamedValue{5.543e-10, "Steiger and Jager, 1977"},
    NamedValue{5.463e-10, "Min et al. 2000"},
    NamedValue{5.5305e-10, "Renne et al. 2011"},  // the sheet's entry ends in a space
};

constexpr std::array kAirRatios = {
    NamedValue{298.56, "Lee et al. 2006"},
    NamedValue{295.5, "Nier 1950"},
};

constexpr double kRelativeTolerance = 1e-4;

std::string name_of(std::span<const NamedValue> table, double value) {
  for (const NamedValue& entry : table) {
    if (std::abs(value - entry.value) <= kRelativeTolerance * entry.value) return std::string(entry.name);
  }
  return {};  // also a NaN
}

struct Citation {
  std::string_view key, name;  // key: "<first author, lower case>_<year>"
};

constexpr std::array kDecayCitations = {
    Citation{"steiger_1977", "Steiger and Jager, 1977"},
    Citation{"steigerjager_1977", "Steiger and Jager, 1977"},
    Citation{"min_2000", "Min et al. 2000"},
    // The citation pychron carries is the 2008 paper, which revises the 2000 value.
    Citation{"min_2008", "Min et al. 2000"},
    Citation{"renne_2010", "Renne et al. 2011"},
    Citation{"renne_2011", "Renne et al. 2011"},
};

constexpr std::array kAirCitations = {
    Citation{"nier_1950", "Nier 1950"},
    Citation{"lee_2006", "Lee et al. 2006"},
};

bool is_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// The first 19xx or 20xx anywhere in the text.
std::string_view first_year(std::string_view text) {
  for (std::size_t i = 0; i + 4 <= text.size(); ++i) {
    const std::string_view century = text.substr(i, 2);
    if ((century == "19" || century == "20") && is_digit(text[i + 2]) && is_digit(text[i + 3])) {
      return text.substr(i, 4);
    }
  }
  return {};
}

// The first run of letters outside parentheses, lower case. A parenthesis
// that is never closed hides nothing.
std::string first_author(std::string_view text) {
  std::string author;
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '(') {
      const std::size_t close = text.find(')', i);
      if (close != std::string_view::npos) {
        if (!author.empty()) break;
        i = close + 1;
        continue;
      }
    }
    if (is_alpha(c)) {
      author.push_back(lower(c));
    } else if (!author.empty()) {
      break;
    }
    ++i;
  }
  return author;
}

// "author_year", or "author" with no year; empty with no author.
std::string citation_key(std::string_view citation) {
  std::string key = first_author(citation);
  if (key.empty()) return key;
  if (const std::string_view year = first_year(citation); !year.empty()) {
    key.push_back('_');
    key.append(year);
  }
  return key;
}

std::string shortest(double value) {
  std::array<char, 32> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string{};
}

}  // namespace

std::span<const LookupSpec> lookup_specs() { return kLookupSpecs; }

std::span<const LookupSpec> uncertainty_type_specs() { return kUncertaintyTypeSpecs; }

std::optional<int> uncertainty_type_id(std::string_view name) {
  std::string key;
  for (const char c : name) {
    if (is_alpha(c) || is_digit(c)) key.push_back(lower(c));
  }
  for (const std::string_view one : {"1sigma"sv, "1s"sv, "1"sv, "se"sv, "sem"sv, "sd"sv, "msem"sv}) {
    if (key == one) return 1;
  }
  for (const std::string_view two : {"2sigma"sv, "2s"sv, "2"sv}) {
    if (key == two) return 2;
  }
  return std::nullopt;
}

std::string ar_method(bool any_step, std::optional<double> max_extract_value) {
  if (!any_step) return "Total fusion - laser";
  if (max_extract_value) {
    if (*max_extract_value >= 300.0) return "Step-heating - furnace";
    if (*max_extract_value <= 50.0) return "Step-heating - laser";
  }
  return "Step-heating - undefined";
}

std::string decay_constant_name(double lambda_total_per_year) {
  return name_of(kDecayConstants, lambda_total_per_year);
}

std::string air_ratio_name(double atm4036) { return name_of(kAirRatios, atm4036); }

std::string normalize_citation(std::string_view override_text, bool decay) {
  const std::string key = citation_key(override_text);
  const std::span<const Citation> table =
      decay ? std::span<const Citation>(kDecayCitations) : std::span<const Citation>(kAirCitations);
  for (const Citation& citation : table) {
    if (key == citation.key) return std::string(citation.name);
  }
  return std::string(override_text);
}

std::string flux_monitor_query(std::string_view name, std::optional<double> age_ma) {
  std::string query(name);
  if (age_ma) {
    if (!query.empty()) query.push_back(' ');
    query += shortest(*age_ma);
    query += " Ma";
  }
  return query;
}

std::string_view age_type_name(AgeKind kind) {
  switch (kind) {
    case AgeKind::Plateau: return "Plateau Age";
    case AgeKind::WeightedMean: return "Weighted Mean Age";
    case AgeKind::Isochron: return "Inverse Isochron Age";
    // The integrated age: all the gas of a heating schedule summed.
    case AgeKind::Integrated: return "Total Gas Age";
  }
  return "";
}

}  // namespace pychron::earthbank
