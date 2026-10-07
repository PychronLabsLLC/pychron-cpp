// The records a lab sends to EarthBank (AuScope's AusGeochem data platform)
// to publish the 40Ar/39Ar results of one sample.
//
// The service stores a sample's argon data as a small tree. A Sample (what
// was collected, of what material) sits at a Location. An Ar-Ar DataPoint
// hangs off the sample: one dated material, with the method and the
// conventions its numbers are quoted in. Under the data point are its
// Aliquots (the pieces that were loaded), its Measurements (one per analysis
// or heating step: the corrected intensities, the ratios, the apparent age),
// one AgeCalc (the decay constant, air ratio and flux monitor the ages rest
// on) and one AgeSummary (the age the lab reports for the sample).
//
// Each record here is a plain struct with one optional member per field of
// the service's record, named exactly as the service names it, misspellings
// included (`apparentAgeUncertainityTypeName`, `stratographicUnitName`). The
// lists are those of the legacy integration's payload builder. A field left
// empty is not sent.
//
// A controlled term appears twice: as `<term>Name`, which is what the mapping
// fills and what the vendor's spreadsheet takes, and as `<term>Id`, which is
// what the REST API stores and what a lookup fills later (vocab.hpp).
//
// The struct, to_json and anything else that walks a record's fields are
// generated from one list per record (the PYCHRON_EARTHBANK_*_FIELDS macros
// below), so they cannot drift apart. to_json is also how a field is read by
// its service name: there is no other reflection.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pychron::earthbank {

// The kinds of field. Ages are in Ma, uncertainties 1 sigma and absolute,
// intensities in the data point's `analysisUnits`.
using Number = std::optional<double>;
using Text = std::optional<std::string>;  // free text, a date or time, or a controlled term's name
using Id = std::optional<std::int64_t>;   // a row of the service: `id` and every `*Id`
using Count = std::optional<std::int64_t>;
using Flag = std::optional<bool>;

// One X(kind, name) per field, in the order of the legacy lists.

#define PYCHRON_EARTHBANK_SAMPLE_FIELDS(X) \
  X(Text, fundingName) \
  X(Id, fundingId) \
  X(Text, literatureName) \
  X(Id, literatureId) \
  X(Id, archiveId) \
  X(Text, archiveName) \
  X(Text, collectDateMax) \
  X(Text, collectDateMin) \
  X(Id, dataPackageId) \
  X(Text, description) \
  X(Id, id) \
  X(Text, igsn) \
  X(Id, locationId) \
  X(Id, locationKindId) \
  X(Text, locationKindName) \
  X(Text, locationName) \
  X(Id, materialId) \
  X(Text, materialName) \
  X(Text, name) \
  X(Number, referenceElevation) \
  X(Number, relativeElevationMax) \
  X(Number, relativeElevationMin) \
  X(Text, rockUnitAgeDescription) \
  X(Number, rockUnitAgeMax) \
  X(Number, rockUnitAgeMin) \
  X(Text, sampleID) \
  X(Id, sampleKindId) \
  X(Text, sampleKindName) \
  X(Id, sampleMethodId) \
  X(Text, sampleMethodName) \
  X(Id, stratographicUnitId) \
  X(Text, stratographicUnitName)

#define PYCHRON_EARTHBANK_LOCATION_FIELDS(X) \
  X(Text, description) \
  X(Id, id) \
  X(Number, lat) \
  X(Number, latLonPrecision) \
  X(Number, lon) \
  X(Text, name)

#define PYCHRON_EARTHBANK_DATA_POINT_FIELDS(X) \
  X(Text, fundingName) \
  X(Id, fundingId) \
  X(Text, literatureName) \
  X(Id, literatureId) \
  X(Text, analysisDate) \
  X(Id, analysisScaleId) \
  X(Text, analysisScaleName) \
  X(Text, analysisUnits) \
  X(Id, analyticalUncertaintyTypeId) \
  X(Text, analyticalUncertaintyTypeName) \
  X(Id, analyticalUncertaintyUnitId) \
  X(Text, analyticalUncertaintyUnitName) \
  X(Id, apparentAgeUncertaintyTypeId) \
  X(Text, apparentAgeUncertaintyTypeName) \
  X(Id, apparentAgeUncertaintyUnitId) \
  X(Text, apparentAgeUncertaintyUnitName) \
  X(Id, arArAnalyticalSetUpId) \
  X(Id, arMethodId) \
  X(Text, arMethodName) \
  X(Text, commentAnalyte) \
  X(Number, grainDiameterMax) \
  X(Number, grainDiameterMin) \
  X(Id, id) \
  X(Id, irradiationBatchIDId) \
  X(Id, jvalueUncertaintyUnitId) \
  X(Text, jvalueUncertaintyUnitName) \
  X(Id, lithologyId) \
  X(Text, lithologyName) \
  X(Id, mineralId) \
  X(Text, mineralName)

