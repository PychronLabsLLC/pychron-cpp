#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "fake_earthbank.hpp"
#include "pychron/core/error.hpp"
#include "pychron/earthbank/lookup.hpp"
#include "pychron/earthbank/model.hpp"
#include "pychron/earthbank/preflight.hpp"
#include "pychron/earthbank/session.hpp"
#include "pychron/earthbank/vocab.hpp"
#include "records_support.hpp"
#include "session_support.hpp"

namespace pychron::earthbank {
namespace {

using nlohmann::json;
using testing::all_fields;
using testing::FakeEarthBank;
using testing::LoggedRequest;
using testing::Store;

// A submission as the mapping makes one: every term a name the fixtures'
// lists hold, no id set.
Submission good(const std::string& sample, int measurements = 3) {
  Submission s;
  s.datapoint_name = sample + " Sanidine";
  s.sample.name = sample;
  s.sample.sampleID = sample;
  s.sample.materialName = "Sanidine";
  s.location = Location{};
  s.location->name = sample;
  s.location->lat = -34.9;
  s.location->lon = 138.6;
  s.data_point.analysisDate = "2026-05-20";
  s.data_point.analysisUnits = std::string(kIntensityUnits);
  s.data_point.arMethodName = "Step-heating - laser";
  s.data_point.mineralName = "Sanidine";
  s.data_point.lithologyName = "Rhyolite";
  s.data_point.analyticalUncertaintyTypeName = std::string(kOneSigma);
  s.data_point.analyticalUncertaintyUnitName = std::string(kAbsoluteUncertainty);
  s.data_point.apparentAgeUncertaintyTypeName = std::string(kOneSigma);
  s.data_point.apparentAgeUncertaintyUnitName = std::string(kAbsoluteUncertainty);
  s.data_point.jvalueUncertaintyUnitName = std::string(kAbsoluteUncertainty);
  Aliquot aliquot;
  aliquot.aliquotName = "20001-01";
  s.aliquots.push_back(aliquot);
  for (int i = 0; i < measurements; ++i) {
    Measurement m;
    m.aliquotName = "20001-01";
    m.stepNumber = i + 1;
    m.apparentAge = 28.2;
    m.apparentAgeUncertainityTypeName = std::string(kOneSigma);
    m.jvalueUncertaintyTypeName = std::string(kOneSigma);
    m.plateauStepName = std::string(i == 0 ? kNo : kYes);
    s.measurements.push_back(m);
  }
  s.age_calc.decayConstantName = "Min et al. 2000";
  s.age_calc.airRatioName = "Lee et al. 2006";
  s.age_calc.fluxMonitorName = flux_monitor_query("FC-2", 28.201);
  s.age_summary.age = 28.2;
  s.age_summary.ageCalcTypeName = std::string(age_type_name(AgeKind::Plateau));
  s.age_summary.ageUncertaintyTypeName = std::string(kOneSigma);
  s.age_summary.calculatedAgeUncertaintyTypeName = std::string(kOneSigma);
  s.age_summary.calculatedAgeUncertaintyTypeUnitsName = std::string(kAbsoluteUncertainty);
  s.age_summary.dataInterpretationToolName = std::string(kInterpretationTool);
  s.age_summary.interpretationName = std::string(kUnknownInterpretation);
  return s;
}

struct Desk {
  FakeEarthBank bank;
  Session session{bank, bank.base_url()};
  LookupResolver resolver{session};
  explicit Desk(bool signed_in = true) {
    if (!signed_in) return;
    const auto in = session.sign_in(bank.login(), bank.password());
    if (!in) ADD_FAILURE() << to_string(in.error());
  }
};

// (sample, record, field, value, endpoint, outcome)
using Row = std::tuple<std::string, std::string, std::string, std::string, std::string, LookupOutcome>;

std::vector<Row> rows(const std::vector<Miss>& misses) {
  std::vector<Row> out;
  out.reserve(misses.size());
  for (const Miss& miss : misses) {
    out.emplace_back(miss.sample, miss.record, miss.field, miss.value, miss.endpoint, miss.outcome);
  }
  return out;
}

std::string dump(const Store& store) {
  json out = json::object();
  out["packages"] = store.packages;
  const auto put = [&](const char* key, const std::vector<Store::Record>& records) {
    json list = json::array();
    for (const Store::Record& record : records) list.push_back({record.id, record.package_id, record.body});
    out[key] = std::move(list);
  };
  put("samples", store.samples);
  put("data_points", store.data_points);
  put("aliquots", store.aliquots);
  put("measurements", store.measurements);
  put("age_calcs", store.age_calcs);
  put("age_summaries", store.age_summaries);
  return out.dump();
}

// ---- What it reports -------------------------------------------------------

TEST(Preflight, ReportsEveryUnknownTermWithItsRecordAndEndpoint) {
  Desk desk;
  // Every term of a submission as the mapping writes it is one the service
  // knows.
  const PreflightReport clean = preflight({good("S-1")}, desk.resolver);
  EXPECT_TRUE(clean.misses.empty()) << ::testing::PrintToString(rows(clean.misses));
  EXPECT_TRUE(clean.clean());

  Submission bad = good("S-2");
  bad.sample.materialName = "Unobtainium";
  bad.sample.sampleKindName = "Moon rock";
  bad.data_point.mineralName = "Unobtainium";
  bad.data_point.arMethodName = "Step-heating - oven";
  bad.data_point.analyticalUncertaintyTypeName = "95% confidence";
  bad.measurements[1].jvalueUncertaintyTypeName = "wide";
  bad.age_calc.fluxMonitorName = "BT-1 0.7666 Ma";
  bad.age_calc.decayConstantName = "Example 1999";
  bad.age_summary.interpretationName = "A guess";
  bad.age_summary.ageCalcTypeName = "Plateau";

  const PreflightReport report = preflight({good("S-1"), bad}, desk.resolver);
  EXPECT_FALSE(report.clean());
  const auto unknown = LookupOutcome::Unknown;
  // In the order the records are sent, a record's fields in the order of the
  // service's record.
  EXPECT_EQ(rows(report.misses),
            (std::vector<Row>{
                {"S-2", "Sample", "materialName", "Unobtainium", "/api/core/materials", unknown},
                {"S-2", "Sample", "sampleKindName", "Moon rock", "/api/core/l-sample-kinds", unknown},
                {"S-2", "ArArDataPoint", "analyticalUncertaintyTypeName", "95% confidence", "", unknown},
                {"S-2", "ArArDataPoint", "arMethodName", "Step-heating - oven", "/api/arar/LArArMethod", unknown},
                {"S-2", "ArArDataPoint", "mineralName", "Unobtainium", "/api/core/materials", unknown},
                {"S-2", "ArArMeasurement", "jvalueUncertaintyTypeName", "wide", "", unknown},
                {"S-2", "ArArAgeCalc", "decayConstantName", "Example 1999", "/api/arar/DecayConstant", unknown},
                {"S-2", "ArArAgeCalc", "fluxMonitorName", "BT-1 0.7666 Ma", "/api/arar/FluxMonitor", unknown},
                {"S-2", "ArArAgeSummary", "ageCalcTypeName", "Plateau", "/api/arar/LAgeType", unknown},
                {"S-2", "ArArAgeSummary", "interpretationName", "A guess", "/api/arar/LArArInterpretation", unknown},
            }));
  for (const Miss& miss : report.misses) {
    EXPECT_FALSE(miss.detail.empty()) << miss.field;
    EXPECT_EQ(miss.count, 1u) << miss.field;
  }
  EXPECT_EQ(report.unknown().size(), 10u);
  EXPECT_TRUE(report.could_not_ask().empty());
}

// A term whose id is already beside it is not asked about, and nor is a field
// that is empty.
TEST(Preflight, ATermWithItsIdIsNotAsked) {
  Desk desk;
  Submission s = good("S-1");
  s.data_point.mineralName = "Unobtainium";
  s.data_point.mineralId = 38433;
  s.sample.materialName = "";
  s.sample.archiveName = std::nullopt;
  s.data_point.analyticalUncertaintyTypeName = "95% confidence";
  s.data_point.analyticalUncertaintyTypeId = 2;
  const PreflightReport report = preflight({s}, desk.resolver);
  EXPECT_TRUE(report.misses.empty()) << ::testing::PrintToString(rows(report.misses));
  EXPECT_EQ(desk.bank.count("GET", "/api/core/materials"), 0u);
}

// The same miss on every measurement of a sample is one line, with how many.
TEST(Preflight, AMissOnManyRecordsOfASampleIsOneLine) {
  Desk desk;
  Submission s = good("S-1", 40);
  for (Measurement& m : s.measurements) m.apparentAgeUncertainityTypeName = "3 sigma";
  s.measurements[7].apparentAgeUncertainityTypeName = "4 sigma";
  const PreflightReport report = preflight({s, good("S-2")}, desk.resolver);
  ASSERT_EQ(report.misses.size(), 2u);
  EXPECT_EQ(report.misses[0].record, "ArArMeasurement");
  EXPECT_EQ(report.misses[0].field, "apparentAgeUncertainityTypeName");
  EXPECT_EQ(report.misses[0].value, "3 sigma");
  EXPECT_EQ(report.misses[0].count, 39u);
  EXPECT_EQ(report.misses[1].value, "4 sigma");
  EXPECT_EQ(report.misses[1].count, 1u);
}

// ---- What it took a value for ----------------------------------------------

// (sample, record, field, value, matched_name, count)
using Taken = std::tuple<std::string, std::string, std::string, std::string, std::string, std::size_t>;

std::vector<Taken> taken(const std::vector<Resolved>& resolved) {
  std::vector<Taken> out;
  out.reserve(resolved.size());
  for (const Resolved& one : resolved) {
    out.emplace_back(one.sample, one.record, one.field, one.value, one.matched_name, one.count);
  }
  return out;
}

// An id that is written cannot be corrected: where the entry found is not
// the value as it was written (but for its case), the report says which
// entry it was, once for each value, so that the user sees it before the
// upload and not after.
TEST(Preflight, SaysWhatAValueWasTakenFor) {
  Desk desk;
  const char* const kuiper = "Fish Canyon Tuff Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)";
  Submission first = good("S-1", 5);
  first.sample.materialName = "sanidine";    // another case: the same name
  first.data_point.arMethodName = " step-heating - LASER ";
  first.sample.archiveName = "Unknown";      // on the list twice
  for (Measurement& m : first.measurements) m.jvalueUncertaintyTypeName = "SEM";
  Submission second = good("S-2", 2);
  second.sample.archiveName = "unknown";
  second.age_calc.fluxMonitorName = "FC-2 Renne et al. 2011";
  second.data_point.mineralName = "Unobtainium";
  for (Measurement& m : second.measurements) m.jvalueUncertaintyTypeName = "SEM";
  second.age_summary.ageUncertaintyTypeName = "2s";

  const PreflightReport report = preflight({first, second, good("S-3")}, desk.resolver);
  // In the order first met; the sample is the first that has the value, and
  // the count is of records, over every sample.
  EXPECT_EQ(taken(report.resolved),
            (std::vector<Taken>{
                {"S-1", "Sample", "archiveName", "Unknown", "Unknown", 1},
                {"S-1", "ArArMeasurement", "jvalueUncertaintyTypeName", "SEM", "1 sigma", 7},
                {"S-1", "ArArAgeCalc", "fluxMonitorName", "FC-2 28.201 Ma", kuiper, 2},
                {"S-2", "Sample", "archiveName", "unknown", "Unknown", 1},
                {"S-2", "ArArAgeCalc", "fluxMonitorName", "FC-2 Renne et al. 2011",
                 "Fish Canyon Tuff Sanidine (FCTs) from Renne et al. 2011 (28.294 Ma)", 1},
                {"S-2", "ArArAgeSummary", "ageUncertaintyTypeName", "2s", "2 sigma", 1},
            }));
  // Why, where there is more to it than another spelling.
  EXPECT_EQ(report.resolved[0].detail,
            "EarthBank's list has 'Unknown' 2 times, under the ids 2401 and 2419; the lowest was taken.");
  EXPECT_EQ(report.resolved[1].detail, "");
  EXPECT_EQ(report.resolved[2].detail, "");
  // It is no miss: the report is as clean as its misses.
  EXPECT_EQ(report.misses.size(), 1u);
  EXPECT_EQ(report.misses[0].field, "mineralName");
  PreflightReport none = preflight({good("S-3")}, desk.resolver);
  EXPECT_TRUE(none.clean());
  EXPECT_EQ(none.resolved.size(), 1u);
  none.resolved.clear();
  EXPECT_TRUE(none.clean());
}

// What the check cannot check is said, where a submission has a value there,
// so that a clean report is not read as more than it is.
TEST(Preflight, SaysWhatItDidNotCheck) {
  Desk desk;
  Submission bare = good("S-1");
  bare.data_point.lithologyName = std::nullopt;
  const PreflightReport nothing = preflight({bare, bare}, desk.resolver);
  EXPECT_TRUE(nothing.not_checked.empty());
  EXPECT_TRUE(nothing.clean());

  Submission with_setup = bare;
  with_setup.setup_name = "Example Lab ARGUS VI";
  Submission with_all = good("S-2");
  with_all.setup_name = "Example Lab ARGUS VI";
  with_all.irradiation_name = "NM-300 (example)";
  const PreflightReport report = preflight({bare, with_setup, with_all, with_all}, desk.resolver);
  ASSERT_EQ(report.not_checked.size(), 3u);
  // One line a field, however many samples have it.
  EXPECT_EQ(report.not_checked[0].field, "setup_name");
  EXPECT_EQ(report.not_checked[0].reason,
            "EarthBank's list of analytical set-ups cannot be asked: whether it has one of this name is not checked.");
  EXPECT_EQ(report.not_checked[1].field, "irradiation_name");
  EXPECT_EQ(report.not_checked[1].reason,
            "EarthBank's list of irradiations cannot be asked: whether it has one of this name is not checked.");
  EXPECT_EQ(report.not_checked[2].field, "lithologyName");
  EXPECT_EQ(report.not_checked[2].reason,
            "The lithology is sent by its name, and no list for it is known: whether EarthBank takes the name is not "
            "checked.");
  // Not a miss.
  EXPECT_TRUE(report.clean());
  const PreflightReport only_setup = preflight({with_setup}, desk.resolver);
  ASSERT_EQ(only_setup.not_checked.size(), 1u);
  EXPECT_EQ(only_setup.not_checked[0].field, "setup_name");
}

// ---- Every term of every record --------------------------------------------

#define PYCHRON_TEST_NAME_ONLY(kind, name) name_only(record.name);
void name_only(Text& field) { field = "no such term"; }
void name_only(Number&) {}
void name_only(std::optional<std::int64_t>&) {}  // an id (or a count) stays unset
void name_only(Flag&) {}
template <typename Record>
Record every_text_set();
#define PYCHRON_TEST_EVERY_TEXT(Record, FIELDS) \
  template <>                                    \
  Record every_text_set<Record>() {              \
    Record record;                               \
    FIELDS(PYCHRON_TEST_NAME_ONLY)               \
    return record;                               \
  }
PYCHRON_TEST_EVERY_TEXT(Sample, PYCHRON_EARTHBANK_SAMPLE_FIELDS)
PYCHRON_TEST_EVERY_TEXT(Location, PYCHRON_EARTHBANK_LOCATION_FIELDS)
PYCHRON_TEST_EVERY_TEXT(DataPoint, PYCHRON_EARTHBANK_DATA_POINT_FIELDS)
PYCHRON_TEST_EVERY_TEXT(Aliquot, PYCHRON_EARTHBANK_ALIQUOT_FIELDS)
PYCHRON_TEST_EVERY_TEXT(Measurement, PYCHRON_EARTHBANK_MEASUREMENT_FIELDS)
PYCHRON_TEST_EVERY_TEXT(AgeCalc, PYCHRON_EARTHBANK_AGE_CALC_FIELDS)
PYCHRON_TEST_EVERY_TEXT(AgeSummary, PYCHRON_EARTHBANK_AGE_SUMMARY_FIELDS)
#undef PYCHRON_TEST_EVERY_TEXT
#undef PYCHRON_TEST_NAME_ONLY

// The `<term>Name` fields of a record that has `<term>Id` too, from the keys
// the record can emit: found here without the table under test.
std::set<std::pair<std::string, std::string>> pairs_of(const std::string& record, const json& fields) {
  std::set<std::pair<std::string, std::string>> out;
  for (const auto& item : fields.items()) {
    const std::string& key = item.key();
    if (key.size() <= 4 || !key.ends_with("Name")) continue;
    if (fields.contains(key.substr(0, key.size() - 4) + "Id")) out.emplace(record, key);
  }
  return out;
}

// A field added to the model later, with an id beside it, is a term until
// someone says it is not: this fails rather than let it pass unchecked.
TEST(Preflight, EveryNameWithAnIdBesideItIsChecked) {
  std::set<std::pair<std::string, std::string>> expected;
  for (const auto& [record, fields] :
       {std::pair<std::string, json>{"Sample", all_fields<Sample>()},
        {"Location", all_fields<Location>()},
        {"ArArDataPoint", all_fields<DataPoint>()},
        {"ArArAliquot", all_fields<Aliquot>()},
        {"ArArMeasurement", all_fields<Measurement>()},
        {"ArArAgeCalc", all_fields<AgeCalc>()},
        {"ArArAgeSummary", all_fields<AgeSummary>()}}) {
    expected.merge(pairs_of(record, fields));
  }
  ASSERT_GT(expected.size(), 30u);

  std::set<std::pair<std::string, std::string>> listed;
  std::map<TermKind, std::set<std::string>> by_kind;
  for (const TermField& term : term_fields()) {
    EXPECT_TRUE(listed.emplace(term.record, term.name_field).second) << term.record << "." << term.name_field;
    by_kind[term.kind].insert(std::string(term.name_field));
    EXPECT_EQ(std::string(term.id_field),
              std::string(term.name_field.substr(0, term.name_field.size() - 4)) + "Id");
    EXPECT_EQ(term.endpoint.empty(), term.kind != TermKind::Listed) << term.name_field;
  }
  EXPECT_EQ(listed, expected);
  // Not one is of a kind nobody decided.
  EXPECT_TRUE(by_kind[TermKind::Unlisted].empty()) << ::testing::PrintToString(by_kind[TermKind::Unlisted]);
  // What is not a term, though it has an id beside it: the name given to the
  // sample's place (its id is the place's own row), and the lithology, which
  // legacy sent by name and never looked up.
  EXPECT_EQ(by_kind[TermKind::FreeText], (std::set<std::string>{"lithologyName", "locationName"}));
  EXPECT_EQ(by_kind[TermKind::ByName],
            (std::set<std::string>{"inverseIsoStepName", "plateauStepName", "weightedMeanStepName"}));
  EXPECT_EQ(by_kind[TermKind::Fixed].size(), uncertainty_type_specs().size());

  // And each is in fact checked: with every text field of every record set to
  // a name no list has, there is a miss for every term that is looked up or
  // fixed, on its record, and for nothing else.
  Submission s;
  s.sample = every_text_set<Sample>();
  s.sample.name = "S-1";
  s.location = every_text_set<Location>();
  s.data_point = every_text_set<DataPoint>();
  s.aliquots = {every_text_set<Aliquot>()};
  s.measurements = {every_text_set<Measurement>()};
  s.age_calc = every_text_set<AgeCalc>();
  s.age_summary = every_text_set<AgeSummary>();
  Desk desk;
  const PreflightReport report = preflight({s}, desk.resolver);
  std::set<std::pair<std::string, std::string>> missed, wanted;
  for (const Miss& miss : report.misses) {
    EXPECT_EQ(miss.outcome, LookupOutcome::Unknown) << miss.field << ": " << miss.detail;
    EXPECT_EQ(miss.value, "no such term");
    EXPECT_TRUE(missed.emplace(miss.record, miss.field).second) << miss.record << "." << miss.field;
  }
  for (const TermField& term : term_fields()) {
    if (term.kind == TermKind::Listed || term.kind == TermKind::Fixed) {
      wanted.emplace(term.record, term.name_field);
    }
  }
  EXPECT_EQ(missed, wanted);
  EXPECT_GT(wanted.size(), 25u);
}

// ---- Could not ask ---------------------------------------------------------

TEST(Preflight, CouldNotAskIsSeparateFromUnknown) {
  // No login: the lists that want one cannot say whether they hold a name.
  Desk desk(false);
  Submission s = good("S-1");
  s.sample.sampleKindName = "Ore";             // on its list, which is closed
  s.sample.sampleMethodName = "No such way";   // not on its list, which is closed too
  s.data_point.mineralName = "Unobtainium";    // not on a list that is open
  desk.bank.lose_next("GET", "/api/arar/AirRatio");
  const PreflightReport report = preflight({s}, desk.resolver);
  EXPECT_FALSE(report.clean());

  const std::vector<Miss> could_not = report.could_not_ask();
  ASSERT_EQ(could_not.size(), 3u) << ::testing::PrintToString(rows(report.misses));
  EXPECT_EQ(could_not[0].field, "sampleKindName");
  EXPECT_EQ(could_not[0].value, "Ore");
  EXPECT_EQ(could_not[0].endpoint, "/api/core/l-sample-kinds");
  EXPECT_EQ(could_not[0].detail, "EarthBank wants a login for this list.");
  EXPECT_EQ(could_not[1].field, "sampleMethodName");
  EXPECT_EQ(could_not[2].field, "airRatioName");
  EXPECT_EQ(could_not[2].detail, "EarthBank did not answer (list of terms). Check the network and try again.");
  for (const Miss& miss : could_not) EXPECT_EQ(miss.outcome, LookupOutcome::CouldNotAsk);

  const std::vector<Miss> unknown = report.unknown();
  ASSERT_EQ(unknown.size(), 1u);
  EXPECT_EQ(unknown[0].field, "mineralName");
  EXPECT_EQ(unknown[0].outcome, LookupOutcome::Unknown);
  EXPECT_EQ(report.misses.size(), 4u);

  // With the login, only what the service does not know is left.
  Desk in;
  const PreflightReport signed_in = preflight({s}, in.resolver);
  EXPECT_TRUE(signed_in.could_not_ask().empty());
  EXPECT_EQ(rows(signed_in.unknown()),
            (std::vector<Row>{{"S-1", "Sample", "sampleMethodName", "No such way", "/api/core/l-sample-methods",
                               LookupOutcome::Unknown},
                              {"S-1", "ArArDataPoint", "mineralName", "Unobtainium", "/api/core/materials",
                               LookupOutcome::Unknown}}));
}

// A miss carries the resolver's sentence, and with it whatever the service
// said: no secret of the session is in one.
TEST(Preflight, NoSecretInAMiss) {
  FakeEarthBank::Options options;
  options.password = "Zq7pre;Zq7post";
  FakeEarthBank bank(options);
  testing::Spy spy(bank);
  Session session(spy, bank.base_url());
  ASSERT_TRUE(session.sign_in(bank.login(), options.password));
  LookupResolver resolver(session);
  for (const char* list : {"/api/arar/AirRatio", "/api/core/materials", "/api/arar/FluxMonitor"}) {
    bank.fail_next("GET", list, 400,
                   json{{"title", "Bad Request"},
                        {"detail", "token " + spy.token() + " and password " + options.password + "; more"}}
                       .dump());
  }
  const PreflightReport report = preflight({good("S-1")}, resolver);
  ASSERT_EQ(report.could_not_ask().size(), 4u) << ::testing::PrintToString(rows(report.misses));
  for (const Miss& miss : report.misses) {
    EXPECT_EQ(miss.detail, "EarthBank refused the list of terms: token <redacted> and password <redacted>");
    EXPECT_EQ(miss.detail.find("Zq7"), std::string::npos) << miss.detail;
    for (const std::string& secret : spy.secrets()) EXPECT_EQ(miss.detail.find(secret), std::string::npos) << miss.detail;
  }
}

// ---- It only asks ----------------------------------------------------------

TEST(Preflight, WritesNothing) {
  for (const bool signed_in : {true, false}) {
    Desk desk(signed_in);
    const std::string before = dump(desk.bank.store());
    const std::size_t logins = desk.bank.requests().size();
    Submission bad = good("S-2");
    bad.data_point.mineralName = "Unobtainium";
    bad.sample.sampleKindName = "Ore";
    // No package is named anywhere: none is needed to ask.
    const PreflightReport report = preflight({good("S-1"), bad}, desk.resolver);
    EXPECT_FALSE(report.clean());

    EXPECT_EQ(dump(desk.bank.store()), before);
    const std::vector<LoggedRequest> log = desk.bank.requests();
    ASSERT_GT(log.size(), logins + 5);
    for (std::size_t i = logins; i < log.size(); ++i) {
      EXPECT_EQ(log[i].method, "GET") << log[i];
      EXPECT_TRUE(log[i].body.empty()) << log[i];
      EXPECT_FALSE(log[i].path.starts_with("/api/management")) << log[i];
    }
    // The submissions are read, not filled in: an id is the upload's to set.
    EXPECT_FALSE(bad.data_point.arMethodId.has_value());
  }
}

TEST(Preflight, ManySamplesAskForEachTermOnce) {
  Desk desk;
  std::vector<Submission> many;
  for (int i = 0; i < 400; ++i) {
    Submission s = good("S-" + std::to_string(1000 + i), 12);
    if (i % 2 == 1) s.data_point.mineralName = "Biotite";
    if (i % 100 == 7) s.sample.materialName = "Unobtainium";
    many.push_back(std::move(s));
  }
  const PreflightReport report = preflight(many, desk.resolver);
  // One line for each sample that has the unknown material.
  ASSERT_EQ(report.misses.size(), 4u);
  EXPECT_EQ(report.misses[0].sample, "S-1007");
  EXPECT_EQ(report.misses[3].sample, "S-1307");

  // 4800 measurements and 400 samples: each list read once, each name asked
  // for once.
  std::map<std::string, std::size_t> asked;
  for (const LoggedRequest& request : desk.bank.requests()) {
    if (request.method == "GET") ++asked[request.path];
  }
  EXPECT_EQ(asked, (std::map<std::string, std::size_t>{
                       {"/api/arar/AirRatio", 1},
                       {"/api/arar/DecayConstant", 1},
                       {"/api/arar/FluxMonitor", 4},  // the list, and the monitor's query by name three ways
                       {"/api/arar/LAgeType", 1},
                       {"/api/arar/LArArDataInterpretationTool", 1},
                       {"/api/arar/LArArInterpretation", 1},
                       {"/api/arar/LArArMethod", 1},
                       {"/api/arar/LUncertaintyUnit", 1},
                       {"/api/core/materials", 4},  // Sanidine, Biotite, and Unobtainium twice (equals, contains)
                   }));

  // Again with the same resolver: nothing more is asked.
  const std::size_t sent = desk.bank.requests().size();
  EXPECT_EQ(preflight(many, desk.resolver).misses.size(), 4u);
  EXPECT_EQ(desk.bank.requests().size(), sent);
}

// ---- The mapping's notes ---------------------------------------------------

TEST(Preflight, TheMappingsNotesArePassedOn) {
  Desk desk;
  Submission first = good("S-1");
  first.notes = {"Plateau age of steps C to H (6 of 10, 78.2% of the 39ArK).", "No analytical set-up is named."};
  Submission second = good("S-2");
  Submission third = good("S-3");
  third.notes = {"1 analysis without an age was left out."};
  third.data_point.mineralName = "Unobtainium";
  const PreflightReport report = preflight({first, second, third}, desk.resolver);
  // As they were written, by sample, and no sample without one.
  ASSERT_EQ(report.notes.size(), 2u);
  EXPECT_EQ(report.notes[0].sample, "S-1");
  EXPECT_EQ(report.notes[0].notes, first.notes);
  EXPECT_EQ(report.notes[1].sample, "S-3");
  EXPECT_EQ(report.notes[1].notes, third.notes);
  // A note is not a miss.
  EXPECT_EQ(report.misses.size(), 1u);
  EXPECT_FALSE(report.clean());
  EXPECT_TRUE(preflight({first}, desk.resolver).clean());
  // Nothing to check is nothing found.
  EXPECT_TRUE(preflight({}, desk.resolver).clean());
}

// A sample with no name is still told from the others.
TEST(Preflight, ASampleIsNamedByItsDataPointWhenItHasNoName) {
  Desk desk;
  Submission s = good("S-1");
  s.sample.name = std::nullopt;
  s.datapoint_name = "The data point";
  s.data_point.mineralName = "Unobtainium";
  const PreflightReport report = preflight({s}, desk.resolver);
  ASSERT_EQ(report.misses.size(), 1u);
  EXPECT_EQ(report.misses[0].sample, "The data point");
}

}  // namespace
}  // namespace pychron::earthbank
