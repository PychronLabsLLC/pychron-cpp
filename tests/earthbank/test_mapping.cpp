#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "analyses.hpp"
#include "pychron/core/error.hpp"
#include "pychron/earthbank/mapping.hpp"
#include "pychron/earthbank/model.hpp"
#include "pychron/earthbank/vocab.hpp"
#include "pychron/processing/arar_groups.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/processing/group_ages.hpp"
#include "pychron/processing/model.hpp"
#include "pychron/processing/reduced.hpp"
#include "pychron/processing/report.hpp"
#include "pychron/reduction/arar_reduction.hpp"
#include "pychron/reduction/arar_types.hpp"
#include "pychron/reduction/stats.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::earthbank {
namespace {

namespace pp = pychron::processing;
namespace r = pychron::reduction;
using testing::AnalysisList;
using testing::both_samples;
using testing::fusion_analysis;
using testing::fusion_sample;
using testing::irradiate_for_real;
using testing::step_analysis;
using testing::step_sample;
using testing::with_edit;
using testing::with_edit_all;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::vector<Submission> build(const AnalysisList& analyses, const PublishOptions& options = {}) {
  auto built = build_submissions(analyses, options);
  EXPECT_TRUE(built.has_value()) << (built ? "" : built.error().what);
  return built ? *built : std::vector<Submission>{};
}

// The one submission of a one-sample list.
Submission build_one(const AnalysisList& analyses, const PublishOptions& options = {}) {
  const auto built = build(analyses, options);
  EXPECT_EQ(built.size(), 1u);
  return built.empty() ? Submission{} : built.front();
}

std::string refused(const AnalysisList& analyses, const PublishOptions& options = {}) {
  const auto built = build_submissions(analyses, options);
  EXPECT_FALSE(built.has_value());
  if (built) return {};
  EXPECT_EQ(built.error().kind, ErrorKind::Config);
  return built.error().what;
}

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// The note that has `part` in it, or nothing.
std::string note_with(const Submission& s, std::string_view part) {
  for (const std::string& note : s.notes) {
    if (contains(note, part)) return note;
  }
  return {};
}

// 1e-12 relative: the mapping passes the reduction's number on, at most
// scaled once.
#define EXPECT_REL(got, want)                                             \
  do {                                                                    \
    const std::optional<double> got_ = (got);                             \
    const double want_ = (want);                                          \
    ASSERT_TRUE(got_.has_value()) << #got;                                \
    EXPECT_TRUE(std::isfinite(want_)) << #want;                           \
    EXPECT_NEAR(*got_, want_, std::abs(want_) * 1e-12) << #got " " #want; \
  } while (false)

// A field of a record by its service name; nothing when it is not sent.
template <typename Record>
std::optional<double> number(const Record& record, const std::string& field) {
  const nlohmann::json j = to_json(record);
  if (!j.contains(field)) return std::nullopt;
  return j.at(field).template get<double>();
}

// A number as a sentence of the mapping prints it, to `digits` significant figures.
std::string printed(double value, int digits) {
  std::array<char, 32> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.*g", digits, value);
  return buffer.data();
}

// The analyses reduced and put in a dataset, as the report takes them.
pp::Dataset dataset_of(const AnalysisList& analyses, const pp::ReductionSettings& settings = {}) {
  pp::Dataset d;
  for (const auto& a : analyses) d.mutable_items().push_back(pp::DatasetItem{pp::reduce_analysis(a, settings), {}, {}});
  return d;
}

pp::GroupItems items_of(const pp::Dataset& d) {
  pp::GroupItems items;
  for (const auto& item : d.items()) items.push_back(&item);
  return pp::step_ordered(items);
}

std::optional<double> summary_cell(const pp::Report& report, const std::string& column);

// The number in a summary column; NaN when the cell is empty.
double cell(const pp::Report& report, const std::string& column) { return summary_cell(report, column).value_or(kNaN); }

std::optional<double> summary_cell(const pp::Report& report, const std::string& column) {
  const pp::ReportTable& t = report.summary;
  const auto it = std::find(t.columns.begin(), t.columns.end(), column);
  if (it == t.columns.end() || t.rows.size() != 1) {
    ADD_FAILURE() << "no column " << column << " or not one row";
    return std::nullopt;
  }
  const auto& cell = t.rows.front().at(static_cast<std::size_t>(it - t.columns.begin()));
  if (const double* v = std::get_if<double>(&cell)) return *v;
  return std::nullopt;
}

// The report of the same analyses at one sigma.
pp::Report report_of(const AnalysisList& analyses, const PublishOptions& options = {}) {
  pp::ReportOptions o = options.ages;
  o.nsigma = 1;
  return pp::make_report(dataset_of(analyses, options.reduction), o);
}

// One letter per measurement, in order, for a "step included" column: Y for
// `Yes`, N for `No`, - for not set.
std::string flags(const Submission& s, Text Measurement::*column) {
  std::string out;
  for (const Measurement& m : s.measurements) {
    const Text& flag = m.*column;
    if (flag) {
      EXPECT_TRUE(*flag == "Yes" || *flag == "No") << *flag;
    }
    out += !flag ? '-' : *flag == "Yes" ? 'Y' : 'N';
  }
  return out;
}

// The steps flagged as steps of the reported plateau.
std::vector<std::string> plateau_flags(const Submission& s) {
  std::vector<std::string> steps;
  for (const Measurement& m : s.measurements) {
    if (m.plateauStepName != "Yes") continue;
    steps.push_back(pp::step_letters(static_cast<int>(*m.stepNumber) - 1));
  }
  return steps;
}

// `analysis` without the isotope `name`.
template <typename AnalysisT>
void drop_isotope(AnalysisT& a, std::string_view name) {
  a.isotopes.erase(std::remove_if(a.isotopes.begin(), a.isotopes.end(),
                                  [name](const pp::IsotopeData& iso) { return iso.isotope == name; }),
                   a.isotopes.end());
}

std::set<std::string> keys_of(const nlohmann::json& record) {
  std::set<std::string> keys;
  for (const auto& item : record.items()) keys.insert(item.key());
  return keys;
}

// ---------------------------------------------------------------- samples

TEST(Mapping, OneSubmissionPerSampleInNameOrder) {
  const auto built = build(both_samples());
  ASSERT_EQ(built.size(), 2u);
  EXPECT_EQ(built[0].sample.name, "S-FUS");
  EXPECT_EQ(built[1].sample.name, "S-STEP");
  EXPECT_EQ(built[0].measurements.size(), static_cast<std::size_t>(testing::kFusionCount));
  EXPECT_EQ(built[1].measurements.size(), static_cast<std::size_t>(testing::kStepCount));
  EXPECT_EQ(built[0].datapoint_name, "S-FUS biotite");
  EXPECT_EQ(built[1].datapoint_name, "S-STEP sanidine");

  // Within a sample the measurements are in time order, whatever order they came in.
  AnalysisList reversed = both_samples();
  std::reverse(reversed.begin(), reversed.end());
  const auto again = build(reversed);
  ASSERT_EQ(again.size(), 2u);
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(again[i], built[i]);
    EXPECT_EQ(content_hash(again[i]), content_hash(built[i]));
  }
  for (std::size_t i = 0; i < built[1].measurements.size(); ++i) {
    EXPECT_EQ(built[1].measurements[i].stepNumber, static_cast<double>(i + 1));
  }

  EXPECT_TRUE(build({}).empty());
  EXPECT_TRUE(contains(refused(with_edit(step_sample(), 0, [](pp::Analysis& a) { a.sample.clear(); })), "20001-01A"));
}

TEST(Mapping, SampleAndLocation) {
  PublishOptions options;
  options.software = "pychron-cpp 9.9.9";
  const Submission s = build_one(step_sample(), options);
  EXPECT_EQ(s.sample.name, "S-STEP");
  EXPECT_EQ(s.sample.sampleID, "S-STEP");
  EXPECT_EQ(s.sample.materialName, "sanidine");
  EXPECT_EQ(s.sample.referenceElevation, 2850.0);
  EXPECT_EQ(s.sample.igsn, "IEXXX0001");
  EXPECT_EQ(s.sample.stratographicUnitName, "Fish Canyon Tuff");
  EXPECT_EQ(s.sample.description, "Uploaded from pychron-cpp 9.9.9");
  EXPECT_EQ(build_one(step_sample()).sample.description, "Uploaded from pychron-cpp");
  // No name given: the fallback the age summary uses, not a sentence without its end.
  options.software.clear();
  const Submission unnamed = build_one(step_sample(), options);
  EXPECT_EQ(unnamed.sample.description, "Uploaded from pychron-cpp");
  EXPECT_TRUE(contains(unnamed.age_summary.description.value_or(""), "; reduced with pychron-cpp."));
  ASSERT_TRUE(s.location.has_value());
  EXPECT_EQ(s.location->lat, 37.75);
  EXPECT_EQ(s.location->lon, -106.9);
  EXPECT_EQ(to_json(*s.location).size(), 2u);

  // The catalog knows nothing of the fusion sample but its material.
  const Submission bare = build_one(fusion_sample());
  EXPECT_FALSE(bare.location.has_value());
  EXPECT_EQ(to_json(bare.sample).dump(),
            R"({"description":"Uploaded from pychron-cpp","materialName":"biotite","name":"S-FUS","sampleID":"S-FUS"})");
  EXPECT_EQ(note_with(bare, "location"), "");

  // Half a point is no point, and the user is told.
  const Submission half =
      build_one(with_edit_all(step_sample(), [](pp::Analysis& a) { a.sample_info.longitude.reset(); }));
  EXPECT_FALSE(half.location.has_value());
  EXPECT_TRUE(contains(note_with(half, "no location"), "latitude and no longitude")) << note_with(half, "no location");
  const Submission other_half =
      build_one(with_edit_all(step_sample(), [](pp::Analysis& a) { a.sample_info.latitude.reset(); }));
  EXPECT_FALSE(other_half.location.has_value());
  EXPECT_TRUE(contains(note_with(other_half, "no location"), "longitude and no latitude"));

  // A fact only some analyses carry is the sample's.
  AnalysisList some = with_edit_all(step_sample(), [](pp::Analysis& a) { a.sample_info = {}; });
  some = with_edit(some, 7, [](pp::Analysis& a) {
    a.sample_info.latitude = -37.8;
    a.sample_info.longitude = 144.96;
    a.sample_info.lithology = "tuff";
  });
  const Submission from_one = build_one(some);
  ASSERT_TRUE(from_one.location.has_value());
  EXPECT_EQ(from_one.location->lat, -37.8);
  EXPECT_EQ(from_one.location->lon, 144.96);
  EXPECT_EQ(from_one.data_point.lithologyName, "tuff");

  // A point is one analysis's latitude with the same analysis's longitude:
  // a latitude from one and a longitude from another are not a place.
  AnalysisList halves = with_edit_all(step_sample(), [](pp::Analysis& a) { a.sample_info = {}; });
  halves = with_edit(halves, 2, [](pp::Analysis& a) { a.sample_info.latitude = -37.8; });
  halves = with_edit(halves, 6, [](pp::Analysis& a) { a.sample_info.longitude = 144.96; });
  const Submission composed = build_one(halves);
  EXPECT_FALSE(composed.location.has_value());
  EXPECT_EQ(note_with(composed, "no location"),
            "S-STEP: no analysis has both a latitude and a longitude; no location is sent");

  // Half a point beside whole ones adds nothing when it agrees with them...
  const Submission beside =
      build_one(with_edit(step_sample(), 3, [](pp::Analysis& a) { a.sample_info.longitude.reset(); }));
  ASSERT_TRUE(beside.location.has_value());
  EXPECT_EQ(beside.location->lat, 37.75);
  EXPECT_EQ(beside.location->lon, -106.9);
  // ...and is a disagreement when it does not.
  const std::string what = refused(with_edit(step_sample(), 3, [](pp::Analysis& a) {
    a.sample_info.latitude = 38.0;
    a.sample_info.longitude.reset();
  }));
  EXPECT_TRUE(contains(what, "earthbank: sample \"S-STEP\"") && contains(what, "latitude differs between analyses") &&
              contains(what, "37.75 (20001-01A)") && contains(what, "38 (20001-01D)"))
      << what;
}