#define PYCHRON_EARTHBANK_ALIQUOT_FIELDS(X) \
  X(Text, aliquotComment) \
  X(Text, aliquotName) \
  X(Number, aliquotSize) \
  X(Text, aliquotUnits) \
  X(Id, arArDataPointId) \
  X(Id, id)

#define PYCHRON_EARTHBANK_MEASUREMENT_FIELDS(X) \
  X(Text, aliquotName) \
  X(Text, analysisTime) \
  X(Text, analysisUnits) \
  X(Number, apparentAge) \
  X(Id, apparentAgeUncertainityTypeId) \
  X(Text, apparentAgeUncertainityTypeName) \
  X(Number, apparentAgeUncertainty) \
  X(Number, ar36Corr) \
  X(Number, ar36CorrUncertainty) \
  X(Number, ar36ExcInt) \
  X(Number, ar36ExcIntUncertainty) \
  X(Number, ar36blank) \
  X(Number, ar36blankUncertainty) \
  X(Number, ar37Corr) \
  X(Number, ar37CorrUncertainty) \
  X(Number, ar37ExcInt) \
  X(Number, ar37ExcIntUncertainty) \
  X(Number, ar37blank) \
  X(Number, ar37blankUncertainty) \
  X(Number, ar38Corr) \
  X(Number, ar38CorrUncertainty) \
  X(Number, ar38ExcInt) \
  X(Number, ar38ExcIntUncertainty) \
  X(Number, ar38blank) \
  X(Number, ar38blankUncertainty) \
  X(Number, ar39Corr) \
  X(Number, ar39CorrUncertainty) \
  X(Number, ar39ExcInt) \
  X(Number, ar39ExcIntUncertainty) \
  X(Number, ar39KReleased) \
  X(Number, ar39blank) \
  X(Number, ar39blankUncertainty) \
  X(Number, ar40Corr) \
  X(Number, ar40CorrUncertainty) \
  X(Number, ar40ExcInt) \
  X(Number, ar40ExcIntUncertainty) \
  X(Number, ar40blank) \
  X(Number, ar40blankUncertainty) \
  X(Id, arArDataPointId) \
  X(Number, beamSize) \
  X(Text, beamSizeUnits) \
  X(Number, caK) \
  X(Number, caKUncertainty) \
  X(Number, clK) \
  X(Number, clKUncertainty) \
  X(Number, cumulative39ArK) \
  X(Number, furnaceTemperature) \
  X(Id, id) \
  X(Id, inverseIsoStepId) \
  X(Text, inverseIsoStepName) \
  X(Number, jValue) \
  X(Number, jValueUncertainty) \
  X(Id, jvalueUncertaintyTypeId) \
  X(Text, jvalueUncertaintyTypeName) \
  X(Number, kCa) \
  X(Number, kCaUncertainty) \
  X(Number, kContent) \
  X(Number, kContentUncertainty) \
  X(Number, laserPower) \
  X(Text, laserPowerUnits) \
  X(Text, measurementComment) \
  X(Id, plateauStepId) \
  X(Text, plateauStepName) \
  X(Number, radiogenicArgon) \
  X(Number, ratio36Arx39ArCorr) \
  X(Number, ratio36Arx39ArCorrUncertainty) \
  X(Number, ratio37Arx39ArCorr) \
  X(Number, ratio37Arx39ArCorrUncertainty) \
  X(Number, ratio38Arx39ArCorr) \
  X(Number, ratio38Arx39ArCorrUncertainty) \
  X(Number, ratio40Arx36ArTrapped) \
  X(Number, ratio40Arx36ArTrappedUncertainty) \
  X(Number, ratio40Arx39ArCorr) \
  X(Number, ratio40Arx39ArCorrUncertainty) \
  X(Number, ratio40Arx39ArK) \
  X(Number, ratio40Arx39ArKUncertainty) \
  X(Number, stepNumber) \
  X(Id, weightedMeanStepId) \
  X(Text, weightedMeanStepName)

#define PYCHRON_EARTHBANK_AGE_CALC_FIELDS(X) \
  X(Id, airRatioId) \
  X(Text, airRatioName) \
  X(Id, arArDataPointId) \
  X(Text, comment) \
  X(Id, decayConstantId) \
  X(Text, decayConstantName) \
  X(Id, fluxMonitorId) \
  X(Text, fluxMonitorName) \
  X(Id, id)

