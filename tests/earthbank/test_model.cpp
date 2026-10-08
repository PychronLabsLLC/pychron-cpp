#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "pychron/earthbank/model.hpp"
#include "records_support.hpp"

namespace pychron::earthbank {
namespace {

using testing::all_fields;

std::set<std::string> keys_of(const nlohmann::json& object) {
  std::set<std::string> keys;
  for (const auto& item : object.items()) keys.insert(item.key());
  return keys;
}

std::set<std::string> as_set(const std::vector<std::string>& names) {
  return {names.begin(), names.end()};
}

bool ends_with(std::string_view text, std::string_view tail) {
  return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
}

// The legacy integration's field lists (earthbank_service.py), written out.
const std::vector<std::string> kSampleFields = {
    "fundingName", "fundingId", "literatureName", "literatureId", "archiveId", "archiveName",
    "collectDateMax", "collectDateMin", "dataPackageId", "description", "id", "igsn", "locationId",
    "locationKindId", "locationKindName", "locationName", "materialId", "materialName", "name",
    "referenceElevation", "relativeElevationMax", "relativeElevationMin", "rockUnitAgeDescription",
    "rockUnitAgeMax", "rockUnitAgeMin", "sampleID", "sampleKindId", "sampleKindName",
    "sampleMethodId", "sampleMethodName", "stratographicUnitId", "stratographicUnitName"};
const std::vector<std::string> kLocationFields = {
    "description", "id", "lat", "latLonPrecision", "lon", "name"};
const std::vector<std::string> kDataPointFields = {
    "fundingName", "fundingId", "literatureName", "literatureId", "analysisDate",
    "analysisScaleId", "analysisScaleName", "analysisUnits", "analyticalUncertaintyTypeId",
    "analyticalUncertaintyTypeName", "analyticalUncertaintyUnitId",
    "analyticalUncertaintyUnitName", "apparentAgeUncertaintyTypeId",
    "apparentAgeUncertaintyTypeName", "apparentAgeUncertaintyUnitId",
    "apparentAgeUncertaintyUnitName", "arArAnalyticalSetUpId", "arMethodId", "arMethodName",
    "commentAnalyte", "grainDiameterMax", "grainDiameterMin", "id", "irradiationBatchIDId",
    "jvalueUncertaintyUnitId", "jvalueUncertaintyUnitName", "lithologyId", "lithologyName",
    "mineralId", "mineralName"};
const std::vector<std::string> kAliquotFields = {
    "aliquotComment", "aliquotName", "aliquotSize", "aliquotUnits", "arArDataPointId", "id"};
const std::vector<std::string> kMeasurementFields = {
    "aliquotName", "analysisTime", "analysisUnits", "apparentAge", "apparentAgeUncertainityTypeId",
    "apparentAgeUncertainityTypeName", "apparentAgeUncertainty", "ar36Corr", "ar36CorrUncertainty",
    "ar36ExcInt", "ar36ExcIntUncertainty", "ar36blank", "ar36blankUncertainty", "ar37Corr",
    "ar37CorrUncertainty", "ar37ExcInt", "ar37ExcIntUncertainty", "ar37blank",
    "ar37blankUncertainty", "ar38Corr", "ar38CorrUncertainty", "ar38ExcInt",
    "ar38ExcIntUncertainty", "ar38blank", "ar38blankUncertainty", "ar39Corr",
    "ar39CorrUncertainty", "ar39ExcInt", "ar39ExcIntUncertainty", "ar39KReleased", "ar39blank",
    "ar39blankUncertainty", "ar40Corr", "ar40CorrUncertainty", "ar40ExcInt",
    "ar40ExcIntUncertainty", "ar40blank", "ar40blankUncertainty", "arArDataPointId", "beamSize",
    "beamSizeUnits", "caK", "caKUncertainty", "clK", "clKUncertainty", "cumulative39ArK",
    "furnaceTemperature", "id", "inverseIsoStepId", "inverseIsoStepName", "jValue",
    "jValueUncertainty", "jvalueUncertaintyTypeId", "jvalueUncertaintyTypeName", "kCa",
    "kCaUncertainty", "kContent", "kContentUncertainty", "laserPower", "laserPowerUnits",
    "measurementComment", "plateauStepId", "plateauStepName", "radiogenicArgon",
    "ratio36Arx39ArCorr", "ratio36Arx39ArCorrUncertainty", "ratio37Arx39ArCorr",
    "ratio37Arx39ArCorrUncertainty", "ratio38Arx39ArCorr", "ratio38Arx39ArCorrUncertainty",
    "ratio40Arx36ArTrapped", "ratio40Arx36ArTrappedUncertainty", "ratio40Arx39ArCorr",
    "ratio40Arx39ArCorrUncertainty", "ratio40Arx39ArK", "ratio40Arx39ArKUncertainty", "stepNumber",
    "weightedMeanStepId", "weightedMeanStepName"};
const std::vector<std::string> kAgeCalcFields = {
    "airRatioId", "airRatioName", "arArDataPointId", "comment", "decayConstantId",
    "decayConstantName", "fluxMonitorId", "fluxMonitorName", "id"};
const std::vector<std::string> kAgeSummaryFields = {
    "age", "ageCalcTypeId", "ageCalcTypeName", "ageUncertainty", "ageUncertaintyTypeId",
    "ageUncertaintyTypeName", "agepvalue", "arArDataPointId", "calculatedAgeUncertaintyTypeId",
    "calculatedAgeUncertaintyTypeName", "calculatedAgeUncertaintyTypeUnitsId",
    "calculatedAgeUncertaintyTypeUnitsName", "dataInterpretationToolId",
    "dataInterpretationToolName", "description", "id", "interpretationId", "interpretationName",
    "mswd", "nAnalysisGrouped", "preferredAge"};

Submission submission() {
  Submission s;
  s.datapoint_name = "S-1 Sanidine";
  s.sample.name = "S-1";
  s.sample.materialName = "Sanidine";
  s.location = Location{};
  s.location->lat = 35.0;
  s.location->lon = -106.5;
  s.data_point.arMethodName = "Step-heating - laser";
  Aliquot aliquot;
  aliquot.aliquotName = "10001-01";
  s.aliquots.push_back(aliquot);
  Measurement first;
  first.aliquotName = "10001-01";
  first.stepNumber = 1.0;
  first.ar40Corr = 1.5;
  Measurement second;
  second.aliquotName = "10001-01";
  second.stepNumber = 2.0;
  second.ar40Corr = 2.5;
  s.measurements = {first, second};
  s.age_calc.airRatioName = "Lee et al. 2006";
  s.age_summary.age = 28.2;
  s.age_summary.ageUncertainty = 0.05;
  s.age_summary.nAnalysisGrouped = 2;
  s.age_summary.preferredAge = true;
  return s;
}

TEST(Model, ToJsonHasOnlySetFieldsUnderServiceNames) {
  Measurement m;
  m.ar40Corr = 1.5;
  m.apparentAgeUncertainityTypeName = "SE";
  EXPECT_EQ(to_json(m).dump(), R"({"apparentAgeUncertainityTypeName":"SE","ar40Corr":1.5})");
  EXPECT_EQ(to_json(Measurement{}).dump(), "{}");
}

// A number that is not finite has no JSON form (nlohmann would write null
// under a numeric key): it is left out, as if the field were unset.
TEST(Model, NonFiniteNumbersAreNotEmitted) {
  Measurement m;
  m.ar40Corr = 1.5;
  m.ar40CorrUncertainty = std::numeric_limits<double>::quiet_NaN();
  m.ar39Corr = std::numeric_limits<double>::infinity();
  m.ar38Corr = -std::numeric_limits<double>::infinity();
  m.stepNumber = 0.0;  // zero is a number
  EXPECT_EQ(to_json(m).dump(), R"({"ar40Corr":1.5,"stepNumber":0.0})");

  Location l;
  l.lat = std::nan("");
  l.lon = -106.5;
  EXPECT_EQ(to_json(l).dump(), R"({"lon":-106.5})");

  // So it takes no part in the hash either.
  Submission with = submission();
  with.measurements[0].apparentAge = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(content_hash(with), content_hash(submission()));
}

// An empty string says nothing to the service: it is left out like an unset
// field. The two still compare unequal as records.
TEST(Model, EmptyTextIsNotEmitted) {
  Sample s;
  s.name = "S-1";
  s.igsn = "";
  s.materialName = "";
  EXPECT_EQ(to_json(s).dump(), R"({"name":"S-1"})");

  Sample unset;
  unset.name = "S-1";
  EXPECT_EQ(to_json(unset), to_json(s));
  EXPECT_NE(unset, s);

  // Blank is not empty: only the empty string is dropped.
  s.igsn = " ";
  EXPECT_EQ(to_json(s).dump(), R"({"igsn":" ","name":"S-1"})");

  Submission with = submission();
  with.age_summary.description = "";
  EXPECT_EQ(content_hash(with), content_hash(submission()));
}

TEST(Model, EveryLegacyFieldIsPresent) {
  EXPECT_EQ(keys_of(all_fields<Sample>()), as_set(kSampleFields));
  EXPECT_EQ(keys_of(all_fields<Location>()), as_set(kLocationFields));
  EXPECT_EQ(keys_of(all_fields<DataPoint>()), as_set(kDataPointFields));
  EXPECT_EQ(keys_of(all_fields<Aliquot>()), as_set(kAliquotFields));
  EXPECT_EQ(keys_of(all_fields<Measurement>()), as_set(kMeasurementFields));
  EXPECT_EQ(keys_of(all_fields<AgeCalc>()), as_set(kAgeCalcFields));
  EXPECT_EQ(keys_of(all_fields<AgeSummary>()), as_set(kAgeSummaryFields));
  // No name written twice above, so the sets are the lists.
  EXPECT_EQ(kSampleFields.size(), 32u);
  EXPECT_EQ(kLocationFields.size(), 6u);
  EXPECT_EQ(kDataPointFields.size(), 30u);
  EXPECT_EQ(kAliquotFields.size(), 6u);
  EXPECT_EQ(kMeasurementFields.size(), 79u);
  EXPECT_EQ(kAgeCalcFields.size(), 9u);
  EXPECT_EQ(kAgeSummaryFields.size(), 21u);
  EXPECT_EQ(all_fields<Measurement>().size(), 79u);
}

TEST(Model, IdsAreIntegersNamesAreTextAndTheFlagIsABoolean) {
  const std::vector<nlohmann::json> records = {
      all_fields<Sample>(),      all_fields<Location>(), all_fields<DataPoint>(), all_fields<Aliquot>(),
      all_fields<Measurement>(), all_fields<AgeCalc>(),  all_fields<AgeSummary>()};
  std::size_t ids = 0;
  for (const nlohmann::json& record : records) {
    for (const auto& item : record.items()) {
      const std::string& key = item.key();
      if (key == "id" || ends_with(key, "Id")) {
        EXPECT_TRUE(item.value().is_number_integer()) << key;
        ++ids;
      } else if (key == "name" || ends_with(key, "Name")) {
        EXPECT_TRUE(item.value().is_string()) << key;
      } else if (key == "preferredAge") {
        EXPECT_TRUE(item.value().is_boolean()) << key;
      } else if (key == "nAnalysisGrouped") {
        EXPECT_TRUE(item.value().is_number_integer()) << key;
      } else {
        EXPECT_TRUE(item.value().is_number_float() || item.value().is_string()) << key;
      }
    }
  }
  EXPECT_EQ(ids, 48u);
  // The lab's own identifier is text, whatever its name ends in.
  EXPECT_TRUE(all_fields<Sample>()["sampleID"].is_string());
  for (const char* number : {"lat", "lon", "latLonPrecision"}) {
    EXPECT_TRUE(all_fields<Location>()[number].is_number_float()) << number;
  }
  for (const char* number : {"ar40Corr", "apparentAge", "stepNumber", "jValue", "laserPower"}) {
    EXPECT_TRUE(all_fields<Measurement>()[number].is_number_float()) << number;
  }
  for (const char* text : {"analysisTime", "analysisUnits", "laserPowerUnits", "measurementComment"}) {
    EXPECT_TRUE(all_fields<Measurement>()[text].is_string()) << text;
  }
}

TEST(Model, ContentHashIgnoresNotesAndFieldOrder) {
  const Submission a = submission();
  const std::string hash = content_hash(a);
  EXPECT_EQ(hash.size(), 64u);
  EXPECT_TRUE(std::all_of(hash.begin(), hash.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  })) << hash;