TEST(Mapping, DataPoint) {
  const Submission step = build_one(step_sample());
  // Step A ran at 2023-11-14 22:13:20 UTC; the later steps run past midnight.
  EXPECT_EQ(step.data_point.analysisDate, "2023-11-14");
  EXPECT_EQ(step.measurements.back().analysisTime, "2023-11-15T02:43:20Z");
  EXPECT_EQ(step.data_point.analysisUnits, "fA");
  EXPECT_EQ(step.data_point.arMethodName, "Step-heating - laser");
  EXPECT_EQ(step.data_point.arMethodName, ar_method(true, 5.0));
  EXPECT_EQ(step.data_point.mineralName, "sanidine");
  EXPECT_EQ(step.data_point.lithologyName, "ash-flow tuff");
  // The two types say what the measurements' own uncertainties are: one sigma.
  EXPECT_EQ(step.data_point.analyticalUncertaintyTypeName, "1 sigma");
  EXPECT_EQ(step.data_point.apparentAgeUncertaintyTypeName, "1 sigma");
  EXPECT_EQ(step.data_point.analyticalUncertaintyUnitName, "abs.");
  EXPECT_EQ(step.data_point.apparentAgeUncertaintyUnitName, "abs.");
  EXPECT_EQ(step.data_point.jvalueUncertaintyUnitName, "abs.");

  const Submission fusion = build_one(fusion_sample());
  EXPECT_EQ(fusion.data_point.arMethodName, "Total fusion - laser");
  EXPECT_EQ(fusion.data_point.mineralName, "biotite");
  EXPECT_FALSE(fusion.data_point.lithologyName.has_value());
  EXPECT_EQ(fusion.data_point.analysisDate, "2023-11-14");

  for (const r::MeanErrorKind kind : {r::MeanErrorKind::Sd, r::MeanErrorKind::Sem, r::MeanErrorKind::Msem}) {
    PublishOptions options;
    options.ages.mean_error = kind;
    // Whatever error kind the mean was asked for: that is said in the age
    // summary's description, and the type is the sigma level.
    const Submission s = build_one(step_sample(), options);
    EXPECT_EQ(s.data_point.analyticalUncertaintyTypeName, kOneSigma);
    EXPECT_EQ(s.data_point.apparentAgeUncertaintyTypeName, kOneSigma);
    EXPECT_EQ(s.age_summary.ageUncertaintyTypeName, kOneSigma);
    EXPECT_EQ(s.age_summary.calculatedAgeUncertaintyTypeName, kOneSigma);
    EXPECT_TRUE(contains(s.age_summary.description.value_or(""), "(the mean's " + std::string(r::to_string(kind)) + ")"))
        << s.age_summary.description.value_or("");
  }

  // A time is cut to its second, not rounded to the nearest: six tenths of
  // a second before midnight is still that day.
  auto late = std::make_shared<pp::Analysis>(*fusion_analysis(0));
  late->timestamp = 1'700'006'399.6;  // 2023-11-14 23:59:59.6 UTC
  irradiate_for_real(*late);
  const Submission before_midnight = build_one({late});
  EXPECT_EQ(before_midnight.measurements[0].analysisTime, "2023-11-14T23:59:59Z");
  EXPECT_EQ(before_midnight.data_point.analysisDate, "2023-11-14");

  // A furnace: the hottest step decides, and degrees are a temperature.
  const Submission furnace = build_one(with_edit_all(step_sample(), [](pp::Analysis& a) {
    a.extraction.value = 600.0 + 50.0 * a.increment;
    a.extraction.units = "C";
  }));
  EXPECT_EQ(furnace.data_point.arMethodName, "Step-heating - furnace");
  EXPECT_EQ(furnace.measurements[2].furnaceTemperature, 700.0);
  EXPECT_FALSE(furnace.measurements[2].laserPower.has_value());
  EXPECT_FALSE(furnace.measurements[2].laserPowerUnits.has_value());
  // Between a laser's watts and a furnace's degrees: not said.
  const Submission unclear =
      build_one(with_edit_all(step_sample(), [](pp::Analysis& a) { a.extraction.value = 120.0; }));
  EXPECT_EQ(unclear.data_point.arMethodName, "Step-heating - undefined");

  // The aliquot of the schedule, and one per fusion.
  ASSERT_EQ(step.aliquots.size(), 1u);
  EXPECT_EQ(step.aliquots[0].aliquotName, "20001-01");
  EXPECT_EQ(to_json(step.aliquots[0]).size(), 1u);
  ASSERT_EQ(fusion.aliquots.size(), 6u);
  EXPECT_EQ(fusion.aliquots[0].aliquotName, "20002-01");
  EXPECT_EQ(fusion.aliquots[5].aliquotName, "20002-06");
}

// ---------------------------------------------------------------- measurements

// Every number of a measurement against the reduction of the same analysis.
void expect_measurement(const Measurement& m, const pp::AnalysisPtr& analysis) {
  const pp::ReducedPtr reduced = pp::reduce_analysis(analysis, {});
  ASSERT_TRUE(reduced->arar && reduced->arar->ages) << reduced->reduction_error;
  const r::ArArResult& arar = *reduced->arar;
  const auto stage = [&](const char* isotope, pp::Stage which) {
    const auto v = reduced->stage(isotope, which);
    EXPECT_TRUE(v.has_value()) << isotope;
    return v.value_or(r::UFloat{});
  };

  // Stage::InterferenceCorrected: 40Ar less 40ArK, 39ArK, 37Ar decay
  // corrected, atmospheric 36Ar. 38Ar is not that stage (which removes
  // nothing from it): it is the corrected 38Ar less the K- and Ca-derived 38Ar.
  const r::UFloat c38 =
      arar.corrected[r::index(r::ArgonIsotope::Ar38)] - arar.f.interference.k38 - arar.f.interference.ca38;
  EXPECT_REL(m.ar40Corr, stage("Ar40", pp::Stage::InterferenceCorrected).nominal());
  EXPECT_REL(m.ar40CorrUncertainty, stage("Ar40", pp::Stage::InterferenceCorrected).std_dev());
  EXPECT_REL(m.ar39Corr, stage("Ar39", pp::Stage::InterferenceCorrected).nominal());
  EXPECT_REL(m.ar39CorrUncertainty, stage("Ar39", pp::Stage::InterferenceCorrected).std_dev());
  EXPECT_REL(m.ar38Corr, c38.nominal());
  EXPECT_REL(m.ar38CorrUncertainty, c38.std_dev());
  EXPECT_GT(arar.f.interference.k38.nominal(), 0.0);
  EXPECT_GT(arar.f.interference.ca38.nominal(), 0.0);
  EXPECT_LT(*m.ar38Corr, *m.ar38ExcInt);
  EXPECT_NE(*m.ar38CorrUncertainty, *m.ar38ExcIntUncertainty);
  // The atmospheric and the chlorine-derived 38Ar are still in it: they are
  // what is left of the 38Ar once the two interferences are gone.
  const r::UFloat left38 = arar.f.atmospheric.atm38 + arar.f.atmospheric.cl38;
  EXPECT_NEAR(*m.ar38Corr, left38.nominal(), std::abs(left38.nominal()) * 1e-9);
  EXPECT_REL(m.ar37Corr, stage("Ar37", pp::Stage::InterferenceCorrected).nominal());
  EXPECT_REL(m.ar37CorrUncertainty, stage("Ar37", pp::Stage::InterferenceCorrected).std_dev());
  EXPECT_REL(m.ar36Corr, stage("Ar36", pp::Stage::InterferenceCorrected).nominal());
  EXPECT_REL(m.ar36CorrUncertainty, stage("Ar36", pp::Stage::InterferenceCorrected).std_dev());
  // The same numbers by their names in the result.
  EXPECT_REL(m.ar39Corr, arar.f.interference.k39.nominal());
  EXPECT_REL(m.ar36Corr, arar.f.atmospheric.atm36.nominal());
  EXPECT_REL(m.ar40Corr, (arar.corrected[r::index(r::ArgonIsotope::Ar40)] - arar.f.k40).nominal());
  EXPECT_REL(m.ar37Corr, arar.corrected[r::index(r::ArgonIsotope::Ar37)].nominal());

  // Excluding the interference correction: Stage::DecayCorrected, which for
  // 40Ar, 38Ar and 36Ar is the discrimination stage and for 39Ar has the decay
  // too; 37Ar is the same stage with its decay taken out again ("mass
  // discrimination only"), which with no abundance sensitivity, as here, is
  // Stage::IcCorrected.
  const r::UFloat exc37 = arar.corrected[r::index(r::ArgonIsotope::Ar37)] / arar.decay.df37;
  EXPECT_REL(m.ar37ExcInt, exc37.nominal());
  EXPECT_REL(m.ar37ExcIntUncertainty, exc37.std_dev());
  EXPECT_REL(m.ar40ExcInt, stage("Ar40", pp::Stage::DecayCorrected).nominal());
  EXPECT_REL(m.ar40ExcIntUncertainty, stage("Ar40", pp::Stage::DecayCorrected).std_dev());
  EXPECT_REL(m.ar39ExcInt, stage("Ar39", pp::Stage::DecayCorrected).nominal());
  EXPECT_REL(m.ar39ExcIntUncertainty, stage("Ar39", pp::Stage::DecayCorrected).std_dev());
  EXPECT_REL(m.ar38ExcInt, stage("Ar38", pp::Stage::DecayCorrected).nominal());
  EXPECT_REL(m.ar38ExcIntUncertainty, stage("Ar38", pp::Stage::DecayCorrected).std_dev());
  EXPECT_REL(m.ar37ExcInt, stage("Ar37", pp::Stage::IcCorrected).nominal());
  EXPECT_REL(m.ar37ExcIntUncertainty, stage("Ar37", pp::Stage::IcCorrected).std_dev());
  EXPECT_REL(m.ar36ExcInt, stage("Ar36", pp::Stage::DecayCorrected).nominal());
  EXPECT_REL(m.ar36ExcIntUncertainty, stage("Ar36", pp::Stage::DecayCorrected).std_dev());
  EXPECT_REL(m.ar40ExcInt, stage("Ar40", pp::Stage::IcCorrected).nominal());
  EXPECT_REL(m.ar36ExcInt, stage("Ar36", pp::Stage::IcCorrected).nominal());

  // The fixture is such that the stages are different numbers.
  EXPECT_GT(*m.ar39ExcInt, 1.0005 * stage("Ar39", pp::Stage::IcCorrected).nominal());
  EXPECT_GT(stage("Ar37", pp::Stage::DecayCorrected).nominal(), 2.0 * *m.ar37ExcInt);
  EXPECT_LT(*m.ar40Corr, *m.ar40ExcInt);
  EXPECT_LT(*m.ar39Corr, *m.ar39ExcInt);
  EXPECT_LT(*m.ar36Corr, *m.ar36ExcInt);
  EXPECT_NE(*m.ar36ExcInt, stage("Ar36", pp::Stage::BlankCorrected).nominal());

  // Stage::Blank.
  EXPECT_REL(m.ar40blank, stage("Ar40", pp::Stage::Blank).nominal());
  EXPECT_REL(m.ar40blankUncertainty, stage("Ar40", pp::Stage::Blank).std_dev());
  EXPECT_REL(m.ar39blank, stage("Ar39", pp::Stage::Blank).nominal());
  EXPECT_REL(m.ar39blankUncertainty, stage("Ar39", pp::Stage::Blank).std_dev());
  EXPECT_REL(m.ar38blank, stage("Ar38", pp::Stage::Blank).nominal());
  EXPECT_REL(m.ar38blankUncertainty, stage("Ar38", pp::Stage::Blank).std_dev());
  EXPECT_REL(m.ar37blank, stage("Ar37", pp::Stage::Blank).nominal());
  EXPECT_REL(m.ar37blankUncertainty, stage("Ar37", pp::Stage::Blank).std_dev());
  EXPECT_REL(m.ar36blank, stage("Ar36", pp::Stage::Blank).nominal());
  EXPECT_REL(m.ar36blankUncertainty, stage("Ar36", pp::Stage::Blank).std_dev());
  EXPECT_EQ(m.ar40blank, 0.8);
  EXPECT_EQ(m.ar40blankUncertainty, 0.02);

  // The corrected isotopes over 39ArK, from ArArResult::f.interference_corrected
  // (the same variables, so the uncertainty is the correlated one).
  const auto& ic = arar.f.interference_corrected;
  const r::UFloat k39 = ic[r::index(r::ArgonIsotope::Ar39)];
  const r::UFloat r36 = ic[r::index(r::ArgonIsotope::Ar36)] / k39;
  const r::UFloat r37 = ic[r::index(r::ArgonIsotope::Ar37)] / k39;
  const r::UFloat r38 = c38 / k39;
  const r::UFloat r40 = ic[r::index(r::ArgonIsotope::Ar40)] / k39;
  EXPECT_REL(m.ratio36Arx39ArCorr, r36.nominal());
  EXPECT_REL(m.ratio36Arx39ArCorrUncertainty, r36.std_dev());
  EXPECT_REL(m.ratio37Arx39ArCorr, r37.nominal());
  EXPECT_REL(m.ratio37Arx39ArCorrUncertainty, r37.std_dev());
  EXPECT_REL(m.ratio38Arx39ArCorr, r38.nominal());
  EXPECT_REL(m.ratio38Arx39ArCorrUncertainty, r38.std_dev());
  EXPECT_REL(m.ratio40Arx39ArCorr, r40.nominal());
  EXPECT_REL(m.ratio40Arx39ArCorrUncertainty, r40.std_dev());
  EXPECT_REL(m.ratio40Arx39ArCorr, *m.ar40Corr / *m.ar39Corr);

  // ArArResult::f.f, 40Ar*/39ArK.
  ASSERT_TRUE(arar.f.f.has_value());
  EXPECT_REL(m.ratio40Arx39ArK, arar.f.f->nominal());
  EXPECT_REL(m.ratio40Arx39ArKUncertainty, arar.f.f->std_dev());
  // 40Ar* in percent of the interference-corrected 40Ar (40Ar* + trapped
  // 40Ar), which is not the reduction's yield (40Ar* over all the 40Ar, the
  // K-derived 40Ar included).
  const r::UFloat star = arar.f.rad40 / ic[r::index(r::ArgonIsotope::Ar40)] * 100.0;
  EXPECT_REL(m.radiogenicArgon, star.nominal());
  EXPECT_REL(m.radiogenicArgon, 100.0 * arar.f.rad40.nominal() / (arar.f.rad40 + arar.f.atm40).nominal());
  ASSERT_TRUE(arar.f.radiogenic_yield.has_value());
  EXPECT_GT(*m.radiogenicArgon, arar.f.radiogenic_yield->nominal() * (1.0 + 1e-6));
  EXPECT_GT(*m.radiogenicArgon, 10.0);
  EXPECT_LT(*m.radiogenicArgon, 100.0);

  // ArArResult::cak, kca, clk.
  ASSERT_TRUE(arar.cak && arar.kca && arar.clk);
  EXPECT_REL(m.caK, arar.cak->nominal());
  EXPECT_REL(m.caKUncertainty, arar.cak->std_dev());
  EXPECT_REL(m.kCa, arar.kca->nominal());
  EXPECT_REL(m.kCaUncertainty, arar.kca->std_dev());
  EXPECT_REL(m.clK, arar.clk->nominal());
  EXPECT_REL(m.clKUncertainty, arar.clk->std_dev());

  // ReducedAnalysis::j: J with its uncertainty.
  ASSERT_TRUE(reduced->j.has_value());
  EXPECT_REL(m.jValue, reduced->j->nominal());
  EXPECT_REL(m.jValueUncertainty, reduced->j->std_dev());
  EXPECT_EQ(m.jValue, 0.0015);
  EXPECT_EQ(m.jValueUncertainty, 3e-6);
  EXPECT_EQ(m.jvalueUncertaintyTypeName, "1 sigma");

  // ArArResult::ages->age: the analytical age (Ma), without the J uncertainty.
  EXPECT_REL(m.apparentAge, arar.ages->age.nominal());
  EXPECT_REL(m.apparentAgeUncertainty, arar.ages->age.std_dev());
  EXPECT_LT(*m.apparentAgeUncertainty, arar.ages->age_w_j_err.std_dev());
  EXPECT_EQ(m.apparentAgeUncertainityTypeName, "1 sigma");

  EXPECT_EQ(m.analysisUnits, "fA");
}