#define PYCHRON_EARTHBANK_AGE_SUMMARY_FIELDS(X) \
  X(Number, age) \
  X(Id, ageCalcTypeId) \
  X(Text, ageCalcTypeName) \
  X(Number, ageUncertainty) \
  X(Id, ageUncertaintyTypeId) \
  X(Text, ageUncertaintyTypeName) \
  X(Number, agepvalue) \
  X(Id, arArDataPointId) \
  X(Id, calculatedAgeUncertaintyTypeId) \
  X(Text, calculatedAgeUncertaintyTypeName) \
  X(Id, calculatedAgeUncertaintyTypeUnitsId) \
  X(Text, calculatedAgeUncertaintyTypeUnitsName) \
  X(Id, dataInterpretationToolId) \
  X(Text, dataInterpretationToolName) \
  X(Text, description) \
  X(Id, id) \
  X(Id, interpretationId) \
  X(Text, interpretationName) \
  X(Number, mswd) \
  X(Count, nAnalysisGrouped) \
  X(Flag, preferredAge)

#define PYCHRON_EARTHBANK_MEMBER(kind, name) kind name;

// What was collected. `sampleID` is the lab's own identifier for it (text),
// `id` the service's row.
struct Sample {
  PYCHRON_EARTHBANK_SAMPLE_FIELDS(PYCHRON_EARTHBANK_MEMBER)
  bool operator==(const Sample&) const = default;
};

// Where it was collected: decimal degrees, WGS 84.
struct Location {
  PYCHRON_EARTHBANK_LOCATION_FIELDS(PYCHRON_EARTHBANK_MEMBER)
  bool operator==(const Location&) const = default;
};

// The Ar-Ar data point: one dated material of the sample. `fundingName` and
// `literatureName` (here and on Sample) are not fields of the service's
// record; they are carried for the spreadsheet's columns and for the link
// tables an upload fills.
struct DataPoint {
  PYCHRON_EARTHBANK_DATA_POINT_FIELDS(PYCHRON_EARTHBANK_MEMBER)
  bool operator==(const DataPoint&) const = default;
};

// One loaded piece of the material. Measurements name their aliquot by
// `aliquotName`.
struct Aliquot {
  PYCHRON_EARTHBANK_ALIQUOT_FIELDS(PYCHRON_EARTHBANK_MEMBER)
  bool operator==(const Aliquot&) const = default;
};

// One analysis: a fusion, or one step of a heating schedule.
struct Measurement {
  PYCHRON_EARTHBANK_MEASUREMENT_FIELDS(PYCHRON_EARTHBANK_MEMBER)
  bool operator==(const Measurement&) const = default;
};

// The references the ages were calculated with.
struct AgeCalc {
  PYCHRON_EARTHBANK_AGE_CALC_FIELDS(PYCHRON_EARTHBANK_MEMBER)
  bool operator==(const AgeCalc&) const = default;
};

// The age reported for the sample and how it was arrived at.
struct AgeSummary {
  PYCHRON_EARTHBANK_AGE_SUMMARY_FIELDS(PYCHRON_EARTHBANK_MEMBER)
  bool operator==(const AgeSummary&) const = default;
};

#undef PYCHRON_EARTHBANK_MEMBER

// Everything published for one sample. It is built once and is what the
// pre-flight check reads, what the spreadsheet is filled from and what is
// uploaded. `notes` are for the user (which age was chosen and why, what was
// left out); they are not sent.
struct Submission {
  Sample sample;
  std::optional<Location> location;
  DataPoint data_point;
  std::vector<Aliquot> aliquots;
  std::vector<Measurement> measurements;
  AgeCalc age_calc;
  AgeSummary age_summary;
  std::vector<std::string> notes;
  bool operator==(const Submission&) const = default;
};

// The record as the service takes it: an object holding the fields that are
// set, and only those, under their service names. Keys come out sorted. A
// number that is not finite dumps as null (nlohmann's rule); the mapping does
// not set one.
nlohmann::json to_json(const Sample& record);
nlohmann::json to_json(const Location& record);
nlohmann::json to_json(const DataPoint& record);
nlohmann::json to_json(const Aliquot& record);
nlohmann::json to_json(const Measurement& record);
nlohmann::json to_json(const AgeCalc& record);
nlohmann::json to_json(const AgeSummary& record);

// Lower-case hex SHA-256 of the submission's records: to_json of the sample,
// the location (null when there is none), the data point, the aliquots, the
// measurements, the age calculation and the age summary, in that order, as
// one JSON array dumped without whitespace. `notes` take no part. Two
// submissions with the same hash would send the same thing; the order of
// aliquots and of measurements counts.
std::string content_hash(const Submission& submission);

}  // namespace pychron::earthbank