  Submission noted = a;
  noted.notes = {"the plateau age was chosen", "one analysis left out"};
  EXPECT_EQ(content_hash(noted), hash);

  // The same values, set in another order.
  Submission b;
  b.age_summary.preferredAge = true;
  b.age_summary.nAnalysisGrouped = 2;
  b.age_summary.ageUncertainty = 0.05;
  b.age_summary.age = 28.2;
  b.age_calc.airRatioName = "Lee et al. 2006";
  Measurement first;
  first.ar40Corr = 1.5;
  first.stepNumber = 1.0;
  first.aliquotName = "10001-01";
  Measurement second;
  second.ar40Corr = 2.5;
  second.stepNumber = 2.0;
  second.aliquotName = "10001-01";
  b.measurements.push_back(first);
  b.measurements.push_back(second);
  b.aliquots.emplace_back().aliquotName = "10001-01";
  b.data_point.arMethodName = "Step-heating - laser";
  Location location;
  location.lon = -106.5;
  location.lat = 35.0;
  b.location = location;
  b.sample.materialName = "Sanidine";
  b.sample.name = "S-1";
  b.datapoint_name = "S-1 Sanidine";
  EXPECT_EQ(b, a);
  EXPECT_EQ(content_hash(b), hash);
}

// The hash of submission() above with a set-up and an irradiation named,
// worked out apart from this code: the SHA-256 of the text below. A platform
// that writes a number another way (28.2 as 28.199999999999999, 1.0 as 1)
// fails here.
TEST(Model, ContentHashOfAKnownSubmission) {
  const std::string text =
      R"(["S-1 Sanidine","NMGRL Helix","NM-900",{"materialName":"Sanidine","name":"S-1"},{"lat":35.0,"lon":-106.5},)"
      R"({"arMethodName":"Step-heating - laser"},[{"aliquotName":"10001-01"}],)"
      R"([{"aliquotName":"10001-01","ar40Corr":1.5,"stepNumber":1.0},)"
      R"({"aliquotName":"10001-01","ar40Corr":2.5,"stepNumber":2.0}],)"
      R"({"airRatioName":"Lee et al. 2006"},)"
      R"({"age":28.2,"ageUncertainty":0.05,"nAnalysisGrouped":2,"preferredAge":true}])";
  Submission known = submission();
  known.setup_name = "NMGRL Helix";
  known.irradiation_name = "NM-900";
  EXPECT_EQ(content_hash(known), "f261df4c9dea7052e1240e720c92de5570ddea090467efe91b185a28f984579f") << text;
}

TEST(Model, ContentHashChangesWithAValue) {
  const Submission a = submission();
  const std::string hash = content_hash(a);

  Submission value = a;
  value.measurements[1].ar40Corr = 2.5000001;
  EXPECT_NE(content_hash(value), hash);

  Submission text = a;
  text.sample.name = "S-2";
  EXPECT_NE(content_hash(text), hash);

  Submission added = a;
  added.age_summary.mswd = 1.1;
  EXPECT_NE(content_hash(added), hash);

  Submission named = a;
  named.datapoint_name = "S-1";
  EXPECT_NE(content_hash(named), hash);

  // The set-up and the irradiation batch are sent with the data point.
  Submission set_up = a;
  set_up.setup_name = "NMGRL Helix";
  EXPECT_NE(content_hash(set_up), hash);
  Submission irradiated = a;
  irradiated.irradiation_name = "NMGRL Helix";
  EXPECT_NE(content_hash(irradiated), hash);
  EXPECT_NE(content_hash(irradiated), content_hash(set_up));

  Submission no_location = a;
  no_location.location.reset();
  EXPECT_NE(content_hash(no_location), hash);

  // A field moved from one record to its neighbour is another submission.
  Submission moved = a;
  moved.measurements[0].ar40Corr.reset();
  moved.measurements[1].ar39Corr = 1.5;
  EXPECT_NE(content_hash(moved), hash);

  Submission swapped = a;
  std::swap(swapped.measurements[0], swapped.measurements[1]);
  EXPECT_NE(content_hash(swapped), hash);
}

}  // namespace
}  // namespace pychron::earthbank