TEST(Mapping, MeasurementValues) {
  const AnalysisList steps = step_sample();
  const Submission step = build_one(steps);
  ASSERT_EQ(step.measurements.size(), 10u);
  const Measurement& d = step.measurements[3];
  expect_measurement(d, steps[3]);
  EXPECT_EQ(d.aliquotName, "20001-01");
  EXPECT_EQ(d.stepNumber, 4.0);
  EXPECT_EQ(d.analysisTime, "2023-11-14T23:43:20Z");
  EXPECT_EQ(d.laserPower, 2.0);
  EXPECT_EQ(d.laserPowerUnits, "W");
  EXPECT_FALSE(d.furnaceTemperature.has_value());
  EXPECT_GT(*d.radiogenicArgon, 90.0);  // a plateau step: percent, not a fraction
  for (std::size_t i = 0; i < steps.size(); ++i) expect_measurement(step.measurements[i], steps[i]);

  // The share of the schedule's 39ArK, in percent: ArArResult::f.interference.k39
  // of the step over the sum of the steps, and the running sum.
  std::vector<double> k39;
  double total = 0.0;
  for (const auto& a : steps) {
    k39.push_back(pp::reduce_analysis(a, {})->arar->f.interference.k39.nominal());
    total += k39.back();
  }
  double sum = 0.0;
  for (std::size_t i = 0; i < steps.size(); ++i) {
    sum += k39[i];
    EXPECT_REL(step.measurements[i].ar39KReleased, k39[i] / total * 100.0);
    EXPECT_REL(step.measurements[i].cumulative39ArK, sum / total * 100.0);
  }
  EXPECT_REL(step.measurements.back().cumulative39ArK, 100.0);
  EXPECT_GT(*step.measurements[4].ar39KReleased, 20.0);  // the largest step: a percentage, not a fraction or fA
  EXPECT_LT(*step.measurements[4].ar39KReleased, 25.0);

  const AnalysisList fusions = fusion_sample();
  const Submission fusion = build_one(fusions);
  ASSERT_EQ(fusion.measurements.size(), 6u);
  const Measurement& third = fusion.measurements[2];
  expect_measurement(third, fusions[2]);
  EXPECT_EQ(third.aliquotName, "20002-03");
  EXPECT_FALSE(third.stepNumber.has_value());
  EXPECT_FALSE(third.ar39KReleased.has_value());
  EXPECT_FALSE(third.cumulative39ArK.has_value());
  // The reported age is the weighted mean of the six: each is in it, and
  // there is no plateau or isochron to be in.
  EXPECT_FALSE(third.plateauStepName.has_value());
  EXPECT_FALSE(third.inverseIsoStepName.has_value());
  EXPECT_EQ(third.weightedMeanStepName, "Yes");
  EXPECT_EQ(third.analysisTime, "2023-11-15T00:13:20Z");
  EXPECT_EQ(third.laserPower, 4.0);
  EXPECT_EQ(third.laserPowerUnits, "W");
}

// 37Ar "corrected for mass discrimination only" has what the other four have
// at that stage, the abundance sensitivity among it, and no decay.
TEST(Mapping, Ar37ExcIntIsTheOtherFoursStageWithoutDecay) {
  const AnalysisList steps = with_edit_all(step_sample(), [](pp::Analysis& a) {
    a.context.constants = r::constants_preset(r::ConstantsPreset::Default);
    a.context.constants->abundance_sensitivity = 2e-3;
  });
  const Submission s = build_one(steps);
  const Measurement& m = s.measurements[3];
  const pp::ReducedPtr reduced = pp::reduce_analysis(steps[3], {});
  const r::ArArResult& arar = *reduced->arar;
  ASSERT_GT(arar.decay.df37, 2.0);
  const r::UFloat exc37 = arar.corrected[r::index(r::ArgonIsotope::Ar37)] / arar.decay.df37;
  EXPECT_REL(m.ar37ExcInt, exc37.nominal());
  EXPECT_REL(m.ar37ExcIntUncertainty, exc37.std_dev());
  // Not the stage before the abundance sensitivity, and not the decayed one.
  const double before = reduced->stage("Ar37", pp::Stage::IcCorrected)->nominal();
  EXPECT_GT(std::abs(*m.ar37ExcInt - before), 1e-4 * before);
  EXPECT_REL(m.ar37Corr, *m.ar37ExcInt * arar.decay.df37);
  // The other four are the stage as it is.
  EXPECT_REL(m.ar40ExcInt, arar.corrected[r::index(r::ArgonIsotope::Ar40)].nominal());
  EXPECT_REL(m.ar39ExcInt, arar.corrected[r::index(r::ArgonIsotope::Ar39)].nominal());
  EXPECT_REL(m.ar38ExcInt, arar.corrected[r::index(r::ArgonIsotope::Ar38)].nominal());
  EXPECT_REL(m.ar36ExcInt, arar.corrected[r::index(r::ArgonIsotope::Ar36)].nominal());
  EXPECT_NE(*m.ar36ExcInt, reduced->stage("Ar36", pp::Stage::IcCorrected)->nominal());
}

// Ages are in Ma whatever unit the reduction counts in.
TEST(Mapping, AgesAreInMa) {
  const auto in_ka = [](pp::Analysis& a) {
    a.context.constants = r::constants_preset(r::ConstantsPreset::Default);
    a.context.constants->age_units = r::AgeUnits::ka;
  };
  const AnalysisList steps = with_edit_all(step_sample(), in_ka);
  const Submission ka = build_one(steps);
  const Submission ma = build_one(step_sample());

  const pp::ReducedPtr reduced = pp::reduce_analysis(steps[4], {});
  const r::UFloat& age = reduced->arar->ages->age;
  EXPECT_GT(age.nominal(), 20'000.0);  // thousands of years
  EXPECT_REL(ka.measurements[4].apparentAge, age.nominal() * 1e-3);
  EXPECT_REL(ka.measurements[4].apparentAgeUncertainty, age.std_dev() * 1e-3);
  EXPECT_NEAR(*ka.measurements[4].apparentAge, *ma.measurements[4].apparentAge, 1e-9);
  EXPECT_NEAR(*ka.age_summary.age, *ma.age_summary.age, 1e-9);
  EXPECT_NEAR(*ka.age_summary.ageUncertainty, *ma.age_summary.ageUncertainty, 1e-9);
  EXPECT_GT(*ka.age_summary.age, 20.0);
  EXPECT_LT(*ka.age_summary.age, 40.0);

  // Two units in one sample cannot be averaged.
  const std::string what = refused(with_edit(step_sample(), 6, in_ka));
  EXPECT_TRUE(contains(what, "\"S-STEP\"") && contains(what, "age unit") && contains(what, "Ma (20001-01A)") &&
              contains(what, "ka (20001-01G)"))
      << what;
}

// ---------------------------------------------------------------- the reported age

TEST(Mapping, PlateauStepsAreFlagged) {
  const AnalysisList steps = step_sample();
  const PublishOptions options;

  // What the reduction says the plateau is.
  const pp::Dataset d = dataset_of(steps);
  const auto found =
      pp::group_plateau(items_of(d), options.ages.plateau, options.ages.plateau_weighting, options.ages.mean_error);
  ASSERT_TRUE(found.has_value());
  std::vector<std::string> expected;
  for (const pp::DatasetItem* step : found->used) expected.push_back(step->analysis->analysis->step());
  EXPECT_EQ(expected, (std::vector<std::string>{"C", "D", "E", "F", "G", "H"}));

  const Submission s = build_one(steps);
  EXPECT_EQ(s.age_summary.ageCalcTypeName, age_type_name(AgeKind::Plateau));
  EXPECT_EQ(plateau_flags(s), expected);
  EXPECT_EQ(s.age_summary.nAnalysisGrouped, 6);
  // Every step says whether it is in the reported plateau, and nothing of the
  // ages that are not reported.
  EXPECT_EQ(flags(s, &Measurement::plateauStepName), "NNYYYYYYNN");
  EXPECT_EQ(flags(s, &Measurement::weightedMeanStepName), "----------");
  EXPECT_EQ(flags(s, &Measurement::inverseIsoStepName), "----------");

  // Another age reported: the column of that age, and no step is a step of a
  // plateau that is not reported.
  PublishOptions other;
  other.samples["S-STEP"].preferred_age = PreferredAge::WeightedMean;
  const Submission mean = build_one(steps, other);
  EXPECT_EQ(flags(mean, &Measurement::plateauStepName), "----------");
  EXPECT_EQ(flags(mean, &Measurement::weightedMeanStepName), "YYYYYYYYYY");
  EXPECT_EQ(flags(mean, &Measurement::inverseIsoStepName), "----------");
  other.samples["S-STEP"].preferred_age = PreferredAge::Isochron;
  const Submission isochron = build_one(steps, other);
  EXPECT_EQ(flags(isochron, &Measurement::plateauStepName), "----------");
  EXPECT_EQ(flags(isochron, &Measurement::weightedMeanStepName), "----------");
  EXPECT_EQ(flags(isochron, &Measurement::inverseIsoStepName), "YYYYYYYYYY");
  EXPECT_EQ(isochron.age_summary.nAnalysisGrouped, 10);
  // The total gas age has no column.
  other.samples["S-STEP"].preferred_age = PreferredAge::Integrated;
  const Submission total = build_one(steps, other);
  EXPECT_EQ(total.age_summary.ageCalcTypeName, age_type_name(AgeKind::Integrated));
  for (const auto column :
       {&Measurement::plateauStepName, &Measurement::weightedMeanStepName, &Measurement::inverseIsoStepName}) {
    EXPECT_EQ(flags(total, column), "----------");
  }

  // No plateau under the criteria: none either.
  PublishOptions strict;
  strict.ages.plateau.gas_fraction = 99.5;
  EXPECT_EQ(flags(build_one(steps, strict), &Measurement::plateauStepName), "----------");

  // A step without an age is not a step of the plateau that is left.
  const AnalysisList holed = with_edit(steps, 4, [](pp::Analysis& a) { a.context.flux.reset(); });
  const Submission without = build_one(holed);
  EXPECT_EQ(without.measurements.size(), 9u);
  const pp::Dataset hd = dataset_of(holed);
  pp::GroupItems dated;
  for (const pp::DatasetItem* item : items_of(hd)) {
    if (pp::has_ages(*item)) dated.push_back(item);
  }
  const auto left =
      pp::group_plateau(dated, options.ages.plateau, options.ages.plateau_weighting, options.ages.mean_error);
  std::vector<std::string> left_steps;
  if (left) {
    for (const pp::DatasetItem* step : left->used) left_steps.push_back(step->analysis->analysis->step());
  }
  EXPECT_EQ(plateau_flags(without), left_steps);
  EXPECT_EQ(std::count(left_steps.begin(), left_steps.end(), "E"), 0);
  if (left) {
    EXPECT_EQ(flags(without, &Measurement::plateauStepName).find('-'), std::string::npos);
  }
}

TEST(Mapping, PreferredAgeRule) {
  const AnalysisList steps = step_sample();
  const AnalysisList fusions = fusion_sample();
  const PublishOptions defaults;
  const pp::Dataset sd = dataset_of(steps);
  const pp::GroupItems step_items = items_of(sd);
  const pp::Dataset fd = dataset_of(fusions);
  const pp::GroupItems fusion_items = items_of(fd);
  const double step_j = pp::j_relative_error(step_items);
  EXPECT_NEAR(step_j, 0.002, 1e-15);

  // A step-heated sample with a plateau reports it.
  {
    const auto found = pp::group_plateau(step_items, defaults.ages.plateau, defaults.ages.plateau_weighting,
                                         defaults.ages.mean_error);
    ASSERT_TRUE(found.has_value());
    const r::Mean& mean = found->mean.mean;
    const Submission s = build_one(steps);
    const AgeSummary& a = s.age_summary;
    EXPECT_EQ(a.ageCalcTypeName, "Plateau Age");
    EXPECT_REL(a.age, mean.value);
    EXPECT_REL(a.ageUncertainty, pp::with_external_error(mean.value, mean.error, step_j));
    EXPECT_GT(*a.ageUncertainty, mean.error);
    EXPECT_REL(a.mswd, mean.mswd);
    EXPECT_REL(a.agepvalue, r::mswd_probability(mean.mswd, 5));
    EXPECT_EQ(a.nAnalysisGrouped, 6);
    EXPECT_EQ(a.preferredAge, true);
    EXPECT_EQ(a.dataInterpretationToolName, "Pychron");
    EXPECT_EQ(a.interpretationName, "Unknown");
    EXPECT_EQ(a.ageUncertaintyTypeName, "1 sigma");
    EXPECT_EQ(a.calculatedAgeUncertaintyTypeName, "1 sigma");
    EXPECT_EQ(a.calculatedAgeUncertaintyTypeUnitsName, "abs.");
    const std::string note = note_with(s, "preferred age");
    EXPECT_TRUE(contains(note, "Plateau Age") && contains(note, "steps C-H") && contains(note, "has a plateau")) << note;
    EXPECT_TRUE(contains(note_with(s, "reported age's uncertainty"), "with the J uncertainty"));

    // And it is the report's plateau, cell for cell.
    const pp::Report report = report_of(steps);
    EXPECT_REL(a.age, cell(report, "plateau age (Ma)"));
    EXPECT_REL(a.ageUncertainty, cell(report, "plateau age ±(1s) with J"));
    EXPECT_REL(a.mswd, cell(report, "plateau MSWD"));
    EXPECT_REL(a.agepvalue, cell(report, "plateau p"));
    EXPECT_EQ(static_cast<double>(*a.nAnalysisGrouped), summary_cell(report, "plateau n"));

    // Without the J uncertainty: the report's analytical column.
    PublishOptions analytical;
    analytical.age_includes_j_error = false;
    const Submission plain = build_one(steps, analytical);
    EXPECT_REL(plain.age_summary.age, mean.value);
    EXPECT_REL(plain.age_summary.ageUncertainty, mean.error);
    EXPECT_REL(plain.age_summary.ageUncertainty, cell(report, "plateau age ±(1s) analytical"));
    EXPECT_TRUE(contains(note_with(plain, "reported age's uncertainty"), "without the J uncertainty"));
  }

  // Criteria no run of steps meets: the integrated age, and a note saying why.
  {
    PublishOptions strict;
    strict.ages.plateau.nsteps = 9;
    ASSERT_FALSE(pp::group_plateau(step_items, strict.ages.plateau, strict.ages.plateau_weighting,
                                   strict.ages.mean_error)
                     .has_value());
    const auto plain = pp::integrated_age(step_items, false);
    const auto with_j = pp::integrated_age(step_items, true);
    ASSERT_TRUE(plain && with_j);
    const Submission s = build_one(steps, strict);
    const AgeSummary& a = s.age_summary;
    EXPECT_EQ(a.ageCalcTypeName, "Total Gas Age");
    EXPECT_REL(a.age, plain->nominal());
    EXPECT_REL(a.ageUncertainty, with_j->std_dev());
    EXPECT_FALSE(a.mswd.has_value());
    EXPECT_FALSE(a.agepvalue.has_value());
    EXPECT_EQ(a.nAnalysisGrouped, 10);
    EXPECT_EQ(a.ageUncertaintyTypeName, "1 sigma");
    const std::string note = note_with(s, "preferred age");
    EXPECT_TRUE(contains(note, "Total Gas Age") && contains(note, "no plateau") && contains(note, "at least 9 steps"))
        << note;

    const pp::Report report = report_of(steps, strict);
    EXPECT_REL(a.age, cell(report, "integrated age (Ma)"));
    EXPECT_REL(a.ageUncertainty, cell(report, "integrated age ±(1s) with J"));
    EXPECT_FALSE(summary_cell(report, "plateau age (Ma)").has_value());

    strict.age_includes_j_error = false;
    EXPECT_REL(build_one(steps, strict).age_summary.ageUncertainty, plain->std_dev());
  }

  // Fusion analyses: the weighted mean.
  {
    const auto mean = pp::group_weighted_mean(fusion_items, defaults.ages.mean_error);
    ASSERT_TRUE(mean.has_value());
    const Submission s = build_one(fusions);
    const AgeSummary& a = s.age_summary;
    EXPECT_EQ(a.ageCalcTypeName, "Weighted Mean Age");
    EXPECT_REL(a.age, mean->value);
    EXPECT_REL(a.ageUncertainty, pp::with_external_error(mean->value, mean->error, pp::j_relative_error(fusion_items)));
    EXPECT_REL(a.mswd, mean->mswd);
    EXPECT_REL(a.agepvalue, r::mswd_probability(mean->mswd, 5));
    EXPECT_EQ(a.nAnalysisGrouped, 6);
    EXPECT_TRUE(contains(note_with(s, "preferred age"), "fusions")) << note_with(s, "preferred age");

    const pp::Report report = report_of(fusions);
    EXPECT_REL(a.age, cell(report, "weighted mean age (Ma)"));
    EXPECT_REL(a.ageUncertainty, cell(report, "weighted mean age ±(1s) with J"));
    EXPECT_REL(a.mswd, cell(report, "weighted mean MSWD"));
    EXPECT_REL(a.agepvalue, cell(report, "weighted mean p"));
  }

  // An age chosen in the options wins over the rule.
  {
    PublishOptions options;
    options.samples["S-STEP"].preferred_age = PreferredAge::WeightedMean;
    const auto mean = pp::group_weighted_mean(step_items, options.ages.mean_error);
    ASSERT_TRUE(mean.has_value());
    const Submission wm = build_one(steps, options);
    EXPECT_EQ(wm.age_summary.ageCalcTypeName, "Weighted Mean Age");
    EXPECT_REL(wm.age_summary.age, mean->value);
    EXPECT_REL(wm.age_summary.ageUncertainty, pp::with_external_error(mean->value, mean->error, step_j));
    EXPECT_REL(wm.age_summary.mswd, mean->mswd);
    EXPECT_EQ(wm.age_summary.nAnalysisGrouped, 10);
    EXPECT_TRUE(plateau_flags(wm).empty());
    EXPECT_TRUE(contains(note_with(wm, "preferred age"), "chosen in the options"));

    options.samples["S-STEP"].preferred_age = PreferredAge::Isochron;
    const auto points = pp::isochron_points(step_items);
    const auto plain = pp::isochron_age(points, options.ages.york, false, false);
    const auto with_j = pp::isochron_age(points, options.ages.york, false, true);
    ASSERT_TRUE(plain && with_j && plain->age && with_j->age);
    const Submission iso = build_one(steps, options);
    EXPECT_EQ(iso.age_summary.ageCalcTypeName, "Inverse Isochron Age");
    EXPECT_REL(iso.age_summary.age, plain->age->nominal());
    EXPECT_REL(iso.age_summary.ageUncertainty, with_j->age->std_dev());
    EXPECT_REL(iso.age_summary.mswd, plain->fit.mswd);
    EXPECT_REL(iso.age_summary.agepvalue, plain->fit.probability);
    EXPECT_EQ(iso.age_summary.nAnalysisGrouped, 10);
    EXPECT_EQ(iso.age_summary.ageUncertaintyTypeName, "1 sigma");
    const pp::Report report = report_of(steps);
    EXPECT_REL(iso.age_summary.age, cell(report, "isochron age (Ma)"));
    EXPECT_REL(iso.age_summary.ageUncertainty, cell(report, "isochron age ±(1s) with J"));
    EXPECT_REL(iso.age_summary.mswd, cell(report, "isochron MSWD"));

    options.samples["S-STEP"].preferred_age = PreferredAge::Plateau;
    EXPECT_EQ(build_one(steps, options).age_summary.ageCalcTypeName, "Plateau Age");

    // The options of another sample are not this one's.
    PublishOptions other;
    other.samples["S-FUS"].preferred_age = PreferredAge::Integrated;
    const auto built = build(both_samples(), other);
    ASSERT_EQ(built.size(), 2u);
    EXPECT_EQ(built[0].age_summary.ageCalcTypeName, "Total Gas Age");
    EXPECT_REL(built[0].age_summary.age, pp::integrated_age(fusion_items, false)->nominal());
    EXPECT_EQ(built[1].age_summary.ageCalcTypeName, "Plateau Age");
  }

  // An age asked for that does not exist is not replaced by another.
  {
    PublishOptions options;
    options.samples["S-FUS"].preferred_age = PreferredAge::Plateau;
    const std::string what = refused(fusions, options);
    EXPECT_TRUE(contains(what, "\"S-FUS\"") && contains(what, "Plateau Age") && contains(what, "cannot be computed"))
        << what;

    PublishOptions strict;
    strict.ages.plateau.nsteps = 9;
    strict.samples["S-STEP"].preferred_age = PreferredAge::Plateau;
    EXPECT_TRUE(contains(refused(steps, strict), "at least 9 steps"));

    PublishOptions isochron;
    isochron.samples["S-FUS"].preferred_age = PreferredAge::Isochron;
    const AnalysisList two = {fusions[0], fusions[1]};
    EXPECT_TRUE(contains(refused(two, isochron), "at least 3 points"));
  }
}

// One fusion is its own weighted mean, with no MSWD to speak of.
TEST(Mapping, ASingleAnalysisHasNoMswd) {
  const AnalysisList one = {fusion_analysis(0)};
  const Submission s = build_one(one);
  const pp::ReducedPtr reduced = pp::reduce_analysis(one[0], {});
  EXPECT_EQ(s.age_summary.ageCalcTypeName, "Weighted Mean Age");
  EXPECT_REL(s.age_summary.age, reduced->arar->ages->age.nominal());
  EXPECT_EQ(s.age_summary.nAnalysisGrouped, 1);
  EXPECT_FALSE(s.age_summary.mswd.has_value());
  EXPECT_FALSE(s.age_summary.agepvalue.has_value());
}

// Several aliquots, and steps beside fusions.
TEST(Mapping, AliquotsAndMixedSamples) {
  // A fusion of the stepped sample, run before the schedule.
  auto fusion = std::make_shared<pp::Analysis>(*fusion_analysis(0));
  fusion->identifier = "20001";
  fusion->aliquot = 2;
  fusion->runid = pp::make_runid("20001", 2, -1);
  fusion->sample = "S-STEP";
  fusion->material = "sanidine";
  fusion->timestamp = step_analysis(0)->timestamp - 7200.0;
  irradiate_for_real(*fusion);
  AnalysisList mixed = step_sample();
  mixed.push_back(fusion);

  const Submission s = build_one(mixed);
  ASSERT_EQ(s.aliquots.size(), 2u);
  EXPECT_EQ(s.aliquots[0].aliquotName, "20001-02");  // first to appear
  EXPECT_EQ(s.aliquots[1].aliquotName, "20001-01");
  ASSERT_EQ(s.measurements.size(), 11u);
  EXPECT_EQ(s.measurements[0].aliquotName, "20001-02");
  EXPECT_FALSE(s.measurements[0].stepNumber.has_value());
  EXPECT_FALSE(s.measurements[0].cumulative39ArK.has_value());
  EXPECT_EQ(s.measurements[0].plateauStepName, "No");  // a fusion is no step of the plateau reported
  EXPECT_REL(s.measurements[10].cumulative39ArK, 100.0);
  EXPECT_EQ(s.data_point.arMethodName, "Step-heating - laser");
  EXPECT_EQ(s.data_point.analysisDate, "2023-11-14");
  EXPECT_NE(note_with(s, "both heating steps and fusion analyses"), "");
  // The plateau is of the schedule: the same as without the fusion.
  const Submission alone = build_one(step_sample());
  EXPECT_EQ(s.age_summary.ageCalcTypeName, "Plateau Age");
  EXPECT_EQ(s.age_summary.age, alone.age_summary.age);
  EXPECT_EQ(plateau_flags(s), plateau_flags(alone));

  // Two heated aliquots: each releases its own hundred percent, and there is
  // no one schedule to find a plateau in.
  AnalysisList two = step_sample();
  for (int i = 0; i < 4; ++i) {
    auto step = std::make_shared<pp::Analysis>(*step_analysis(i + 2));
    step->aliquot = 2;
    step->increment = i;
    step->runid = pp::make_runid("20001", 2, i);
    step->uuid += "-second";
    step->timestamp += 86'400.0;
    irradiate_for_real(*step);
    two.push_back(step);
  }
  // Two heating experiments have no one summary age that goes without
  // saying: the lab chooses, and is told between what.
  EXPECT_EQ(refused(two),
            "earthbank: sample \"S-STEP\": it has 2 step-heated aliquots (20001-01, 20001-02), and which summary "
            "age to report for them has to be chosen: set the sample's `preferred_age` setting to weighted_mean, "
            "integrated or isochron (each pools the aliquots), or publish the aliquots as separate selections");

  PublishOptions pooled;
  pooled.samples["S-STEP"].preferred_age = PreferredAge::WeightedMean;
  const Submission both = build_one(two, pooled);
  ASSERT_EQ(both.aliquots.size(), 2u);
  ASSERT_EQ(both.measurements.size(), 14u);
  EXPECT_REL(both.measurements[9].cumulative39ArK, 100.0);
  EXPECT_EQ(both.measurements[10].stepNumber, 1.0);
  EXPECT_LT(*both.measurements[10].cumulative39ArK, 100.0);
  EXPECT_REL(both.measurements[10].cumulative39ArK, *both.measurements[10].ar39KReleased);
  EXPECT_REL(both.measurements[13].cumulative39ArK, 100.0);
  EXPECT_EQ(both.age_summary.ageCalcTypeName, "Weighted Mean Age");
  EXPECT_EQ(both.age_summary.nAnalysisGrouped, 14);
  const pp::Dataset td = dataset_of(two);
  const auto mean = pp::group_weighted_mean(items_of(td), pooled.ages.mean_error);
  ASSERT_TRUE(mean.has_value());
  EXPECT_REL(both.age_summary.age, mean->value);
  EXPECT_TRUE(contains(note_with(both, "preferred age"), "chosen in the options") &&
              contains(note_with(both, "preferred age"), "the age pools 2 step-heated aliquots (20001-01, 20001-02)"))
      << note_with(both, "preferred age");
  EXPECT_TRUE(contains(both.age_summary.description.value_or(""), "Weighted Mean Age of 14 of 14 analyses, pooling 2 step-heated aliquots; uncertainty 1 sigma"))
      << both.age_summary.description.value_or("");
  EXPECT_TRUE(plateau_flags(both).empty());

  pooled.samples["S-STEP"].preferred_age = PreferredAge::Integrated;
  const Submission gas = build_one(two, pooled);
  EXPECT_EQ(gas.age_summary.ageCalcTypeName, "Total Gas Age");
  EXPECT_EQ(gas.age_summary.nAnalysisGrouped, 14);
  EXPECT_REL(gas.age_summary.age, pp::integrated_age(items_of(td), false)->nominal());
  EXPECT_TRUE(contains(note_with(gas, "preferred age"), "the age pools 2 step-heated aliquots"));

  // An isochron over the aliquots, when the lab asks for it: the fit of all
  // their steps, and the record says it pools them.
  pooled.samples["S-STEP"].preferred_age = PreferredAge::Isochron;
  const auto points = pp::isochron_points(items_of(td));
  const auto fit = pp::isochron_age(points, pooled.ages.york, false, false);
  const auto fit_with_j = pp::isochron_age(points, pooled.ages.york, false, true);
  ASSERT_TRUE(fit && fit_with_j && fit->age && fit_with_j->age);
  const Submission iso = build_one(two, pooled);
  EXPECT_EQ(iso.age_summary.ageCalcTypeName, "Inverse Isochron Age");
  EXPECT_EQ(iso.age_summary.nAnalysisGrouped, 14);
  EXPECT_REL(iso.age_summary.age, fit->age->nominal());
  EXPECT_REL(iso.age_summary.ageUncertainty, fit_with_j->age->std_dev());
  EXPECT_REL(iso.age_summary.mswd, fit->fit.mswd);
  EXPECT_EQ(flags(iso, &Measurement::inverseIsoStepName), std::string(14, 'Y'));
  EXPECT_TRUE(contains(note_with(iso, "preferred age"), "chosen in the options") &&
              contains(note_with(iso, "preferred age"), "the age pools 2 step-heated aliquots (20001-01, 20001-02)"))
      << note_with(iso, "preferred age");
  EXPECT_TRUE(contains(iso.age_summary.description.value_or(""),
                       "Inverse Isochron Age of 14 of 14 analyses, pooling 2 step-heated aliquots (trapped 40Ar/36Ar "))
      << iso.age_summary.description.value_or("");

  // A plateau is of one heating experiment: refused, with the three that are not.
  pooled.samples["S-STEP"].preferred_age = PreferredAge::Plateau;
  EXPECT_EQ(refused(two, pooled),
            "earthbank: sample \"S-STEP\": the Plateau Age asked for cannot be computed: 2 aliquots were step "
            "heated (20001-01, 20001-02), and a plateau is of one heating experiment: set the sample's "
            "`preferred_age` setting to weighted_mean, integrated or isochron (each pools the aliquots), or publish "
            "the aliquots as separate selections");
}

// ---------------------------------------------------------------- vocabulary

TEST(Mapping, AgeCalcNamesComeFromTheConstantsUsed) {
  // The Default preset: lambda_b + lambda_e of 5.543e-10 /a and 298.56.
  const Submission by_default = build_one(step_sample());
  const r::ReductionConstants preset = r::constants_preset(r::ConstantsPreset::Default);
  EXPECT_EQ(by_default.age_calc.decayConstantName, decay_constant_name(r::lambda_k(preset).value));
  EXPECT_EQ(by_default.age_calc.airRatioName, air_ratio_name(preset.atm4036.value));
  EXPECT_EQ(by_default.age_calc.decayConstantName, "Steiger and Jager, 1977");
  EXPECT_EQ(by_default.age_calc.airRatioName, "Lee et al. 2006");
  EXPECT_EQ(by_default.age_calc.fluxMonitorName, flux_monitor_query("FC-2", 28.201));
  EXPECT_EQ(by_default.age_calc.fluxMonitorName, "FC-2 28.201 Ma");
  EXPECT_EQ(to_json(by_default.age_calc).size(), 3u);

  // The preset the reduction was asked for.
  PublishOptions legacy;
  legacy.reduction.preset = r::ConstantsPreset::Legacy;
  const Submission by_legacy = build_one(step_sample(), legacy);
  EXPECT_EQ(by_legacy.age_calc.decayConstantName, "Steiger and Jager, 1977");
  EXPECT_EQ(by_legacy.age_calc.airRatioName, "Nier 1950");

  // The constants an analysis carries win over the preset, and a decay
  // constant on the flux over those.
  const Submission own = build_one(with_edit_all(step_sample(), [](pp::Analysis& a) {
    a.context.constants = r::constants_preset(r::ConstantsPreset::LegacyPreferences);
    a.context.flux->lambda_k_total = r::Measured{5.463e-10, 1.07e-12};
  }));
  EXPECT_EQ(own.age_calc.decayConstantName, "Min et al. 2000");
  EXPECT_EQ(own.age_calc.airRatioName, "Nier 1950");
  // An override of exactly 0 +- 0 is no override.
  const Submission zero = build_one(with_edit_all(
      step_sample(), [](pp::Analysis& a) { a.context.flux->lambda_k_total = r::Measured{0.0, 0.0}; }));
  EXPECT_EQ(zero.age_calc.decayConstantName, "Steiger and Jager, 1977");

  // A value EarthBank has no name for: no name, and a note.
  const AnalysisList odd = with_edit_all(step_sample(), [](pp::Analysis& a) {
    a.context.constants = r::constants_preset(r::ConstantsPreset::Default);
    a.context.constants->atm4036 = {300.0, 0.3};
    a.context.constants->lambda_e = {6.0e-11, 0.0};
  });
  const Submission unnamed = build_one(odd);
  EXPECT_FALSE(unnamed.age_calc.airRatioName.has_value());
  EXPECT_FALSE(unnamed.age_calc.decayConstantName.has_value());
  EXPECT_TRUE(contains(note_with(unnamed, "airRatioName"), "300")) << note_with(unnamed, "airRatioName");
  EXPECT_TRUE(contains(note_with(unnamed, "decayConstantName"), "5.562e-10 /a")) << note_with(unnamed, "decayConstantName");
  // Each says which setting names it.
  EXPECT_TRUE(note_with(unnamed, "airRatioName")
                  .ends_with("is not one EarthBank names; airRatioName was left out. Give EarthBank's name for it, "
                             "or a citation, as the `air_ratio` setting"))
      << note_with(unnamed, "airRatioName");
  EXPECT_TRUE(note_with(unnamed, "decayConstantName")
                  .ends_with("is not one EarthBank names; decayConstantName was left out. Give EarthBank's name for "
                             "it, or a citation, as the `decay_constant` setting"))
      << note_with(unnamed, "decayConstantName");
  for (const std::string& note : unnamed.notes) EXPECT_FALSE(contains(note, "in the options")) << note;
  // The options name it.
  PublishOptions named;
  named.air_ratio = "Lee et al. 2006";
  named.decay_constant = "Carter et al. 2025";
  const Submission with_names = build_one(odd, named);
  EXPECT_EQ(with_names.age_calc.airRatioName, "Lee et al. 2006");
  EXPECT_EQ(with_names.age_calc.decayConstantName, "Carter et al. 2025");
  EXPECT_EQ(note_with(with_names, "airRatioName"), "");
  EXPECT_EQ(note_with(with_names, "decayConstantName"), "");

  // No monitor on the analyses: no name, and a note.
  const Submission no_monitor = build_one(with_edit_all(step_sample(), [](pp::Analysis& a) { a.monitor = {}; }));
  EXPECT_FALSE(no_monitor.age_calc.fluxMonitorName.has_value());
  EXPECT_NE(note_with(no_monitor, "fluxMonitorName"), "");
  const Submission name_only =
      build_one(with_edit_all(step_sample(), [](pp::Analysis& a) { a.monitor.age.reset(); }));
  EXPECT_EQ(name_only.age_calc.fluxMonitorName, "FC-2");
}

TEST(Mapping, OverridesAreNormalized) {
  PublishOptions options;
  options.decay_constant = "Min (2008)";
  options.air_ratio = "Nier, 1950";
  SampleOptions& so = options.samples["S-STEP"];
  so.interpretation = "T (cooling age)";
  so.flux_monitor = "Alder Creek sanidine 1.1848 Ma";
  so.datapoint_name = "S-STEP plateau";
  const auto built = build(both_samples(), options);
  ASSERT_EQ(built.size(), 2u);
  const Submission& fusion = built[0];
  const Submission& step = built[1];

  // The citations as the service's names, for every sample.
  EXPECT_EQ(step.age_calc.decayConstantName, "Min et al. 2000");
  EXPECT_EQ(step.age_calc.decayConstantName, normalize_citation("Min (2008)", true));
  EXPECT_EQ(step.age_calc.airRatioName, "Nier 1950");
  EXPECT_EQ(fusion.age_calc.decayConstantName, "Min et al. 2000");
  EXPECT_EQ(fusion.age_calc.airRatioName, "Nier 1950");

  // What is said of one sample is that sample's.
  EXPECT_EQ(step.age_summary.interpretationName, "T (cooling age)");
  EXPECT_EQ(fusion.age_summary.interpretationName, "Unknown");
  EXPECT_EQ(step.age_calc.fluxMonitorName, "Alder Creek sanidine 1.1848 Ma");
  EXPECT_EQ(fusion.age_calc.fluxMonitorName, "FC-2 28.201 Ma");
  EXPECT_EQ(step.datapoint_name, "S-STEP plateau");
  EXPECT_EQ(fusion.datapoint_name, "S-FUS biotite");

  // A string that is no citation the table knows goes as it came, for the lookup to judge.
  options.decay_constant = "Somebody 1999";
  EXPECT_EQ(build(both_samples(), options)[1].age_calc.decayConstantName, "Somebody 1999");

  // No material: the sample's name alone.
  const Submission bare = build_one(with_edit_all(step_sample(), [](pp::Analysis& a) { a.material.clear(); }));
  EXPECT_EQ(bare.datapoint_name, "S-STEP");
  EXPECT_FALSE(bare.sample.materialName.has_value());
  EXPECT_FALSE(bare.data_point.mineralName.has_value());

  // The name ties a workbook's rows together: two samples cannot share one.
  PublishOptions same;
  same.samples["S-STEP"].datapoint_name = "S-FUS biotite";
  const std::string what = refused(both_samples(), same);
  EXPECT_TRUE(contains(what, "\"S-FUS\"") && contains(what, "\"S-STEP\"") && contains(what, "\"S-FUS biotite\"")) << what;
}

// ---------------------------------------------------------------- what is refused or left out

TEST(Mapping, TheSetUpAndTheIrradiationAreNamed) {
  // The set-up is the lab's to name: nothing on an analysis says what
  // EarthBank calls it.
  // Left unnamed, that is said, on each submission: it is the same for all.
  const char* const no_setup = "no analytical set-up named: every data point refers to one EarthBank already has; "
                               "give its name there as the `setup` setting";
  for (const Submission& s : build(both_samples(), {})) {
    EXPECT_EQ(s.setup_name, "");
    EXPECT_EQ(note_with(s, "set-up"), no_setup) << s.datapoint_name;
    EXPECT_EQ(std::count(s.notes.begin(), s.notes.end(), no_setup), 1) << s.datapoint_name;
    // A note is no part of what is submitted.
    Submission quiet = s;
    quiet.notes.clear();
    EXPECT_EQ(content_hash(quiet), content_hash(s));
  }
  PublishOptions options;
  options.setup = "A Noble Gas Lab, Helix MC";
  for (const Submission& s : build(both_samples(), options)) {
    EXPECT_EQ(s.setup_name, "A Noble Gas Lab, Helix MC");
    EXPECT_EQ(note_with(s, "set-up"), "");
  }

  // The irradiation is the one the sample's analyses name.
  Submission step = build_one(step_sample(), options);
  EXPECT_EQ(step.irradiation_name, "NM-900");
  EXPECT_EQ(note_with(step, "irradiation"), "");
  // An analysis that names none is passed over; a sample none of whose
  // analyses names one has none.
  step = build_one(with_edit(step_sample(), 0, [](pp::Analysis& a) { a.irradiation.clear(); }));
  EXPECT_EQ(step.irradiation_name, "NM-900");
  EXPECT_EQ(note_with(step, "irradiation"), "");
  const Submission fusion = build_one(fusion_sample());
  EXPECT_EQ(fusion.irradiation_name, "");
  const char* const no_irradiation = "S-FUS: none of its analyses names an irradiation and the irradiation batch was "
                                     "left out; name it for the sample (the per-sample `irradiation` setting)";
  EXPECT_EQ(note_with(fusion, "irradiation"), no_irradiation);
  EXPECT_EQ(std::count(fusion.notes.begin(), fusion.notes.end(), no_irradiation), 1);

  // A sample can span irradiations: it is published, the name is left out
  // and a note names two of them.
  step = build_one(with_edit(step_sample(), 3, [](pp::Analysis& a) { a.irradiation = "NM-901"; }));
  EXPECT_EQ(step.irradiation_name, "");
  // One note says so, with what to do; not also that none is named.
  const std::string two = "S-STEP: its analyses are of more than one irradiation, \"NM-900\" (20001-01A) and "
                          "\"NM-901\" (20001-01D), and the irradiation batch was left out; name the one to publish "
                          "under for the sample (the per-sample `irradiation` setting)";
  EXPECT_EQ(note_with(step, "irradiation"), two);
  EXPECT_EQ(std::count_if(step.notes.begin(), step.notes.end(),
                          [](const std::string& note) { return note.find("irradiation") != std::string::npos; }),
            1);

  // The lab's word replaces what the analyses say, and settles a
  // disagreement.
  options.samples["S-STEP"].irradiation = "NM-900 (TRIGA, 10 h)";
  step = build_one(with_edit(step_sample(), 3, [](pp::Analysis& a) { a.irradiation = "NM-901"; }), options);
  EXPECT_EQ(step.irradiation_name, "NM-900 (TRIGA, 10 h)");
  EXPECT_EQ(note_with(step, "irradiation"), "");
  const std::vector<Submission> both = build(both_samples(), options);
  ASSERT_EQ(both.size(), 2u);
  EXPECT_EQ(both[0].irradiation_name, "");  // S-FUS: the override is S-STEP's
  EXPECT_EQ(note_with(both[0], "irradiation"), no_irradiation);
  EXPECT_EQ(both[1].irradiation_name, "NM-900 (TRIGA, 10 h)");
  // Named for the sample, there is nothing to say.
  options.samples["S-FUS"].irradiation = "NM-901";
  EXPECT_EQ(note_with(build_one(fusion_sample(), options), "irradiation"), "");

  // An analysis left out for want of an age or an isotope has no say.
  auto undated = with_edit(step_sample(), 3, [](pp::Analysis& a) {
    a.irradiation = "NM-901";
    drop_isotope(a, "Ar37");
  });
  EXPECT_EQ(build_one(undated).irradiation_name, "NM-900");
}

TEST(Mapping, DisagreementIsRefusedNamingBothValues) {
  const auto expect_refused = [](const AnalysisList& analyses, std::initializer_list<std::string_view> parts) {
    const std::string what = refused(analyses);
    EXPECT_TRUE(contains(what, "earthbank: sample \"S-STEP\"")) << what;
    for (const std::string_view part : parts) EXPECT_TRUE(contains(what, part)) << part << " not in: " << what;
  };

  // Two materials.
  expect_refused(with_edit(step_sample(), 3, [](pp::Analysis& a) { a.material = "biotite"; }),
                 {"material", "\"sanidine\" (20001-01A)", "\"biotite\" (20001-01D)"});
  // Two lithologies.
  expect_refused(with_edit(step_sample(), 9, [](pp::Analysis& a) { a.sample_info.lithology = "rhyolite"; }),
                 {"lithology", "\"ash-flow tuff\" (20001-01A)", "\"rhyolite\" (20001-01J)"});
  // Two decay constants: one analysis's flux carries its own.
  expect_refused(with_edit(step_sample(), 5,
                           [](pp::Analysis& a) { a.context.flux->lambda_k_total = r::Measured{5.463e-10, 0.0}; }),
                 {"decay constant", "5.543e-10 (20001-01A)", "5.463e-10 (20001-01F)"});
  // Two air ratios: one analysis was reduced with other constants.
  expect_refused(with_edit(step_sample(), 1,
                           [](pp::Analysis& a) {
                             a.context.constants = r::constants_preset(r::ConstantsPreset::Legacy);
                           }),
                 {"atmospheric 40Ar/36Ar", "298.56 (20001-01A)", "295.5 (20001-01B)"});
  // A location that differs on some analyses: the two points, each whole.
  expect_refused(with_edit(step_sample(), 2, [](pp::Analysis& a) { a.sample_info.latitude = 37.76; }),
                 {"location (latitude, longitude) differs between analyses", "37.75, -106.9 (20001-01A)",
                  "37.76, -106.9 (20001-01C)"});
  expect_refused(with_edit(step_sample(), 8, [](pp::Analysis& a) { a.sample_info.longitude = 106.9; }),
                 {"location (latitude, longitude) differs between analyses", "37.75, -106.9 (20001-01A)",
                  "37.75, 106.9 (20001-01I)"});
  // And the rest of what is one fact of a sample.
  expect_refused(with_edit(step_sample(), 4, [](pp::Analysis& a) { a.sample_info.elevation = 2851.0; }),
                 {"elevation", "2850 (20001-01A)", "2851 (20001-01E)"});
  expect_refused(with_edit(step_sample(), 4, [](pp::Analysis& a) { a.sample_info.igsn = "IEXXX0002"; }),
                 {"IGSN", "\"IEXXX0001\" (20001-01A)", "\"IEXXX0002\" (20001-01E)"});
  expect_refused(with_edit(step_sample(), 4, [](pp::Analysis& a) { a.sample_info.unit = "Carpenter Ridge Tuff"; }),
                 {"stratigraphic unit", "\"Fish Canyon Tuff\"", "\"Carpenter Ridge Tuff\" (20001-01E)"});
  expect_refused(with_edit(step_sample(), 6, [](pp::Analysis& a) { a.monitor.age = pp::Value{28.02, 0.16}; }),
                 {"flux monitor", "\"FC-2 28.201 Ma\" (20001-01A)", "\"FC-2 28.02 Ma\" (20001-01G)"});

  // A monitor named for the sample settles which it is.
  PublishOptions named;
  named.samples["S-STEP"].flux_monitor = "FC-2 28.201 Ma";
  const AnalysisList two_monitors =
      with_edit(step_sample(), 6, [](pp::Analysis& a) { a.monitor.age = pp::Value{28.02, 0.16}; });
  EXPECT_EQ(build_one(two_monitors, named).age_calc.fluxMonitorName, "FC-2 28.201 Ma");

  // One sample's trouble stops the whole build: nothing is half published.
  AnalysisList all = fusion_sample();
  for (const auto& a : with_edit(step_sample(), 3, [](pp::Analysis& x) { x.material = "biotite"; })) all.push_back(a);
  EXPECT_FALSE(build_submissions(all, {}).has_value());
}

TEST(Mapping, AnalysesWithoutAgesAreLeftOutAndCounted) {
  // Step F has no J; step H has an intercept the store does not know.
  AnalysisList steps = with_edit(step_sample(), 5, [](pp::Analysis& a) { a.context.flux.reset(); });
  steps = with_edit(steps, 7, [](pp::Analysis& a) { a.isotopes[1].intercept = {kNaN, 0.01}; });
  ASSERT_FALSE(pp::reduce_analysis(steps[5], {})->arar->ages.has_value());
  ASSERT_FALSE(pp::reduce_analysis(steps[7], {})->arar.has_value());

  const Submission s = build_one(steps);
  ASSERT_EQ(s.measurements.size(), 8u);
  std::vector<double> numbers;
  numbers.reserve(s.measurements.size());
  for (const Measurement& m : s.measurements) numbers.push_back(*m.stepNumber);
  EXPECT_EQ(numbers, (std::vector<double>{1, 2, 3, 4, 5, 7, 9, 10}));  // each keeps its own step number
  const std::string note = note_with(s, "without an age");
  EXPECT_TRUE(contains(note, "2 analyses of 10") && contains(note, "20001-01F") && contains(note, "20001-01H") &&
              contains(note, "Ar39 intercept value"))
      << note;

  // The shares of the 39ArK are of the steps given. With F and H not among
  // them a share would be of a schedule nobody is shown: no step is given one.
  const auto expect_no_shares = [](const Submission& of) {
    for (const Measurement& m : of.measurements) {
      EXPECT_FALSE(m.ar39KReleased.has_value());
      EXPECT_FALSE(m.cumulative39ArK.has_value());
    }
  };
  expect_no_shares(s);
  EXPECT_EQ(note_with(s, "ar39KReleased"),
            "20001-01: steps F, H are not among the analyses published, so ar39KReleased and cumulative39ArK were "
            "left out for the aliquot");
  // And the reported age counts only the steps with an age.
  EXPECT_LE(*s.age_summary.nAnalysisGrouped, 8);

  // F alone, which reduced but has no J: the same, though its 39ArK is known.
  const AnalysisList no_j = with_edit(step_sample(), 5, [](pp::Analysis& a) { a.context.flux.reset(); });
  const Submission t = build_one(no_j);
  ASSERT_EQ(t.measurements.size(), 9u);
  expect_no_shares(t);
  EXPECT_TRUE(contains(note_with(t, "ar39KReleased"), "20001-01: step F is not among the analyses published"))
      << note_with(t, "ar39KReleased");
  EXPECT_TRUE(contains(note_with(t, "without an age"), "1 analysis of 10"));

  // A step that was never given: the gap in the letters says so.
  AnalysisList gap = step_sample();
  gap.erase(gap.begin() + 2);
  const Submission g = build_one(gap);
  ASSERT_EQ(g.measurements.size(), 9u);
  expect_no_shares(g);
  EXPECT_TRUE(contains(note_with(g, "ar39KReleased"), "20001-01: step C is not among the analyses published"))
      << note_with(g, "ar39KReleased");

  // A complete aliquot beside an incomplete one keeps its shares.
  AnalysisList two = step_sample();
  for (const int i : {0, 1, 3}) {  // a second aliquot heated in steps A, B and D
    auto step = std::make_shared<pp::Analysis>(*step_analysis(i + 2));
    step->aliquot = 2;
    step->increment = i;
    step->runid = pp::make_runid("20001", 2, i);
    step->uuid += "-second";
    step->timestamp += 86'400.0;
    irradiate_for_real(*step);
    two.push_back(step);
  }
  PublishOptions pooled;
  pooled.samples["S-STEP"].preferred_age = PreferredAge::WeightedMean;
  const Submission both = build_one(two, pooled);
  const Submission whole = build_one(step_sample());
  ASSERT_EQ(both.measurements.size(), 13u);
  for (std::size_t i = 0; i < 10; ++i) {
    EXPECT_REL(both.measurements[i].ar39KReleased, *whole.measurements[i].ar39KReleased);
    EXPECT_REL(both.measurements[i].cumulative39ArK, *whole.measurements[i].cumulative39ArK);
  }
  for (std::size_t i = 10; i < 13; ++i) EXPECT_FALSE(both.measurements[i].ar39KReleased.has_value());
  EXPECT_TRUE(contains(note_with(both, "ar39KReleased"), "20001-02: step C is not among the analyses published"));
  EXPECT_EQ(note_with(whole, "ar39KReleased"), "");

  // Nothing left out: no such note.
  EXPECT_EQ(note_with(build_one(step_sample()), "without an age"), "");
}

TEST(Mapping, ASampleWithNoAgeIsRefused) {
  const AnalysisList no_flux = with_edit_all(step_sample(), [](pp::Analysis& a) { a.context.flux.reset(); });
  const std::string what = refused(no_flux);
  EXPECT_TRUE(contains(what, "earthbank: sample \"S-STEP\"") && contains(what, "none of its 10 analyses has an age") &&
              contains(what, "20001-01A"))
      << what;

  // An unknown without its irradiation: the reduction's own reason is passed on.
  const AnalysisList no_chronology =
      with_edit_all(fusion_sample(), [](pp::Analysis& a) { a.context.chronology.clear(); });
  EXPECT_TRUE(contains(refused(no_chronology), "no chronology")) << refused(no_chronology);

  // With a good sample beside it, still refused.
  AnalysisList all = fusion_sample();
  for (const auto& a : no_flux) all.push_back(a);
  EXPECT_TRUE(contains(refused(all), "\"S-STEP\""));
}

// An aliquot's size is its weight in mg, where the analyses know one.
TEST(Mapping, AliquotSize) {
  const auto weighing = [](double weight) { return [weight](pp::Analysis& a) { a.extraction.weight = weight; }; };

  const Submission step = build_one(with_edit_all(step_sample(), weighing(12.5)));
  ASSERT_EQ(step.aliquots.size(), 1u);
  EXPECT_EQ(step.aliquots[0].aliquotSize, 12.5);
  EXPECT_EQ(step.aliquots[0].aliquotUnits, "mg");
  EXPECT_EQ(step.aliquots[0].aliquotUnits, kMilligrams);

  // A weight only some of its analyses carry is the aliquot's.
  const Submission from_one = build_one(with_edit(step_sample(), 6, weighing(3.25)));
  EXPECT_EQ(from_one.aliquots[0].aliquotSize, 3.25);
  EXPECT_EQ(from_one.aliquots[0].aliquotUnits, "mg");

  // Each fusion is its own aliquot with its own weight; one without has neither field.
  AnalysisList fusions = fusion_sample();
  for (std::size_t i = 0; i < fusions.size(); ++i) {
    if (i != 4) fusions = with_edit(fusions, i, weighing(1.0 + 0.5 * static_cast<double>(i)));
  }
  const Submission fusion = build_one(fusions);
  ASSERT_EQ(fusion.aliquots.size(), 6u);
  EXPECT_EQ(fusion.aliquots[0].aliquotSize, 1.0);
  EXPECT_EQ(fusion.aliquots[3].aliquotSize, 2.5);
  EXPECT_EQ(fusion.aliquots[3].aliquotUnits, "mg");
  EXPECT_FALSE(fusion.aliquots[4].aliquotSize.has_value());
  EXPECT_FALSE(fusion.aliquots[4].aliquotUnits.has_value());

  // No weight, or one that is none (zero, negative): no size and no unit.
  for (const double none : {0.0, -2.0}) {
    const Submission s = build_one(with_edit_all(step_sample(), weighing(none)));
    EXPECT_FALSE(s.aliquots[0].aliquotSize.has_value()) << none;
    EXPECT_FALSE(s.aliquots[0].aliquotUnits.has_value()) << none;
  }
  // Not a number: left out, and the notes say so.
  const Submission nan = build_one(with_edit_all(step_sample(), weighing(kNaN)));
  EXPECT_FALSE(nan.aliquots[0].aliquotSize.has_value());
  EXPECT_FALSE(nan.aliquots[0].aliquotUnits.has_value());
  EXPECT_EQ(note_with(nan, "aliquotSize"), "20001-01: aliquotSize is not a finite number and was left out");

  // One piece has one weight.
  const std::string what = refused(with_edit(with_edit_all(step_sample(), weighing(12.5)), 3, weighing(13.0)));
  EXPECT_TRUE(contains(what, "earthbank: sample \"S-STEP\"") && contains(what, "aliquot size of 20001-01") &&
              contains(what, "12.5 (20001-01A)") && contains(what, "13 (20001-01D)"))
      << what;

  // The beam's diameter is recorded without its unit: no beam size is sent.
  const Submission beam =
      build_one(with_edit_all(step_sample(), [](pp::Analysis& a) { a.extraction.beam_diameter = 2.0; }));
  EXPECT_FALSE(beam.measurements[0].beamSize.has_value());
  EXPECT_FALSE(beam.measurements[0].beamSizeUnits.has_value());
}

// The fields of spec 4.5, and every id: none is the mapping's to fill.
TEST(Mapping, LeftEmptyFieldsAreAbsent) {
  // Even when the analyses carry a beam diameter (its unit is not known).
  AnalysisList all = with_edit_all(both_samples(), [](pp::Analysis& a) {
    a.extraction.weight = 12.5;
    a.extraction.beam_diameter = 2.0;
    a.comment = "a comment";
  });
  const std::set<std::string> empty = {
      "grainDiameterMin", "grainDiameterMax", "aliquotComment", "beamSize",
      "beamSizeUnits", "kContent", "kContentUncertainty", "fundingName", "literatureName", "archiveName",
      "sampleKindName", "sampleMethodName", "collectDateMin", "collectDateMax", "rockUnitAgeMin", "rockUnitAgeMax",
      "rockUnitAgeDescription", "relativeElevationMin", "relativeElevationMax", "locationKindName", "locationName",
      "analysisScaleName", "commentAnalyte", "measurementComment", "ratio40Arx36ArTrapped",
      "ratio40Arx36ArTrappedUncertainty", "latLonPrecision"};
  std::size_t records = 0;
  const auto check = [&](const nlohmann::json& record) {
    ++records;
    ASSERT_TRUE(record.is_object());
    for (const auto& item : record.items()) {
      const std::string& key = item.key();
      EXPECT_FALSE(empty.contains(key)) << key;
      EXPECT_FALSE(key == "id" || (key.size() > 2 && key.substr(key.size() - 2) == "Id")) << key;
      // Whatever is sent is something: no null, no empty string, no number that is not one.
      EXPECT_FALSE(item.value().is_null()) << key;
      if (item.value().is_string()) {
        EXPECT_FALSE(item.value().get<std::string>().empty()) << key;
      }
      if (item.value().is_number_float()) {
        EXPECT_TRUE(std::isfinite(item.value().get<double>())) << key;
      }
    }
  };
  for (const Submission& s : build(all)) {
    check(to_json(s.sample));
    if (s.location) check(to_json(*s.location));
    check(to_json(s.data_point));
    for (const Aliquot& a : s.aliquots) check(to_json(a));
    for (const Measurement& m : s.measurements) check(to_json(m));
    check(to_json(s.age_calc));
    check(to_json(s.age_summary));
  }
  EXPECT_EQ(records, 2u * 4u + 1u + 6u + 1u + 6u + 10u);

  // What a step's measurement does carry, by name: nothing has gone missing
  // or been added unnoticed.
  const Submission step = build_one(step_sample());
  const std::set<std::string> keys = keys_of(to_json(step.measurements[3]));
  std::set<std::string> expected = {
      "aliquotName", "analysisTime", "analysisUnits", "apparentAge", "apparentAgeUncertainityTypeName",
      "apparentAgeUncertainty", "ar39KReleased", "caK", "caKUncertainty", "clK", "clKUncertainty", "cumulative39ArK",
      "jValue", "jValueUncertainty", "jvalueUncertaintyTypeName", "kCa", "kCaUncertainty", "laserPower",
      "laserPowerUnits", "plateauStepName", "radiogenicArgon", "ratio36Arx39ArCorr", "ratio36Arx39ArCorrUncertainty",
      "ratio37Arx39ArCorr", "ratio37Arx39ArCorrUncertainty", "ratio38Arx39ArCorr", "ratio38Arx39ArCorrUncertainty",
      "ratio40Arx39ArCorr", "ratio40Arx39ArCorrUncertainty", "ratio40Arx39ArK", "ratio40Arx39ArKUncertainty",
      "stepNumber"};
  for (const char* mass : {"36", "37", "38", "39", "40"}) {
    for (const char* kind : {"Corr", "ExcInt", "blank"}) {
      expected.insert(std::string("ar") + mass + kind);
      expected.insert(std::string("ar") + mass + kind + "Uncertainty");
    }
  }
  EXPECT_EQ(keys, expected);
  // A fusion's: no step number and no share of a schedule, and the column of
  // the weighted mean in place of the plateau's.
  expected.erase("stepNumber");
  expected.erase("ar39KReleased");
  expected.erase("cumulative39ArK");
  expected.erase("plateauStepName");
  expected.insert("weightedMeanStepName");
  EXPECT_EQ(keys_of(to_json(build_one(fusion_sample()).measurements[3])), expected);
  // An aliquot with a weight, and one without.
  ASSERT_FALSE(build(all).empty());
  EXPECT_EQ(keys_of(to_json(build(all)[1].aliquots[0])), (std::set<std::string>{"aliquotName", "aliquotSize", "aliquotUnits"}));
  EXPECT_EQ(keys_of(to_json(step.aliquots[0])), (std::set<std::string>{"aliquotName"}));
  std::set<std::string> summary;
  const nlohmann::json age_summary = to_json(step.age_summary);
  for (const auto& item : age_summary.items()) summary.insert(item.key());
  EXPECT_EQ(summary, (std::set<std::string>{"age", "ageCalcTypeName", "ageUncertainty", "ageUncertaintyTypeName",
                                            "agepvalue", "calculatedAgeUncertaintyTypeName",
                                            "calculatedAgeUncertaintyTypeUnitsName", "dataInterpretationToolName",
                                            "description", "interpretationName", "mswd", "nAnalysisGrouped",
                                            "preferredAge"}));
}

// A number that is not finite is not sent, and the notes say which.
TEST(Mapping, NonFiniteValuesAreLeftOutAndNoted) {
  AnalysisList steps = with_edit(step_sample(), 3, [](pp::Analysis& a) { a.extraction.value = kNaN; });
  steps = with_edit_all(steps, [](pp::Analysis& a) {
    a.sample_info.elevation = std::numeric_limits<double>::infinity();
    a.sample_info.latitude = kNaN;
    a.monitor.age = pp::Value{kNaN, 0.0};
  });
  const Submission s = build_one(steps);

  const Measurement& d = s.measurements[3];
  EXPECT_EQ(d.stepNumber, 4.0);
  EXPECT_FALSE(d.laserPower.has_value());
  EXPECT_FALSE(d.laserPowerUnits.has_value());
  EXPECT_FALSE(d.furnaceTemperature.has_value());
  EXPECT_EQ(note_with(s, "laserPower"), "20001-01D: laserPower is not a finite number and was left out");
  EXPECT_TRUE(s.measurements[4].laserPower.has_value());
  // The method is still judged from the steps that have a power.
  EXPECT_EQ(s.data_point.arMethodName, "Step-heating - laser");

  EXPECT_FALSE(s.sample.referenceElevation.has_value());
  EXPECT_EQ(note_with(s, "referenceElevation"), "S-STEP: referenceElevation is not a finite number and was left out");
  EXPECT_FALSE(s.location.has_value());
  EXPECT_EQ(note_with(s, "lat is not"), "S-STEP: lat is not a finite number and was left out");
  EXPECT_NE(note_with(s, "longitude and no latitude"), "");
  EXPECT_EQ(s.age_calc.fluxMonitorName, "FC-2");
  EXPECT_NE(note_with(s, "flux monitor's age is not a finite number"), "");

  // An analysis whose time was not recorded (one that needs no chronology to
  // have an age): no time is invented for it, and the date is of the others.
  const AnalysisList fusions = with_edit(fusion_sample(), 2, [](pp::Analysis& a) {
    a.analysis_type = "monitor";
    a.context.chronology.clear();
    a.timestamp = 0.0;
  });
  const Submission f = build_one(fusions);
  ASSERT_EQ(f.measurements.size(), 6u);
  EXPECT_EQ(f.measurements[0].aliquotName, "20002-03");  // sorts first
  EXPECT_FALSE(f.measurements[0].analysisTime.has_value());
  EXPECT_TRUE(f.measurements[0].apparentAge.has_value());
  EXPECT_NE(note_with(f, "20002-03: the analysis has no time"), "");
  EXPECT_EQ(f.data_point.analysisDate, "2023-11-14");
  EXPECT_EQ(f.measurements[1].analysisTime, "2023-11-14T22:13:20Z");

  // Nothing that is not a number in what is sent.
  for (const Measurement& m : s.measurements) {
    const nlohmann::json record = to_json(m);
    for (const auto& item : record.items()) {
      EXPECT_FALSE(item.value().is_null()) << item.key();
      if (item.value().is_number_float()) {
        EXPECT_TRUE(std::isfinite(item.value().get<double>())) << item.key();
      }
    }
  }
}

// An analysis is published only with all five argon isotopes measured. The
// reduction puts a zero in the place of one that was not, and goes on: without
// 37Ar it makes no Ca correction, without 38Ar no chlorine correction, without
// 36Ar no atmospheric one, and gives an age all the same. None of that is sent.
TEST(Mapping, AMissingIsotopeIsAbsent) {
  AnalysisList five = fusion_sample();
  five.erase(five.begin() + 2);
  const Submission others = build_one(five);
  const auto dropping = [](std::initializer_list<const char*> isotopes) {
    return [isotopes](pp::Analysis& a) {
      for (const char* isotope : isotopes) drop_isotope(a, isotope);
    };
  };

  for (const char* isotope : {"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"}) {
    const Submission s = build_one(with_edit(fusion_sample(), 2, dropping({isotope})));
    ASSERT_EQ(s.measurements.size(), 5u) << isotope;
    for (const Measurement& m : s.measurements) EXPECT_NE(m.aliquotName, "20002-03") << isotope;
    EXPECT_EQ(s.aliquots.size(), 5u) << isotope;
    EXPECT_EQ(s.age_summary.nAnalysisGrouped, 5) << isotope;
    EXPECT_EQ(note_with(s, "left out"),
              "1 analysis of 6 left out, not having all five argon isotopes measured: 20002-03 (" +
                  std::string(isotope) + " not measured)")
        << isotope;
    // What is sent is what the five others alone give: the age, and every field of every measurement.
    EXPECT_EQ(s.age_summary, others.age_summary) << isotope;
    EXPECT_EQ(s.measurements, others.measurements) << isotope;
    for (const Measurement& m : s.measurements) {
      EXPECT_EQ(keys_of(to_json(m)), keys_of(to_json(others.measurements[0]))) << isotope;
    }
    // Nothing is published with a part unset in its place.
    EXPECT_EQ(note_with(s, "was not measured;"), "") << isotope;
  }
  // The reduction gives the three of these an age, so it is not for want of
  // one that they are left out.
  for (const char* isotope : {"Ar38", "Ar37", "Ar36"}) {
    const AnalysisList one = with_edit(fusion_sample(), 2, dropping({isotope}));
    ASSERT_TRUE(pp::reduce_analysis(one[2], {})->arar->ages.has_value()) << isotope;
  }

  // Two missing: both are named; and two analyses: both are counted.
  AnalysisList several = with_edit(fusion_sample(), 2, dropping({"Ar37", "Ar38"}));
  several = with_edit(several, 4, dropping({"Ar36"}));
  const Submission four = build_one(several);
  EXPECT_EQ(four.measurements.size(), 4u);
  EXPECT_EQ(note_with(four, "left out"),
            "2 analyses of 6 left out, not having all five argon isotopes measured: 20002-03 (Ar38, Ar37 not "
            "measured), 20002-05 (Ar36 not measured)");
  // All measured: no such note.
  EXPECT_EQ(note_with(build_one(fusion_sample()), "argon isotopes"), "");

  // A heating step left out leaves its aliquot's schedule with a gap, like any other.
  const Submission steps = build_one(with_edit(step_sample(), 3, dropping({"Ar37"})));
  ASSERT_EQ(steps.measurements.size(), 9u);
  EXPECT_TRUE(contains(note_with(steps, "argon isotopes"), "1 analysis of 10") &&
              contains(note_with(steps, "argon isotopes"), "20001-01D (Ar37 not measured)"))
      << note_with(steps, "argon isotopes");
  EXPECT_TRUE(contains(note_with(steps, "ar39KReleased"), "20001-01: step D is not among the analyses published"));
  EXPECT_FALSE(steps.measurements[0].cumulative39ArK.has_value());

  // None left: refused, saying which isotope each lacks and what the rule is.
  for (const char* isotope : {"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"}) {
    const std::string what = refused(with_edit_all(fusion_sample(), dropping({isotope})));
    EXPECT_TRUE(contains(what, "earthbank: sample \"S-FUS\": none of its 6 analyses can be published: 20002-01 (" +
                                   std::string(isotope) + " not measured), 20002-02 (") &&
                contains(what, "an analysis is published only when Ar40, Ar39, Ar38, Ar37 and Ar36 were all "
                               "measured and it has an age; take the sample out of the selection"))
        << what;
  }
  // The same when some lack an isotope and the rest an age.
  AnalysisList none = with_edit_all(fusion_sample(), [](pp::Analysis& a) { a.context.flux.reset(); });
  none = with_edit(none, 0, dropping({"Ar38"}));
  const std::string what = refused(none);
  EXPECT_TRUE(contains(what, "none of its 6 analyses can be published: 20002-01 (Ar38 not measured), 20002-02") &&
              contains(what, "take the sample out of the selection"))
      << what;
}

// What the reported age is and what its uncertainty holds is said in words,
// in the age summary's description and in the notes: the sentence of each kind.
TEST(Mapping, TheAgeSummarySaysWhatItsUncertaintyIs) {
  const AnalysisList steps = step_sample();
  const pp::Dataset sd = dataset_of(steps);
  const pp::GroupItems step_items = items_of(sd);
  const auto described = [](const Submission& s) {
    EXPECT_TRUE(s.age_summary.description.has_value());
    const std::string sentence = s.age_summary.description.value_or("");
    // The same sentence is among the notes the user is shown.
    EXPECT_NE(std::find(s.notes.begin(), s.notes.end(), sentence), s.notes.end()) << sentence;
    EXPECT_EQ(s.age_summary.ageUncertaintyTypeName, "1 sigma");
    EXPECT_EQ(s.age_summary.calculatedAgeUncertaintyTypeName, "1 sigma");
    return sentence;
  };

  // A plateau: its steps and its share of the gas.
  PublishOptions options;
  options.software = "pychron-cpp 9.9.9";
  const auto found =
      pp::group_plateau(step_items, options.ages.plateau, options.ages.plateau_weighting, options.ages.mean_error);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(described(build_one(steps, options)),
            "Plateau Age of 6 of 10 analyses (steps C to H, " + printed(found->mean.gas_fraction, 4) +
                "% of the 39ArK); uncertainty 1 sigma (the mean's msem), J uncertainty included; reduced with "
                "pychron-cpp 9.9.9.");
  EXPECT_GT(found->mean.gas_fraction, 50.0);

  // The same without the J uncertainty, and with another error kind.
  options.age_includes_j_error = false;
  options.ages.mean_error = r::MeanErrorKind::Sem;
  EXPECT_EQ(described(build_one(steps, options)),
            "Plateau Age of 6 of 10 analyses (steps C to H, " + printed(found->mean.gas_fraction, 4) +
                "% of the 39ArK); uncertainty 1 sigma (the mean's sem), analytical uncertainty only; reduced with "
                "pychron-cpp 9.9.9.");

  // A weighted mean.
  EXPECT_EQ(described(build_one(fusion_sample())),
            "Weighted Mean Age of 6 of 6 analyses; uncertainty 1 sigma (the mean's msem), J uncertainty included; "
            "reduced with pychron-cpp.");
  PublishOptions sd_only;
  sd_only.ages.mean_error = r::MeanErrorKind::Sd;
  sd_only.age_includes_j_error = false;
  EXPECT_EQ(described(build_one({fusion_analysis(0)}, sd_only)),
            "Weighted Mean Age of 1 of 1 analysis; uncertainty 1 sigma (the mean's sd), analytical uncertainty "
            "only; reduced with pychron-cpp.");

  // An isochron: the trapped ratio its intercept gives.
  PublishOptions isochron;
  isochron.samples["S-STEP"].preferred_age = PreferredAge::Isochron;
  const auto fit = pp::isochron_age(pp::isochron_points(step_items), isochron.ages.york, false, false);
  ASSERT_TRUE(fit.has_value());
  EXPECT_GT(fit->trapped.value, 200.0);
  EXPECT_GT(fit->trapped.error, 0.0);
  EXPECT_EQ(described(build_one(steps, isochron)),
            "Inverse Isochron Age of 10 of 10 analyses (trapped 40Ar/36Ar " + printed(fit->trapped.value, 6) +
                " +/- " + printed(fit->trapped.error, 2) +
                "); uncertainty 1 sigma (propagated through the regression and the age equation), J uncertainty "
                "included; reduced with pychron-cpp.");
  isochron.age_includes_j_error = false;
  EXPECT_TRUE(contains(described(build_one(steps, isochron)),
                       "(propagated through the regression and the age equation), analytical uncertainty only;"));

  // A total gas age; here because no run of steps is a plateau.
  PublishOptions strict;
  strict.ages.plateau.nsteps = 9;
  EXPECT_EQ(described(build_one(steps, strict)),
            "Total Gas Age of 10 of 10 analyses; uncertainty 1 sigma (propagated through the age equation), J "
            "uncertainty included; reduced with pychron-cpp.");
  strict.age_includes_j_error = false;
  EXPECT_EQ(described(build_one(steps, strict)),
            "Total Gas Age of 10 of 10 analyses; uncertainty 1 sigma (propagated through the age equation), "
            "analytical uncertainty only; reduced with pychron-cpp.");

  // The decay constant's uncertainty, when the reduction carries it into the
  // ages, is in every one of these uncertainties, and the sentence says so:
  // none of them is then "analytical uncertainty only".
  const std::string plateau_of = "Plateau Age of 6 of 10 analyses (steps C to H, " +
                                 printed(found->mean.gas_fraction, 4) + "% of the 39ArK); uncertainty 1 sigma ";
  PublishOptions decay;
  decay.reduction.include_decay_error = true;
  const Submission with_decay = build_one(steps, decay);
  EXPECT_EQ(described(with_decay), plateau_of + "(the mean's msem), J uncertainty included, decay constant "
                                                "uncertainty included; reduced with pychron-cpp.");
  // It is the report's number under the same settings, and more than without.
  EXPECT_REL(with_decay.age_summary.ageUncertainty, cell(report_of(steps, decay), "plateau age ±(1s) with J"));
  EXPECT_GT(*with_decay.age_summary.ageUncertainty, *build_one(steps).age_summary.ageUncertainty);
  EXPECT_GT(*with_decay.measurements[3].apparentAgeUncertainty,
            *build_one(steps).measurements[3].apparentAgeUncertainty);
  decay.age_includes_j_error = false;
  const Submission decay_only = build_one(steps, decay);
  EXPECT_EQ(described(decay_only), plateau_of + "(the mean's msem), J uncertainty not included, decay constant "
                                                "uncertainty included; reduced with pychron-cpp.");
  EXPECT_FALSE(contains(note_with(decay_only, "reported age's uncertainty"), "analytical"))
      << note_with(decay_only, "reported age's uncertainty");
  // The constants of the analyses can ask for it as well as the settings.
  const AnalysisList by_constants = with_edit_all(steps, [](pp::Analysis& a) {
    a.context.constants = r::constants_preset(r::ConstantsPreset::Default);
    a.context.constants->include_decay_error = true;
  });
  EXPECT_EQ(described(build_one(by_constants)), described(with_decay));
  EXPECT_EQ(build_one(by_constants).age_summary.ageUncertainty, with_decay.age_summary.ageUncertainty);
  // The other kinds.
  for (const PreferredAge kind : {PreferredAge::WeightedMean, PreferredAge::Isochron, PreferredAge::Integrated}) {
    PublishOptions of_kind;
    of_kind.samples["S-STEP"].preferred_age = kind;
    of_kind.reduction.include_decay_error = true;
    const Submission with = build_one(steps, of_kind);
    EXPECT_TRUE(contains(described(with), "), J uncertainty included, decay constant uncertainty included; reduced"))
        << described(with);
    of_kind.age_includes_j_error = false;
    const Submission without_j = build_one(steps, of_kind);
    EXPECT_TRUE(contains(described(without_j),
                         "), J uncertainty not included, decay constant uncertainty included; reduced"))
        << described(without_j);
    of_kind.reduction.include_decay_error = false;
    const Submission analytical = build_one(steps, of_kind);
    EXPECT_TRUE(contains(described(analytical), "), analytical uncertainty only; reduced")) << described(analytical);
    // What the sentence says is in the number.
    EXPECT_GT(*without_j.age_summary.ageUncertainty, *analytical.age_summary.ageUncertainty) << age_type_name(kind);
  }
  // A mean over several J says both.
  const AnalysisList two_j =
      with_edit(fusion_sample(), 4, [](pp::Analysis& a) { a.context.flux->j = r::Measured{0.00151, 6.04e-6}; });
  PublishOptions fusion_decay;
  fusion_decay.reduction.include_decay_error = true;
  EXPECT_EQ(described(build_one(two_j, fusion_decay)),
            "Weighted Mean Age of 6 of 6 analyses; uncertainty 1 sigma (the mean's msem), J uncertainty included "
            "(the largest relative J uncertainty among its analyses, 0.4% of 20002-05), decay constant uncertainty "
            "included; reduced with pychron-cpp.");
  // Analyses that do not agree on it have no one sentence that is true of them.
  const std::string mixed = refused(with_edit(steps, 4, [](pp::Analysis& a) {
    a.context.constants = r::constants_preset(r::ConstantsPreset::Default);
    a.context.constants->include_decay_error = true;
  }));
  EXPECT_TRUE(contains(mixed, "earthbank: sample \"S-STEP\"") &&
              contains(mixed, "decay constant uncertainty in the ages differs between analyses") &&
              contains(mixed, "not included (20001-01A)") && contains(mixed, "included (20001-01E)"))
      << mixed;

  // An analysis left out is in neither number.
  const AnalysisList nine = with_edit(steps, 9, [](pp::Analysis& a) { a.context.flux.reset(); });
  EXPECT_TRUE(contains(described(build_one(nine)), "Plateau Age of 6 of 9 analyses (steps C to H, "))
      << described(build_one(nine));
}

// The J uncertainty in a mean is that of the analyses the mean is of.
TEST(Mapping, TheJOfAMeanIsThatOfItsAnalyses) {
  const PublishOptions defaults;
  // Steps A and J, outside the plateau, and a fusion run before the schedule
  // sit in other irradiation positions, whose J is ten times less well known.
  const auto elsewhere = [](pp::Analysis& a) { a.context.flux->j = r::Measured{0.0015, 3e-5}; };
  auto fusion = std::make_shared<pp::Analysis>(*fusion_analysis(0));
  fusion->identifier = "20001";
  fusion->aliquot = 2;
  fusion->runid = pp::make_runid("20001", 2, -1);
  fusion->sample = "S-STEP";
  fusion->material = "sanidine";
  fusion->timestamp = step_analysis(0)->timestamp - 7200.0;
  irradiate_for_real(*fusion);
  elsewhere(*fusion);
  AnalysisList mixed = with_edit(with_edit(step_sample(), 0, elsewhere), 9, elsewhere);
  mixed.push_back(fusion);

  const pp::Dataset d = dataset_of(step_sample());
  const auto found = pp::group_plateau(items_of(d), defaults.ages.plateau, defaults.ages.plateau_weighting,
                                       defaults.ages.mean_error);
  ASSERT_TRUE(found.has_value());
  const r::Mean& mean = found->mean.mean;
  const Submission s = build_one(mixed);
  ASSERT_EQ(s.age_summary.ageCalcTypeName, "Plateau Age");
  EXPECT_EQ(plateau_flags(s), (std::vector<std::string>{"C", "D", "E", "F", "G", "H"}));
  EXPECT_REL(s.age_summary.age, mean.value);
  // 0.2%, the J of steps C to H; not the 2% of the fusion, which is the first analysis of the sample.
  EXPECT_REL(s.age_summary.ageUncertainty, pp::with_external_error(mean.value, mean.error, 0.002));
  EXPECT_LT(*s.age_summary.ageUncertainty, pp::with_external_error(mean.value, mean.error, 0.02) / 2.0);
  // Its steps share one J: nothing to say about that.
  EXPECT_EQ(note_with(s, "do not share one J"), "");
  EXPECT_FALSE(contains(s.age_summary.description.value_or(""), "largest"));

  // A mean over analyses of two positions: each age was made with its own J,
  // and the J term of the mean is the larger of the two relative
  // uncertainties, which no weighting of them exceeds. Both are said.
  const AnalysisList fusions =
      with_edit(fusion_sample(), 4, [](pp::Analysis& a) { a.context.flux->j = r::Measured{0.00151, 6.04e-6}; });
  const pp::Dataset fd = dataset_of(fusions);
  const auto fmean = pp::group_weighted_mean(items_of(fd), defaults.ages.mean_error);
  ASSERT_TRUE(fmean.has_value());
  const Submission f = build_one(fusions);
  EXPECT_EQ(f.age_summary.ageCalcTypeName, "Weighted Mean Age");
  EXPECT_EQ(f.measurements[4].jValue, 0.00151);
  EXPECT_REL(f.age_summary.age, fmean->value);
  EXPECT_REL(f.age_summary.ageUncertainty, pp::with_external_error(fmean->value, fmean->error, 6.04e-6 / 0.00151));
  EXPECT_GT(*f.age_summary.ageUncertainty, pp::with_external_error(fmean->value, fmean->error, 0.002) * 1.2);
  const std::string note = note_with(f, "do not share one J");
  EXPECT_TRUE(contains(note, "20002-05") && contains(note, "0.4%") && contains(note, "largest")) << note;
  EXPECT_EQ(f.age_summary.description,
            "Weighted Mean Age of 6 of 6 analyses; uncertainty 1 sigma (the mean's msem), J uncertainty included "
            "(the largest relative J uncertainty among its analyses, 0.4% of 20002-05); reduced with pychron-cpp.");
  // Without the J uncertainty there is no J in the age's uncertainty to speak of.
  PublishOptions analytical;
  analytical.age_includes_j_error = false;
  const Submission plain = build_one(fusions, analytical);
  EXPECT_REL(plain.age_summary.ageUncertainty, fmean->error);
  EXPECT_EQ(note_with(plain, "do not share one J"), "");
  EXPECT_EQ(note_with(build_one(fusion_sample()), "do not share one J"), "");
}

// An age made by one pass through the age equation rests on one J: over
// analyses of two it is refused, not made with the J of whichever came first.
TEST(Mapping, AnAgeOfOneJIsRefusedOverTwo) {
  const auto expect_refused = [](const AnalysisList& analyses, const PublishOptions& options,
                                 std::initializer_list<std::string_view> parts) {
    const std::string what = refused(analyses, options);
    for (const std::string_view part : parts) EXPECT_TRUE(contains(what, part)) << part << " not in: " << what;
  };
  const AnalysisList other_j =
      with_edit(fusion_sample(), 4, [](pp::Analysis& a) { a.context.flux->j = r::Measured{0.00151, 3e-6}; });
  const AnalysisList other_error =
      with_edit(fusion_sample(), 2, [](pp::Analysis& a) { a.context.flux->j = r::Measured{0.0015, 4e-6}; });

  for (const PreferredAge kind : {PreferredAge::Integrated, PreferredAge::Isochron}) {
    PublishOptions options;
    options.samples["S-FUS"].preferred_age = kind;
    expect_refused(other_j, options,
                   {"earthbank: sample \"S-FUS\"", age_type_name(kind), "J differs",
                    "0.0015 +/- 3e-06 (20002-01)", "0.00151 +/- 3e-06 (20002-05)", "weighted_mean"});
    // The value the same and its uncertainty another is another J.
    expect_refused(other_error, options,
                   {"earthbank: sample \"S-FUS\"", age_type_name(kind), "J differs",
                    "0.0015 +/- 3e-06 (20002-01)", "0.0015 +/- 4e-06 (20002-03)"});
    // With or without the J uncertainty in what is reported: the age itself is of J.
    options.age_includes_j_error = false;
    expect_refused(other_j, options, {age_type_name(kind), "J differs"});
    // One J: computed.
    options.samples["S-STEP"].preferred_age = kind;
    EXPECT_EQ(build_one(step_sample(), options).age_summary.ageCalcTypeName, age_type_name(kind));
  }

  // The rule's own fall back, a step-heated sample without a plateau, is
  // refused the same way.
  PublishOptions strict;
  strict.ages.plateau.nsteps = 9;
  const AnalysisList steps =
      with_edit(step_sample(), 7, [](pp::Analysis& a) { a.context.flux->j = r::Measured{0.00152, 3e-6}; });
  expect_refused(steps, strict,
                 {"earthbank: sample \"S-STEP\"", "no age to report", "at least 9 steps", "Total Gas Age",
                  "0.0015 +/- 3e-06 (20001-01A)", "0.00152 +/- 3e-06 (20001-01H)", "weighted_mean"});
  // The weighted mean it is told of is there for the asking.
  strict.samples["S-STEP"].preferred_age = PreferredAge::WeightedMean;
  EXPECT_EQ(build_one(steps, strict).age_summary.ageCalcTypeName, "Weighted Mean Age");
}

}  // namespace
}  // namespace pychron::earthbank
