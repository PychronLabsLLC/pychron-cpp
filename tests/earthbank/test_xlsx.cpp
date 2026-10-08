// Submissions written into the vendor's two workbooks
// (pychron/earthbank/xlsx.hpp): every value under the column that is its
// own, the rows of a data point tied together by its name, and everything
// that is not a data row left as the template has it.
//
// The census at the top is the test's own statement of which column takes
// which field. It is written out here, not read from the exporter, so that a
// change to either the exporter's table or a template shows up as a failure
// and not as a workbook with a value in another column.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "analyses.hpp"
#include "ooxml.hpp"
#include "pychron/core/error.hpp"
#include "pychron/earthbank/mapping.hpp"
#include "pychron/earthbank/model.hpp"
#include "pychron/earthbank/xlsx.hpp"
#include "records_support.hpp"
#include "zip_support.hpp"

namespace pychron::earthbank {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using testing::all_fields;

const char* const kArAr = "ArArDataPoint_Template2026.xlsx";
const char* const kSample = "Sample_Template_v2025-04-16.xlsx";
const char* const kStamp = "20261007-010203";

fs::path template_dir() { return fs::path(PYCHRON_EARTHBANK_TEMPLATE_DIR); }

// ---- The census -------------------------------------------------------------

using SheetKey = std::pair<std::string, std::string>;  // sheet, the name in row 4
using RecordField = std::pair<std::string, std::string>;

// The record a sheet's rows are of.
const std::map<std::string, std::string> kOwnRecord = {
    {"ArArDataPoints", "DataPoint"},  {"ArArMeasurement", "Measurement"}, {"ArArAliquot", "Aliquot"},
    {"ArArAgeSummary", "AgeSummary"}, {"ArArAgeCalc", "AgeCalc"},         {"Samples", "Sample"},
};

const std::vector<std::string> kArArSheets = {"ArArDataPoints", "ArArMeasurement", "ArArAliquot", "ArArAgeSummary",
                                              "ArArAgeCalc"};

// Every column that does not take its own record's field of the column's
// name. All the others do.
const std::map<SheetKey, RecordField> kNamedOtherwise = {
    {{"ArArDataPoints", "datapointName"}, {"Submission", "datapoint_name"}},
    {{"ArArDataPoints", "sampleName"}, {"Sample", "name"}},
    {{"ArArDataPoints", "arArSetUp"}, {"Submission", "setup_name"}},
    {{"ArArDataPoints", "arArMethod"}, {"DataPoint", "arMethodName"}},
    {{"ArArDataPoints", "irradiationBatchID"}, {"Submission", "irradiation_name"}},
    {{"ArArDataPoints", "analysisScale"}, {"DataPoint", "analysisScaleName"}},
    {{"ArArDataPoints", "lithology"}, {"DataPoint", "lithologyName"}},
    {{"ArArDataPoints", "mineral"}, {"DataPoint", "mineralName"}},
    {{"ArArDataPoints", "analyticalUncertaintyType"}, {"DataPoint", "analyticalUncertaintyTypeName"}},
    {{"ArArDataPoints", "analyticalUncertaintyUnit"}, {"DataPoint", "analyticalUncertaintyUnitName"}},
    {{"ArArDataPoints", "apparentAgeUncertaintyType"}, {"DataPoint", "apparentAgeUncertaintyTypeName"}},
    {{"ArArDataPoints", "apparentAgeUncertaintyUnit"}, {"DataPoint", "apparentAgeUncertaintyUnitName"}},
    {{"ArArDataPoints", "jValueUncertaintyType"}, {"Measurement", "jvalueUncertaintyTypeName"}},
    {{"ArArDataPoints", "jValueUncertaintyUnit"}, {"DataPoint", "jvalueUncertaintyUnitName"}},
    {{"ArArDataPoints", "datapoint.literature"}, {"DataPoint", "literatureName"}},
    {{"ArArDataPoints", "datapoint.funding"}, {"DataPoint", "fundingName"}},

    {{"ArArMeasurement", "datapointName"}, {"Submission", "datapoint_name"}},
    {{"ArArMeasurement", "Ar39KReleased"}, {"Measurement", "ar39KReleased"}},
    {{"ArArMeasurement", "plateauStep"}, {"Measurement", "plateauStepName"}},
    {{"ArArMeasurement", "inverseIsoStep"}, {"Measurement", "inverseIsoStepName"}},
    {{"ArArMeasurement", "weightedMeanStep"}, {"Measurement", "weightedMeanStepName"}},

    {{"ArArAliquot", "datapointName"}, {"Submission", "datapoint_name"}},

    {{"ArArAgeSummary", "datapointName"}, {"Submission", "datapoint_name"}},
    {{"ArArAgeSummary", "dataInterpretationTool"}, {"AgeSummary", "dataInterpretationToolName"}},
    {{"ArArAgeSummary", "ageCalcType"}, {"AgeSummary", "ageCalcTypeName"}},
    {{"ArArAgeSummary", "calculatedAgeUncertaintyType"}, {"AgeSummary", "calculatedAgeUncertaintyTypeName"}},
    {{"ArArAgeSummary", "calculatedAgeUncertaintyTypeUnits"}, {"AgeSummary", "calculatedAgeUncertaintyTypeUnitsName"}},
    {{"ArArAgeSummary", "interpretation"}, {"AgeSummary", "interpretationName"}},

    {{"ArArAgeCalc", "datapointName"}, {"Submission", "datapoint_name"}},
    {{"ArArAgeCalc", "decayConstant"}, {"AgeCalc", "decayConstantName"}},
    {{"ArArAgeCalc", "airRatio"}, {"AgeCalc", "airRatioName"}},
    {{"ArArAgeCalc", "fluxMonitor"}, {"AgeCalc", "fluxMonitorName"}},

    {{"Samples", "sampleName"}, {"Sample", "name"}},
    {{"Samples", "sampleKind"}, {"Sample", "sampleKindName"}},
    {{"Samples", "sampleMethod"}, {"Sample", "sampleMethodName"}},
    {{"Samples", "material"}, {"Sample", "materialName"}},
    {{"Samples", "sampleComment"}, {"Sample", "description"}},
    {{"Samples", "latitude"}, {"Location", "lat"}},
    {{"Samples", "longitude"}, {"Location", "lon"}},
    {{"Samples", "latLonPrecision"}, {"Location", "latLonPrecision"}},
    {{"Samples", "elevationGround"}, {"Sample", "referenceElevation"}},
    {{"Samples", "elevationDepthMin"}, {"Sample", "relativeElevationMin"}},
    {{"Samples", "elevationDepthMax"}, {"Sample", "relativeElevationMax"}},
    {{"Samples", "locationKind"}, {"Sample", "locationKindName"}},
    {{"Samples", "unitName"}, {"Sample", "stratographicUnitName"}},
    {{"Samples", "ageMin"}, {"Sample", "rockUnitAgeMin"}},
    {{"Samples", "ageMax"}, {"Sample", "rockUnitAgeMax"}},
    {{"Samples", "unitAgeDescription"}, {"Sample", "rockUnitAgeDescription"}},
    {{"Samples", "lastKnownArchive"}, {"Sample", "archiveName"}},
    {{"Samples", "funding"}, {"Sample", "fundingName"}},
    {{"Samples", "literature"}, {"Sample", "literatureName"}},
};

// The columns nothing fills: no record has the thing they ask for.
const std::set<SheetKey> kNotFilled = {
    {"ArArDataPoints", "description"},
    {"Samples", "elevationDepthAccuracy"},
    {"Samples", "elevationNote"},
    {"Samples", "locationComment"},
    {"Samples", "person"},
    {"Samples", "personRole"},
    {"Samples", "archiveComment"},
    {"Samples", "Tag"},
};

// The fields no column takes. Beside these, no `id` and no `*Id` has one.
const std::set<RecordField> kNoColumn = {
    {"Sample", "sampleID"},                             // the name again
    {"Location", "description"},
    {"Location", "name"},
    {"Measurement", "analysisUnits"},                   // the data point's column says it
    {"Measurement", "apparentAgeUncertainityTypeName"},  // likewise
    {"Measurement", "ratio40Arx36ArTrapped"},
    {"Measurement", "ratio40Arx36ArTrappedUncertainty"},
    {"AgeCalc", "comment"},
    {"AgeSummary", "ageUncertaintyTypeName"},  // calculatedAgeUncertaintyType says it
};

bool is_id(std::string_view field) { return field == "id" || field.ends_with("Id"); }

json fields_of(const std::string& record) {
  if (record == "Sample") return all_fields<Sample>();
  if (record == "Location") return all_fields<Location>();
  if (record == "DataPoint") return all_fields<DataPoint>();
  if (record == "Aliquot") return all_fields<Aliquot>();
  if (record == "Measurement") return all_fields<Measurement>();
  if (record == "AgeCalc") return all_fields<AgeCalc>();
  if (record == "AgeSummary") return all_fields<AgeSummary>();
  ADD_FAILURE() << "no record " << record;
  return json::object();
}

// Where the census says the column takes its value; nothing for a column
// that is left empty.
std::optional<RecordField> source_of(const std::string& sheet, const std::string& key) {
  if (const auto it = kNamedOtherwise.find({sheet, key}); it != kNamedOtherwise.end()) return it->second;
  if (kNotFilled.contains({sheet, key})) return std::nullopt;
  const std::string& own = kOwnRecord.at(sheet);
  if (fields_of(own).contains(key)) return RecordField{own, key};
  ADD_FAILURE() << sheet << "." << key << " is in no part of the census";
  return std::nullopt;
}

// ---- Reading a workbook back --------------------------------------------------

using Row = std::map<std::string, std::string>;  // the name in row 4 -> the cell's text, cells that hold something

ooxml::Workbook open(const fs::path& file) {
  auto book = ooxml::Workbook::open(file);
  if (!book) throw std::runtime_error("cannot open " + file.string() + ": " + to_string(book.error()));
  return std::move(book.value());
}

std::vector<std::string> row_of(const ooxml::Workbook& book, const std::string& sheet, int row) {
  auto values = book.row_values(sheet, row);
  if (!values) throw std::runtime_error(sheet + ": " + to_string(values.error()));
  return *values;
}

// The data rows of `sheet`, from row 5 to the first row that holds nothing.
std::vector<Row> rows_of(const fs::path& file, const std::string& sheet) {
  const ooxml::Workbook book = open(file);
  const std::vector<std::string> keys = row_of(book, sheet, 4);
  std::vector<Row> out;
  for (int row = 5;; ++row) {
    const std::vector<std::string> cells = row_of(book, sheet, row);
    Row values;
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (cells[i].empty()) continue;
      if (i >= keys.size() || keys[i].empty()) {
        ADD_FAILURE() << sheet << " row " << row << ": a value under a column without a name";
        continue;
      }
      values[keys[i]] = cells[i];
    }
    if (values.empty()) return out;
    out.push_back(std::move(values));
  }
}

std::string slurp(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void spit(const fs::path& file, std::string_view bytes) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

long process_id() {
#ifdef _WIN32
  return static_cast<long>(_getpid());
#else
  return static_cast<long>(getpid());
#endif
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  if (*value == '\0') {
    unsetenv(name);
  } else {
    setenv(name, value, 1);
  }
#endif
}

// ---- What a cell should hold --------------------------------------------------

// The value as the workbook shows it: a time to the minute in the
// measurement sheet, a collection date day first, a flag as Yes or No.
json shown(const std::string& sheet, const std::string& key, const json& value) {
  if (value.is_boolean()) return value.get<bool>() ? "Yes" : "No";
  if (!value.is_string()) return value;
  const std::string text = value.get<std::string>();
  if (sheet == "ArArMeasurement" && key == "analysisTime") {
    return text.substr(0, 10) + " " + text.substr(11, 5) + " UTC";
  }
  if (sheet == "Samples" && (key == "collectDateMin" || key == "collectDateMax")) {
    return text.substr(8, 2) + "." + text.substr(5, 2) + "." + text.substr(0, 4);
  }
  return value;
}

// The row the census expects for one record of `submission` in `sheet`.
std::map<std::string, json> expected_row(const std::vector<std::string>& keys, const std::string& sheet,
                                         const Submission& submission, const json& own) {
  std::map<std::string, json> out;
  for (const std::string& key : keys) {
    if (key.empty()) continue;
    const auto source = source_of(sheet, key);
    if (!source) continue;
    json value;
    if (source->first == "Submission") {
      // What a submission carries beside its records; one not known is empty.
      const std::string& text = source->second == "datapoint_name" ? submission.datapoint_name
                                : source->second == "setup_name"   ? submission.setup_name
                                                                   : submission.irradiation_name;
      if (!text.empty()) value = text;
    } else if (source->first == kOwnRecord.at(sheet)) {
      if (own.contains(source->second)) value = own.at(source->second);
    } else if (source->first == "Sample") {
      const json sample = to_json(submission.sample);
      if (sample.contains(source->second)) value = sample.at(source->second);
    } else if (source->first == "Location") {
      const json location = submission.location ? to_json(*submission.location) : json::object();
      if (location.contains(source->second)) value = location.at(source->second);
    } else if (source->first == "Measurement") {
      // Of the data point: what its measurements all say.
      std::set<json> said;
      for (const Measurement& m : submission.measurements) {
        const json one = to_json(m);
        if (one.contains(source->second)) said.insert(one.at(source->second));
      }
      if (said.size() == 1) value = *said.begin();
    } else {
      ADD_FAILURE() << sheet << "." << key << ": the census names " << source->first;
    }
    if (!value.is_null()) out[key] = shown(sheet, key, value);
  }
  return out;
}

void expect_row(const Row& actual, const std::map<std::string, json>& expected, const std::string& where) {
  for (const auto& [key, value] : expected) {
    const auto it = actual.find(key);
    if (it == actual.end()) {
      ADD_FAILURE() << where << ": nothing under " << key << ", expected " << value.dump();
      continue;
    }
    if (value.is_number()) {
      // A number is written as the shortest digits that read back the same.
      EXPECT_EQ(std::strtod(it->second.c_str(), nullptr), value.get<double>()) << where << ": " << key;
    } else {
      EXPECT_EQ(it->second, value.get<std::string>()) << where << ": " << key;
    }
  }
  for (const auto& [key, text] : actual) {
    EXPECT_TRUE(expected.contains(key)) << where << ": " << key << " holds " << text << " and should be empty";
  }
}

// The rows of every Ar-Ar data sheet that `submissions` should have made.
void expect_arar_workbook(const fs::path& file, const std::vector<Submission>& submissions) {
  const ooxml::Workbook book = open(file);
  for (const std::string& sheet : kArArSheets) {
    const std::vector<std::string> keys = row_of(book, sheet, 4);
    std::vector<std::map<std::string, json>> expected;
    for (const Submission& s : submissions) {
      if (sheet == "ArArDataPoints") expected.push_back(expected_row(keys, sheet, s, to_json(s.data_point)));
      if (sheet == "ArArAgeSummary") expected.push_back(expected_row(keys, sheet, s, to_json(s.age_summary)));
      if (sheet == "ArArAgeCalc") expected.push_back(expected_row(keys, sheet, s, to_json(s.age_calc)));
      if (sheet == "ArArAliquot") {
        for (const Aliquot& a : s.aliquots) expected.push_back(expected_row(keys, sheet, s, to_json(a)));
      }
      if (sheet == "ArArMeasurement") {
        for (const Measurement& m : s.measurements) expected.push_back(expected_row(keys, sheet, s, to_json(m)));
      }
    }
    const std::vector<Row> actual = rows_of(file, sheet);
    ASSERT_EQ(actual.size(), expected.size()) << sheet;
    for (std::size_t i = 0; i < actual.size(); ++i) {
      expect_row(actual[i], expected[i], sheet + " row " + std::to_string(5 + i));
    }
  }
}

void expect_sample_workbook(const fs::path& file, const std::vector<Submission>& submissions) {
  const std::vector<std::string> keys = row_of(open(file), "Samples", 4);
  const std::vector<Row> actual = rows_of(file, "Samples");
  ASSERT_EQ(actual.size(), submissions.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    expect_row(actual[i], expected_row(keys, "Samples", submissions[i], to_json(submissions[i].sample)),
               "Samples row " + std::to_string(5 + i));
  }
}

std::vector<std::string> file_names(const std::vector<fs::path>& files) {
  std::vector<std::string> out;
  for (const fs::path& file : files) out.push_back(file.filename().string());
  return out;
}

std::vector<std::string> sorted(std::vector<std::string> list) {
  std::sort(list.begin(), list.end());
  return list;
}

// Records with every field set, each to a value no other field of the
// submission has: a value under a neighbour's column is then a failure.
struct Distinct {
  int next = 0;
  void fill(std::optional<double>& field) { field = 100.25 + next++; }
  void fill(std::optional<std::string>& field) { field = "text " + std::to_string(next++); }
  void fill(std::optional<std::int64_t>& field) { field = 1000 + next++; }
  void fill(std::optional<bool>& field) { field = true; }

#define PYCHRON_EARTHBANK_DISTINCT(kind, name) fill(record.name);
#define PYCHRON_EARTHBANK_ALL_DISTINCT(Record, FIELDS) \
  Record all(const Record&) {                          \
    Record record;                                     \
    FIELDS(PYCHRON_EARTHBANK_DISTINCT)                 \
    return record;                                     \
  }
  PYCHRON_EARTHBANK_ALL_DISTINCT(Sample, PYCHRON_EARTHBANK_SAMPLE_FIELDS)
  PYCHRON_EARTHBANK_ALL_DISTINCT(Location, PYCHRON_EARTHBANK_LOCATION_FIELDS)
  PYCHRON_EARTHBANK_ALL_DISTINCT(DataPoint, PYCHRON_EARTHBANK_DATA_POINT_FIELDS)
  PYCHRON_EARTHBANK_ALL_DISTINCT(Aliquot, PYCHRON_EARTHBANK_ALIQUOT_FIELDS)
  PYCHRON_EARTHBANK_ALL_DISTINCT(Measurement, PYCHRON_EARTHBANK_MEASUREMENT_FIELDS)
  PYCHRON_EARTHBANK_ALL_DISTINCT(AgeCalc, PYCHRON_EARTHBANK_AGE_CALC_FIELDS)
  PYCHRON_EARTHBANK_ALL_DISTINCT(AgeSummary, PYCHRON_EARTHBANK_AGE_SUMMARY_FIELDS)
#undef PYCHRON_EARTHBANK_ALL_DISTINCT
#undef PYCHRON_EARTHBANK_DISTINCT
};

class Xlsx : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    // Per process as well as per test: two build trees may run at once.
    dir_ = fs::temp_directory_path() /
           (std::string("pychron_xlsx_") + info->name() + "_" + std::to_string(process_id()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    out_ = dir_ / "out";
    options_.template_dir = template_dir();
    using namespace std::chrono;
    options_.now = sys_days{year{2026} / 10 / 7} + hours{1} + minutes{2} + seconds{3};
  }
  void TearDown() override { fs::remove_all(dir_); }

  // The two samples of the mapping's tests: S-FUS (six fusions, an aliquot
  // each) and S-STEP (ten steps of one aliquot, with a plateau).
  static std::vector<Submission> fixtures() {
    auto built = build_submissions(testing::both_samples(), {});
    if (!built) throw std::runtime_error(built.error().what);
    return *built;
  }

  // A submission with every field of every record set, ids too, and no two
  // to the same value, except where one field says again what another does
  // (the census's "said by" fields), which agree.
  static Submission everything() {
    Distinct distinct;
    Submission s;
    s.datapoint_name = "all of it";
    s.setup_name = "the set-up";
    s.irradiation_name = "the batch";
    s.sample = distinct.all(Sample{});
    s.sample.sampleID = s.sample.name;
    s.sample.collectDateMin = "2019-06-30";
    s.sample.collectDateMax = "2019-07-01T12:00:00Z";
    s.location = distinct.all(Location{});
    s.data_point = distinct.all(DataPoint{});
    s.data_point.analysisDate = "2024-03-05";
    s.aliquots = {distinct.all(Aliquot{}), distinct.all(Aliquot{})};
    s.measurements = {distinct.all(Measurement{}), distinct.all(Measurement{})};
    s.measurements[0].analysisTime = "2024-03-05T06:07:08Z";
    s.measurements[1].analysisTime = "2024-03-05T23:59:59Z";
    for (Measurement& m : s.measurements) {
      m.analysisUnits = s.data_point.analysisUnits;
      m.apparentAgeUncertainityTypeName = s.data_point.apparentAgeUncertaintyTypeName;
      m.jvalueUncertaintyTypeName = "1 sigma";
    }
    s.age_calc = distinct.all(AgeCalc{});
    s.age_summary = distinct.all(AgeSummary{});
    s.age_summary.ageUncertaintyTypeName = s.age_summary.calculatedAgeUncertaintyTypeName;
    return s;
  }

  XlsxResult exported(const std::vector<Submission>& submissions) const {
    auto result = export_xlsx(submissions, out_, options_);
    if (!result) throw std::runtime_error("export_xlsx: " + to_string(result.error()));
    return *result;
  }

  Error refused(const std::vector<Submission>& submissions) const {
    auto result = export_xlsx(submissions, out_, options_);
    EXPECT_FALSE(result.has_value());
    return result ? Error{} : result.error();
  }

  // Nothing was left in the output directory (which may not have been made).
  void expect_no_file() const {
    if (!fs::exists(out_)) return;
    for (const auto& entry : fs::directory_iterator(out_)) ADD_FAILURE() << "left behind: " << entry.path();
  }

  // A copy of the templates in which `edit` changed the XML of one part of
  // the Ar-Ar workbook.
  template <typename Edit>
  fs::path edited_templates(const std::string& part, Edit edit) const {
    const fs::path dir = dir_ / "templates";
    fs::create_directories(dir);
    fs::copy_file(template_dir() / kSample, dir / kSample);
    auto entries = ooxml::read_zip(template_dir() / kArAr);
    if (!entries) throw std::runtime_error(to_string(entries.error()));
    std::vector<testing::Part> parts;
    bool found = false;
    for (const ooxml::ZipEntry& entry : *entries) {
      parts.push_back({entry.name, entry.data});
      if (entry.name != part) continue;
      edit(parts.back().data);
      found = true;
    }
    EXPECT_TRUE(found) << "no part " << part;
    spit(dir / kArAr, testing::make_zip(parts));
    return dir;
  }

  fs::path dir_;
  fs::path out_;
  XlsxOptions options_;
};

// Takes the cell at `reference` out of a sheet's XML: the column loses its
// name.
void remove_cell(std::string& xml, const std::string& reference) {
  const std::size_t from = xml.find("<c r=\"" + reference + "\"");
  ASSERT_NE(from, std::string::npos) << reference;
  const std::size_t to = xml.find("</c>", from);
  ASSERT_NE(to, std::string::npos) << reference;
  xml.erase(from, to + 4 - from);
}

// ---- The census ---------------------------------------------------------------

TEST_F(Xlsx, EveryColumnAndEveryFieldIsAccountedFor) {
  const auto columns = xlsx_columns(template_dir());
  ASSERT_TRUE(columns.has_value()) << to_string(columns.error());

  // Every named column of every data sheet is the census's: taken from the
  // field it names, or left empty because the census says so.
  std::map<std::string, std::pair<int, int>> counts;  // sheet -> filled, not filled
  std::set<RecordField> taken;
  std::set<SheetKey> seen;
  for (const XlsxColumn& column : *columns) {
    ASSERT_TRUE(kOwnRecord.contains(column.sheet)) << column.sheet;
    EXPECT_TRUE(seen.insert({column.sheet, column.key}).second) << column.sheet << "." << column.key << " twice";
    const auto expected = source_of(column.sheet, column.key);
    if (!expected) {
      EXPECT_TRUE(kNotFilled.contains({column.sheet, column.key})) << column.sheet << "." << column.key;
      EXPECT_EQ(column.record, "") << column.sheet << "." << column.key;
      EXPECT_EQ(column.field, "") << column.sheet << "." << column.key;
      ++counts[column.sheet].second;
      continue;
    }
    EXPECT_EQ(column.record, expected->first) << column.sheet << "." << column.key;
    EXPECT_EQ(column.field, expected->second) << column.sheet << "." << column.key;
    if (expected->first == "Submission") {
      EXPECT_TRUE(expected->second == "datapoint_name" || expected->second == "setup_name" ||
                  expected->second == "irradiation_name")
          << column.sheet << "." << column.key;
    } else {
      EXPECT_TRUE(fields_of(expected->first).contains(expected->second))
          << column.sheet << "." << column.key << ": " << expected->first << " has no " << expected->second;
      EXPECT_FALSE(is_id(expected->second)) << column.sheet << "." << column.key;
    }
    taken.insert(*expected);
    ++counts[column.sheet].first;
  }
  // The census names no column the templates do not have.
  for (const auto& [where, source] : kNamedOtherwise) {
    EXPECT_TRUE(seen.contains(where)) << where.first << "." << where.second;
  }
  for (const SheetKey& where : kNotFilled) EXPECT_TRUE(seen.contains(where)) << where.first << "." << where.second;

  using Counts = std::map<std::string, std::pair<int, int>>;
  EXPECT_EQ(counts, (Counts{{"ArArDataPoints", {21, 1}},
                            {"ArArMeasurement", {68, 0}},
                            {"ArArAliquot", {5, 0}},
                            {"ArArAgeSummary", {13, 0}},
                            {"ArArAgeCalc", {4, 0}},
                            {"Samples", {23, 7}}}));

  // Every field of every record has a column, or is an id, or is listed.
  for (const char* record : {"Sample", "Location", "DataPoint", "Aliquot", "Measurement", "AgeCalc", "AgeSummary"}) {
    const json fields = fields_of(record);
    for (const auto& [field, value] : fields.items()) {
      const RecordField one{record, field};
      const bool has_column = taken.contains(one);
      if (is_id(field)) {
        EXPECT_FALSE(has_column) << record << "." << field;
        EXPECT_FALSE(kNoColumn.contains(one)) << record << "." << field;
      } else {
        EXPECT_NE(has_column, kNoColumn.contains(one))
            << record << "." << field << (has_column ? " has a column and is listed as having none" : " has no column");
      }
    }
  }
  for (const RecordField& one : kNoColumn) {
    EXPECT_TRUE(fields_of(one.first).contains(one.second)) << one.first << " has no " << one.second;
  }
}

// ---- Values -------------------------------------------------------------------

TEST_F(Xlsx, EveryRecordLandsInItsSheetUnderItsColumn) {
  const std::vector<Submission> submissions = fixtures();
  ASSERT_EQ(submissions.size(), 2u);
  const XlsxResult result = exported(submissions);
  ASSERT_EQ(result.files.size(), 2u);
  expect_arar_workbook(result.files[0], submissions);
  expect_sample_workbook(result.files[1], submissions);
  // What the mapping fills all has a place.
  EXPECT_EQ(result.notes, std::vector<std::string>{});

  // A few by hand, so that the check above is not only the census against
  // itself.
  const Submission& step = submissions[1];
  ASSERT_EQ(step.datapoint_name, "S-STEP sanidine");
  const std::vector<Row> points = rows_of(result.files[0], "ArArDataPoints");
  ASSERT_EQ(points.size(), 2u);
  EXPECT_EQ(points[1].at("datapointName"), "S-STEP sanidine");
  EXPECT_EQ(points[1].at("sampleName"), "S-STEP");
  EXPECT_EQ(points[1].at("mineral"), "sanidine");
  EXPECT_EQ(points[1].at("lithology"), "ash-flow tuff");
  EXPECT_EQ(points[1].at("arArMethod"), "Step-heating - laser");
  EXPECT_EQ(points[1].at("analysisUnits"), "fA");
  EXPECT_EQ(points[1].at("analyticalUncertaintyType"), "1 sigma");
  EXPECT_EQ(points[1].at("jValueUncertaintyType"), "1 sigma");
  EXPECT_EQ(points[1].at("jValueUncertaintyUnit"), "abs.");
  EXPECT_EQ(points[1].at("analysisDate"), *step.data_point.analysisDate);
  // The irradiation the analyses name; the fusions name none, and no set-up
  // was given.
  EXPECT_EQ(points[1].at("irradiationBatchID"), "NM-900");
  EXPECT_FALSE(points[0].contains("irradiationBatchID"));
  EXPECT_FALSE(points[1].contains("arArSetUp"));

  const std::vector<Row> measurements = rows_of(result.files[0], "ArArMeasurement");
  ASSERT_EQ(measurements.size(), 16u);
  const Row& third = measurements[6 + 2];  // step C
  const Measurement& m = step.measurements[2];
  EXPECT_EQ(std::stod(third.at("ar40Corr")), *m.ar40Corr);
  EXPECT_EQ(std::stod(third.at("ar39CorrUncertainty")), *m.ar39CorrUncertainty);
  EXPECT_EQ(std::stod(third.at("apparentAge")), *m.apparentAge);
  EXPECT_EQ(std::stod(third.at("Ar39KReleased")), *m.ar39KReleased);
  EXPECT_EQ(std::stod(third.at("stepNumber")), 3.0);
  EXPECT_EQ(third.at("plateauStep"), "Yes");
  EXPECT_EQ(measurements[6].at("plateauStep"), "No");
  EXPECT_EQ(third.at("laserPowerUnits"), "W");

  const std::vector<Row> samples = rows_of(result.files[1], "Samples");
  EXPECT_EQ(samples[1].at("sampleName"), "S-STEP");
  EXPECT_EQ(samples[1].at("material"), "sanidine");
  EXPECT_EQ(samples[1].at("igsn"), "IEXXX0001");
  EXPECT_EQ(std::stod(samples[1].at("latitude")), 37.75);
  EXPECT_EQ(std::stod(samples[1].at("longitude")), -106.9);
  EXPECT_EQ(std::stod(samples[1].at("elevationGround")), 2850.0);
  EXPECT_EQ(samples[1].at("unitName"), "Fish Canyon Tuff");
  EXPECT_EQ(samples[1].at("sampleComment"), "Uploaded from pychron-cpp");
  // The fusion sample has no place: its columns stay empty.
  EXPECT_FALSE(samples[0].contains("latitude"));
}

TEST_F(Xlsx, EveryFieldOfEveryRecordIsWrittenOrNoted) {
  const std::vector<Submission> submissions = {everything()};
  const XlsxResult result = exported(submissions);
  ASSERT_EQ(result.files.size(), 2u);
  expect_arar_workbook(result.files[0], submissions);
  expect_sample_workbook(result.files[1], submissions);

  // Each field the census lists without a column is noted, except the three
  // that say again what a column holds (here they agree with it), and no id.
  EXPECT_EQ(sorted(result.notes), sorted({
                                      "ArArMeasurement: no column for ratio40Arx36ArTrapped",
                                      "ArArMeasurement: no column for ratio40Arx36ArTrappedUncertainty",
                                      "ArArAgeCalc: no column for comment",
                                      "Samples: no column for the location's description",
                                      "Samples: no column for the location's name",
                                  }));

  // Every named column of the three sheets that have no empty ones is full.
  for (const char* sheet : {"ArArMeasurement", "ArArAliquot", "ArArAgeSummary", "ArArAgeCalc"}) {
    const std::vector<std::string> keys = row_of(open(result.files[0]), sheet, 4);
    for (const Row& row : rows_of(result.files[0], sheet)) EXPECT_EQ(row.size(), keys.size()) << sheet;
  }
  EXPECT_EQ(rows_of(result.files[0], "ArArDataPoints").at(0).size(), 21u);
  EXPECT_EQ(rows_of(result.files[1], "Samples").at(0).size(), 23u);
}

TEST_F(Xlsx, AFieldSaidTwiceIsNotedOnlyWhenTheTwoDiffer) {
  Submission s = everything();
  const auto none = [](const XlsxResult& result, const char* field) {
    return std::none_of(result.notes.begin(), result.notes.end(),
                        [&](const std::string& note) { return note.ends_with(std::string("no column for ") + field); });
  };
  const XlsxResult agreeing = exported({s});
  for (const char* field : {"analysisUnits", "apparentAgeUncertainityTypeName", "ageUncertaintyTypeName", "sampleID"}) {
    EXPECT_TRUE(none(agreeing, field)) << field;
  }

  s.measurements[1].analysisUnits = "mV";
  s.measurements[0].apparentAgeUncertainityTypeName = "2 sigma";
  s.age_summary.ageUncertaintyTypeName = "2 sigma";
  s.sample.sampleID = "another";
  options_.now += std::chrono::seconds(1);
  const XlsxResult result = exported({s});
  const auto noted = [&](const char* note) {
    return std::count(result.notes.begin(), result.notes.end(), std::string(note));
  };
  EXPECT_EQ(noted("ArArMeasurement: no column for analysisUnits"), 1);
  EXPECT_EQ(noted("ArArMeasurement: no column for apparentAgeUncertainityTypeName"), 1);
  EXPECT_EQ(noted("ArArAgeSummary: no column for ageUncertaintyTypeName"), 1);
  EXPECT_EQ(noted("Samples: no column for sampleID"), 1);
}

TEST_F(Xlsx, TheJUncertaintyTypeOfADataPointIsItsMeasurements) {
  Submission s = everything();
  XlsxResult result = exported({s});
  EXPECT_EQ(rows_of(result.files[0], "ArArDataPoints").at(0).at("jValueUncertaintyType"), "1 sigma");
  EXPECT_EQ(std::count(result.notes.begin(), result.notes.end(),
                       std::string("ArArMeasurement: no column for jvalueUncertaintyTypeName")),
            0);

  // One that does not say leaves the others' word standing.
  s.measurements[1].jvalueUncertaintyTypeName.reset();
  result = exported({s});
  EXPECT_EQ(rows_of(result.files[0], "ArArDataPoints").at(0).at("jValueUncertaintyType"), "1 sigma");

  // Two that disagree: the column cannot hold both and holds neither.
  s.measurements[1].jvalueUncertaintyTypeName = "2 sigma";
  result = exported({s});
  EXPECT_FALSE(rows_of(result.files[0], "ArArDataPoints").at(0).contains("jValueUncertaintyType"));
  EXPECT_EQ(std::count(result.notes.begin(), result.notes.end(),
                       std::string("ArArMeasurement: no column for jvalueUncertaintyTypeName")),
            1);
}

TEST_F(Xlsx, DatesTimesAndFlagsAreInTheSheetsOwnForm) {
  // What the sheets ask for, in their own second row.
  const ooxml::Workbook arar = open(template_dir() / kArAr);
  const ooxml::Workbook sample = open(template_dir() / kSample);
  const auto asked = [](const ooxml::Workbook& book, const std::string& sheet, const std::string& key) {
    const std::vector<std::string> keys = row_of(book, sheet, 4);
    const std::vector<std::string> kinds = row_of(book, sheet, 2);
    const auto at = static_cast<std::size_t>(std::find(keys.begin(), keys.end(), key) - keys.begin());
    return at < kinds.size() ? kinds[at] : std::string("no such column");
  };
  EXPECT_EQ(asked(arar, "ArArMeasurement", "analysisTime"), "YYYY-MM-DD hh:mm z");
  EXPECT_EQ(asked(arar, "ArArAgeSummary", "preferredAge"), "Yes/No");
  EXPECT_EQ(asked(sample, "Samples", "collectDateMin"), "Date dd.mm.yyyy");
  EXPECT_EQ(asked(sample, "Samples", "collectDateMax"), "Date dd.mm.yyyy");
  const std::vector<std::string> definition = row_of(arar, "ArArDataPoints", 1);
  EXPECT_NE(definition.at(6).find("YYYY-MM-DD"), std::string::npos) << definition.at(6);

  Submission s = everything();
  s.data_point.analysisDate = "2024-03-05T06:07:08Z";  // a date is cut out of a time
  s.age_summary.preferredAge = false;
  XlsxResult result = exported({s});
  EXPECT_EQ(result.notes.size(), 5u);  // those of the fields without a column
  const std::vector<Row> measurements = rows_of(result.files[0], "ArArMeasurement");
  EXPECT_EQ(measurements.at(0).at("analysisTime"), "2024-03-05 06:07 UTC");
  EXPECT_EQ(measurements.at(1).at("analysisTime"), "2024-03-05 23:59 UTC");  // cut, not rounded
  EXPECT_EQ(rows_of(result.files[0], "ArArDataPoints").at(0).at("analysisDate"), "2024-03-05");
  EXPECT_EQ(rows_of(result.files[0], "ArArAgeSummary").at(0).at("preferredAge"), "No");
  EXPECT_EQ(rows_of(result.files[1], "Samples").at(0).at("collectDateMin"), "30.06.2019");
  EXPECT_EQ(rows_of(result.files[1], "Samples").at(0).at("collectDateMax"), "01.07.2019");

  s.age_summary.preferredAge = true;
  // What is not a time or a date in the records' form is written as it is,
  // and said.
  s.measurements[0].analysisTime = "2024-03-05 16:07 AEST";
  s.measurements[1].analysisTime = "2024-03-05T06:07:08+10:00";
  s.data_point.analysisDate = "5 March 2024";
  s.sample.collectDateMin = "2019-13-30";
  s.sample.collectDateMax = "June 2019";
  result = exported({s});
  const std::vector<Row> again = rows_of(result.files[0], "ArArMeasurement");
  EXPECT_EQ(again.at(0).at("analysisTime"), "2024-03-05 16:07 AEST");
  EXPECT_EQ(again.at(1).at("analysisTime"), "2024-03-05T06:07:08+10:00");
  EXPECT_EQ(rows_of(result.files[0], "ArArDataPoints").at(0).at("analysisDate"), "5 March 2024");
  EXPECT_EQ(rows_of(result.files[0], "ArArAgeSummary").at(0).at("preferredAge"), "Yes");
  EXPECT_EQ(rows_of(result.files[1], "Samples").at(0).at("collectDateMin"), "2019-13-30");
  EXPECT_EQ(rows_of(result.files[1], "Samples").at(0).at("collectDateMax"), "June 2019");
  const auto noted = [&](const char* note) {
    return std::count(result.notes.begin(), result.notes.end(), std::string(note));
  };
  EXPECT_EQ(noted("ArArMeasurement: analysisTime 2024-03-05 16:07 AEST is not a UTC time "
                  "(YYYY-MM-DDTHH:MM:SSZ) and was written as it is"),
            1);
  EXPECT_EQ(noted("ArArMeasurement: analysisTime 2024-03-05T06:07:08+10:00 is not a UTC time "
                  "(YYYY-MM-DDTHH:MM:SSZ) and was written as it is"),
            1);
  EXPECT_EQ(noted("ArArDataPoints: analysisDate 5 March 2024 is not a date (YYYY-MM-DD) and was written as it is"), 1);
  EXPECT_EQ(noted("Samples: collectDateMin 2019-13-30 is not a date (YYYY-MM-DD) and was written as it is"), 1);
  EXPECT_EQ(noted("Samples: collectDateMax June 2019 is not a date (YYYY-MM-DD) and was written as it is"), 1);
}

TEST_F(Xlsx, SheetOverrides) {
  const XlsxResult result = exported({everything()});
  const Row point = rows_of(result.files[0], "ArArDataPoints").at(0);
  Submission s = everything();
  EXPECT_EQ(point.at("arArMethod"), *s.data_point.arMethodName);
  // The set-up and the irradiation batch are asked for as search terms: the
  // names the submission carries, never the record's ids, set here too.
  ASSERT_TRUE(s.data_point.arArAnalyticalSetUpId.has_value() && s.data_point.irradiationBatchIDId.has_value());
  EXPECT_EQ(point.at("arArSetUp"), "the set-up");
  EXPECT_EQ(point.at("irradiationBatchID"), "the batch");
  EXPECT_EQ(point.at("datapoint.literature"), *s.data_point.literatureName);
  EXPECT_EQ(point.at("datapoint.funding"), *s.data_point.fundingName);
  EXPECT_EQ(point.at("jValueUncertaintyUnit"), *s.data_point.jvalueUncertaintyUnitName);
  EXPECT_EQ(std::stod(rows_of(result.files[0], "ArArMeasurement").at(1).at("Ar39KReleased")),
            *s.measurements[1].ar39KReleased);

  s.sample.name = "the name";
  s.sample.sampleID = "the name";
  s.sample.description = "a description";
  s.sample.archiveName = "an archive";
  s.sample.stratographicUnitName = "a unit";
  s.sample.referenceElevation = 100.0;
  s.sample.relativeElevationMin = 2.0;
  s.sample.relativeElevationMax = 3.0;
  s.sample.rockUnitAgeMin = 10.0;
  s.sample.rockUnitAgeMax = 20.0;
  s.sample.rockUnitAgeDescription = "about fifteen";
  s.location->lat = -37.8;
  s.location->lon = 144.9;
  s.location->latLonPrecision = 25.0;
  options_.now += std::chrono::seconds(1);
  const XlsxResult second = exported({s});
  const Row sample = rows_of(second.files[1], "Samples").at(0);
  EXPECT_EQ(sample.at("sampleName"), "the name");
  EXPECT_EQ(sample.at("sampleComment"), "a description");
  EXPECT_EQ(sample.at("lastKnownArchive"), "an archive");
  EXPECT_EQ(sample.at("unitName"), "a unit");
  EXPECT_EQ(sample.at("elevationGround"), "100");
  EXPECT_EQ(sample.at("elevationDepthMin"), "2");
  EXPECT_EQ(sample.at("elevationDepthMax"), "3");
  EXPECT_EQ(sample.at("ageMin"), "10");
  EXPECT_EQ(sample.at("ageMax"), "20");
  EXPECT_EQ(sample.at("unitAgeDescription"), "about fifteen");
  EXPECT_EQ(sample.at("latitude"), "-37.8");
  EXPECT_EQ(sample.at("longitude"), "144.9");
  EXPECT_EQ(sample.at("latLonPrecision"), "25");
  EXPECT_EQ(sample.at("funding"), *s.sample.fundingName);
  EXPECT_EQ(sample.at("literature"), *s.sample.literatureName);
  EXPECT_FALSE(sample.contains("Tag"));
  EXPECT_FALSE(sample.contains("elevationDepthAccuracy"));
  EXPECT_EQ(rows_of(second.files[0], "ArArDataPoints").at(0).at("sampleName"), "the name");
}

// ---- Rows ---------------------------------------------------------------------

TEST_F(Xlsx, RowsAreLinkedByDatapointName) {
  const std::vector<Submission> submissions = fixtures();
  const XlsxResult result = exported(submissions);
  const std::map<std::string, std::vector<std::size_t>> rows_each = {
      {"ArArDataPoints", {1, 1}}, {"ArArMeasurement", {6, 10}}, {"ArArAliquot", {6, 1}},
      {"ArArAgeSummary", {1, 1}}, {"ArArAgeCalc", {1, 1}},
  };
  for (const std::string& sheet : kArArSheets) {
    // Column A of every sheet is the key.
    EXPECT_EQ(row_of(open(result.files[0]), sheet, 4).at(0), "datapointName") << sheet;
    std::vector<std::string> expected;
    for (std::size_t i = 0; i < submissions.size(); ++i) {
      expected.insert(expected.end(), rows_each.at(sheet)[i], submissions[i].datapoint_name);
    }
    std::vector<std::string> actual;
    for (const Row& row : rows_of(result.files[0], sheet)) {
      ASSERT_TRUE(row.contains("datapointName")) << sheet;
      actual.push_back(row.at("datapointName"));
    }
    EXPECT_EQ(actual, expected) << sheet;
  }
  // Measurements and aliquots are in the submission's order.
  const std::vector<Row> measurements = rows_of(result.files[0], "ArArMeasurement");
  const std::vector<Row> aliquots = rows_of(result.files[0], "ArArAliquot");
  for (std::size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(std::stod(measurements.at(6 + i).at("stepNumber")), static_cast<double>(i + 1));
    EXPECT_EQ(measurements.at(6 + i).at("aliquotName"), "20001-01");
  }
  for (std::size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(measurements.at(i).at("aliquotName"), *submissions[0].measurements[i].aliquotName);
    EXPECT_EQ(aliquots.at(i).at("aliquotName"), *submissions[0].aliquots[i].aliquotName);
  }
  EXPECT_EQ(aliquots.at(6).at("aliquotName"), "20001-01");
}

TEST_F(Xlsx, HeaderRowsAreTheTemplates) {
  const XlsxResult result = exported(fixtures());
  const std::map<std::string, std::set<std::string>> written = {
      {kArAr,
       {"xl/worksheets/sheet1.xml", "xl/worksheets/sheet2.xml", "xl/worksheets/sheet3.xml",
        "xl/worksheets/sheet4.xml", "xl/worksheets/sheet5.xml"}},
      {kSample, {"xl/worksheets/sheet1.xml"}},
  };
  const std::pair<const char*, fs::path> pairs[] = {{kArAr, result.files[0]}, {kSample, result.files[1]}};
  for (const auto& [name, file] : pairs) {
    const ooxml::Workbook original = open(template_dir() / name);
    const ooxml::Workbook copy = open(file);
    ASSERT_EQ(copy.sheet_names(), original.sheet_names());
    for (const std::string& sheet : original.sheet_names()) {
      for (int row = 1; row <= 4; ++row) {
        EXPECT_EQ(row_of(copy, sheet, row), row_of(original, sheet, row)) << sheet << " row " << row;
      }
    }
    // The sheets of definitions and lookup lists and the Props sheets, and
    // every other part that is not a data sheet, are the template's bytes.
    const auto before = ooxml::read_zip(template_dir() / name);
    const auto after = ooxml::read_zip(file);
    ASSERT_TRUE(before.has_value() && after.has_value());
    ASSERT_EQ(after->size(), before->size());
    std::set<std::string> changed;
    for (std::size_t i = 0; i < before->size(); ++i) {
      EXPECT_EQ((*after)[i].name, (*before)[i].name);
      if ((*after)[i].data != (*before)[i].data) changed.insert((*before)[i].name);
    }
    EXPECT_EQ(changed, written.at(name)) << name;
  }
  // The Props sheets hold their header and nothing else.
  EXPECT_TRUE(row_of(open(result.files[0]), "Datapoint Props", 2).empty());
  EXPECT_TRUE(row_of(open(result.files[1]), "Sample Props", 2).empty());
}

TEST_F(Xlsx, RowsPastTheFormattedOnesAreSaidOnce) {
  // The age summary sheet has three formatted rows.
  std::vector<Submission> four;
  for (int i = 0; i < 4; ++i) {
    Submission s = fixtures().at(0);
    s.datapoint_name += " " + std::to_string(i);
    four.push_back(std::move(s));
  }
  options_.per_sample = false;
  const XlsxResult result = exported(four);
  EXPECT_EQ(result.notes,
            std::vector<std::string>{"ArArAgeSummary: the rows after row 7 are past the template's formatted rows "
                                     "and are plain"});
  EXPECT_EQ(rows_of(result.files[0], "ArArAgeSummary").size(), 4u);
  // One sample row: the four are the same sample.
  EXPECT_EQ(rows_of(result.files[1], "Samples").size(), 1u);
}

TEST_F(Xlsx, ASheetWithNoRowsIsTheTemplates) {
  // A data point no aliquot was recorded for.
  std::vector<Submission> submissions = {fixtures().at(1)};
  submissions[0].aliquots.clear();
  const XlsxResult result = exported(submissions);
  EXPECT_TRUE(rows_of(result.files[0], "ArArAliquot").empty());
  EXPECT_EQ(rows_of(result.files[0], "ArArMeasurement").size(), 10u);
  const auto before = ooxml::read_zip(template_dir() / kArAr);
  const auto after = ooxml::read_zip(result.files[0]);
  ASSERT_TRUE(before.has_value() && after.has_value());
  for (std::size_t i = 0; i < before->size(); ++i) {
    if ((*before)[i].name == "xl/worksheets/sheet3.xml") EXPECT_EQ((*after)[i].data, (*before)[i].data);
  }
}

// ---- Files --------------------------------------------------------------------

TEST_F(Xlsx, OneWorkbookOrOnePerSample) {
  const std::vector<Submission> submissions = fixtures();
  XlsxResult result = exported(submissions);
  EXPECT_EQ(file_names(result.files), (std::vector<std::string>{std::string("ArArDataPoint_") + kStamp + ".xlsx",
                                                                std::string("Samples_") + kStamp + ".xlsx"}));
  for (const fs::path& file : result.files) {
    EXPECT_EQ(file.parent_path(), out_);
    EXPECT_TRUE(fs::is_regular_file(file));
  }

  out_ = dir_ / "each";
  options_.per_sample = true;
  result = exported(submissions);
  ASSERT_EQ(file_names(result.files),
            (std::vector<std::string>{"ArArDataPoint_S-FUS.xlsx", "ArArDataPoint_S-STEP.xlsx",
                                      std::string("Samples_") + kStamp + ".xlsx"}));
  expect_arar_workbook(result.files[0], {submissions[0]});
  expect_arar_workbook(result.files[1], {submissions[1]});
  expect_sample_workbook(result.files[2], submissions);
  EXPECT_EQ(std::distance(fs::directory_iterator(out_), fs::directory_iterator()), 3);
}

TEST_F(Xlsx, SampleWorkbookIsOptional) {
  options_.samples = false;
  const XlsxResult result = exported(fixtures());
  EXPECT_EQ(file_names(result.files), std::vector<std::string>{std::string("ArArDataPoint_") + kStamp + ".xlsx"});
  EXPECT_EQ(std::distance(fs::directory_iterator(out_), fs::directory_iterator()), 1);

  // Without it the Sample template is not needed.
  const fs::path only_arar = dir_ / "only";
  fs::create_directories(only_arar);
  fs::copy_file(template_dir() / kArAr, only_arar / kArAr);
  options_.template_dir = only_arar;
  out_ = dir_ / "second";
  EXPECT_EQ(exported(fixtures()).files.size(), 1u);
}

TEST_F(Xlsx, UnsafeSampleNamesMakeSafeDistinctFileNames) {
  std::vector<Submission> submissions;
  for (const char* name : {"A/B", "A B", "A:B", "a_b", "../../up", "", "caf\xC3\xA9"}) {
    Submission s = fixtures().at(0);
    s.sample.name = name;
    s.sample.sampleID = name;
    s.datapoint_name = std::string("point ") + std::to_string(submissions.size());
    submissions.push_back(std::move(s));
  }
  submissions[5].sample.name.reset();  // a sample without a name
  submissions[5].sample.sampleID.reset();
  options_.per_sample = true;
  options_.samples = false;
  const XlsxResult result = exported(submissions);
  // `a_b` is `A_B` to a file system that ignores case.
  EXPECT_EQ(file_names(result.files),
            (std::vector<std::string>{"ArArDataPoint_A_B.xlsx", "ArArDataPoint_A_B-2.xlsx", "ArArDataPoint_A_B-3.xlsx",
                                      "ArArDataPoint_a_b-4.xlsx", "ArArDataPoint_______up.xlsx",
                                      "ArArDataPoint_point_5.xlsx", "ArArDataPoint_caf__.xlsx"}));
  std::set<fs::path> distinct;
  for (std::size_t i = 0; i < result.files.size(); ++i) {
    EXPECT_EQ(result.files[i].parent_path(), out_);
    EXPECT_TRUE(fs::is_regular_file(result.files[i]));
    distinct.insert(result.files[i]);
    EXPECT_EQ(rows_of(result.files[i], "ArArDataPoints").at(0).at("datapointName"), submissions[i].datapoint_name);
  }
  EXPECT_EQ(distinct.size(), submissions.size());
  EXPECT_EQ(std::distance(fs::directory_iterator(out_), fs::directory_iterator()), 7);
  EXPECT_FALSE(fs::exists(dir_ / "up.xlsx"));
}

TEST_F(Xlsx, AnExistingFileIsNotOverwritten) {
  const std::vector<Submission> submissions = fixtures();
  const XlsxResult first = exported(submissions);
  const std::string arar = slurp(first.files[0]);
  const std::string samples = slurp(first.files[1]);

  const XlsxResult second = exported(submissions);  // the same second
  EXPECT_EQ(file_names(second.files), (std::vector<std::string>{std::string("ArArDataPoint_") + kStamp + "-2.xlsx",
                                                                 std::string("Samples_") + kStamp + "-2.xlsx"}));
  EXPECT_EQ(slurp(first.files[0]), arar);
  EXPECT_EQ(slurp(first.files[1]), samples);
  // The same submissions give the same bytes.
  EXPECT_EQ(slurp(second.files[0]), arar);
  EXPECT_EQ(slurp(second.files[1]), samples);

  options_.per_sample = true;
  spit(out_ / "ArArDataPoint_S-FUS.xlsx", "somebody's file");
  spit(out_ / "ArArDataPoint_S-FUS-2.xlsx", "and another");
  const XlsxResult third = exported(submissions);
  EXPECT_EQ(file_names(third.files),
            (std::vector<std::string>{"ArArDataPoint_S-FUS-3.xlsx", "ArArDataPoint_S-STEP.xlsx",
                                      std::string("Samples_") + kStamp + "-3.xlsx"}));
  EXPECT_EQ(slurp(out_ / "ArArDataPoint_S-FUS.xlsx"), "somebody's file");
  EXPECT_EQ(slurp(out_ / "ArArDataPoint_S-FUS-2.xlsx"), "and another");
}

// ---- Templates ----------------------------------------------------------------

TEST_F(Xlsx, AFieldTheTemplateLacksIsNoted) {
  // An Ar-Ar template whose age summary sheet has no `mswd` column (I4) and
  // whose measurement sheet has no key column (A4).
  options_.template_dir =
      edited_templates("xl/worksheets/sheet4.xml", [](std::string& xml) { remove_cell(xml, "I4"); });
  const std::vector<Submission> submissions = fixtures();
  ASSERT_TRUE(submissions[0].age_summary.mswd.has_value());
  XlsxResult result = exported(submissions);
  EXPECT_EQ(result.notes, std::vector<std::string>{"ArArAgeSummary: no column for mswd"});  // once, for two samples
  const std::vector<Row> summaries = rows_of(result.files[0], "ArArAgeSummary");
  ASSERT_EQ(summaries.size(), 2u);
  EXPECT_FALSE(summaries[0].contains("mswd"));
  // Its neighbours are where they were.
  EXPECT_EQ(std::stod(summaries[0].at("ageUncertainty")), *submissions[0].age_summary.ageUncertainty);
  EXPECT_EQ(std::stod(summaries[0].at("agepvalue")), *submissions[0].age_summary.agepvalue);
  const auto columns = xlsx_columns(options_.template_dir);
  ASSERT_TRUE(columns.has_value());
  EXPECT_EQ(std::count_if(columns->begin(), columns->end(), [](const XlsxColumn& c) { return c.key == "mswd"; }), 0);

  fs::remove_all(dir_ / "templates");
  options_.template_dir =
      edited_templates("xl/worksheets/sheet2.xml", [](std::string& xml) { remove_cell(xml, "A4"); });
  out_ = dir_ / "unlinked";
  result = exported(submissions);
  EXPECT_EQ(result.notes, std::vector<std::string>{"ArArMeasurement: no column for datapointName"});
  EXPECT_FALSE(rows_of(result.files[0], "ArArMeasurement").at(0).contains("datapointName"));
}

TEST_F(Xlsx, AMissingTemplateIsAnErrorNamingTheFile) {
  const fs::path empty = dir_ / "none";
  fs::create_directories(empty);
  options_.template_dir = empty;
  Error error = refused(fixtures());
  EXPECT_EQ(error.kind, ErrorKind::Io);
  EXPECT_NE(error.what.find("earthbank: "), std::string::npos) << error.what;
  EXPECT_NE(error.what.find((empty / kArAr).string()), std::string::npos) << error.what;
  expect_no_file();

  // With only the Ar-Ar template the Ar-Ar workbook could be written. It is
  // not: all the files or none.
  fs::copy_file(template_dir() / kArAr, empty / kArAr);
  error = refused(fixtures());
  EXPECT_EQ(error.kind, ErrorKind::Io);
  EXPECT_NE(error.what.find((empty / kSample).string()), std::string::npos) << error.what;
  expect_no_file();

  // A file that is not a workbook.
  spit(empty / kSample, "not a workbook");
  error = refused(fixtures());
  EXPECT_EQ(error.kind, ErrorKind::Protocol);
  EXPECT_NE(error.what.find((empty / kSample).string()), std::string::npos) << error.what;
  expect_no_file();

  EXPECT_FALSE(xlsx_columns(dir_ / "nowhere").has_value());
}

TEST_F(Xlsx, ATemplateWithoutASheetIsRefused) {
  options_.template_dir = edited_templates("xl/workbook.xml", [](std::string& xml) {
    const std::size_t at = xml.find("name=\"ArArAliquot\"");
    ASSERT_NE(at, std::string::npos);
    xml.replace(at, 18, "name=\"SomeOthers\"");
  });
  const Error error = refused(fixtures());
  EXPECT_NE(error.what.find("ArArAliquot"), std::string::npos) << error.what;
  EXPECT_NE(error.what.find(kArAr), std::string::npos) << error.what;
  expect_no_file();
}

TEST_F(Xlsx, DefaultTemplateDir) {
  // A development build: no installed templates beside the test program.
  set_env("PYCHRON_EARTHBANK_TEMPLATES", "");
  EXPECT_TRUE(fs::equivalent(default_template_dir(), template_dir())) << default_template_dir();
  options_.template_dir.clear();
  EXPECT_EQ(exported(fixtures()).files.size(), 2u);
  EXPECT_TRUE(xlsx_columns().has_value());

  // Installed: beside the program, as cmake/PychronInstall.cmake lays it out.
  const fs::path prefix = dir_ / "prefix";
  const fs::path share = prefix / "share" / "pychron" / "earthbank" / "templates";
  fs::create_directories(prefix / "bin");
  EXPECT_TRUE(fs::equivalent(default_template_dir(prefix / "bin"), template_dir()));  // nothing installed there yet
  fs::create_directories(share);
  EXPECT_EQ(default_template_dir(prefix / "bin"), fs::weakly_canonical(share));
  const fs::path bundle = dir_ / "Pychron.app" / "Contents";
  fs::create_directories(bundle / "MacOS");
  fs::create_directories(bundle / "Resources" / "earthbank" / "templates");
  EXPECT_EQ(default_template_dir(bundle / "MacOS"),
            fs::weakly_canonical(bundle / "Resources" / "earthbank" / "templates"));
  EXPECT_TRUE(fs::equivalent(default_template_dir(fs::path{}), template_dir()));

  // The environment comes first.
  const fs::path named = dir_ / "named";
  set_env("PYCHRON_EARTHBANK_TEMPLATES", named.string().c_str());
  EXPECT_EQ(default_template_dir(), named);
  EXPECT_EQ(default_template_dir(prefix / "bin"), named);
  out_ = dir_ / "from-env";
  const auto result = export_xlsx(fixtures(), out_, options_);
  set_env("PYCHRON_EARTHBANK_TEMPLATES", "");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().what.find((named / kArAr).string()), std::string::npos) << result.error().what;
}

// ---- Refusals -----------------------------------------------------------------

TEST_F(Xlsx, AllTheFilesOrNone) {
  // Text a workbook cannot hold, in the last sheet written of the last
  // workbook but one.
  std::vector<Submission> submissions = fixtures();
  submissions[1].age_calc.fluxMonitorName = std::string("FC\x01", 3);
  options_.per_sample = true;
  Error error = refused(submissions);
  EXPECT_EQ(error.kind, ErrorKind::Config);
  EXPECT_NE(error.what.find("ArArAgeCalc"), std::string::npos) << error.what;
  expect_no_file();

  // And in the Sample workbook, after the Ar-Ar ones were ready.
  submissions = fixtures();
  submissions[1].sample.igsn = std::string("IE\x02", 3);
  error = refused(submissions);
  EXPECT_EQ(error.kind, ErrorKind::Config);
  EXPECT_NE(error.what.find("Samples"), std::string::npos) << error.what;
  expect_no_file();

  // A directory that cannot be one.
  spit(dir_ / "a-file", "x");
  out_ = dir_ / "a-file";
  error = refused(fixtures());
  EXPECT_EQ(error.kind, ErrorKind::Io);
  EXPECT_EQ(slurp(dir_ / "a-file"), "x");
}

TEST_F(Xlsx, WhatCannotBeLinkedIsRefused) {
  Error error = refused({});
  EXPECT_EQ(error.kind, ErrorKind::Config);
  EXPECT_EQ(error.what, "earthbank: no submission to write");

  std::vector<Submission> submissions = fixtures();
  submissions[1].datapoint_name = submissions[0].datapoint_name;
  error = refused(submissions);
  EXPECT_EQ(error.kind, ErrorKind::Config);
  EXPECT_NE(error.what.find("S-FUS biotite"), std::string::npos) << error.what;
  options_.per_sample = true;  // the rows of two workbooks would be told apart; the names are still one
  EXPECT_EQ(refused(submissions).kind, ErrorKind::Config);
  options_.per_sample = false;

  submissions = fixtures();
  submissions[0].datapoint_name.clear();
  error = refused(submissions);
  EXPECT_EQ(error.kind, ErrorKind::Config);
  EXPECT_NE(error.what.find("S-FUS"), std::string::npos) << error.what;
  expect_no_file();
}

TEST_F(Xlsx, ASampleIsWrittenOnce) {
  // Two data points of one sample: one row in the Sample workbook.
  std::vector<Submission> submissions = fixtures();
  Submission other = submissions[1];
  other.datapoint_name = "S-STEP again";
  submissions.push_back(other);
  const XlsxResult result = exported(submissions);
  const std::vector<Row> samples = rows_of(result.files[1], "Samples");
  ASSERT_EQ(samples.size(), 2u);
  EXPECT_EQ(samples[0].at("sampleName"), "S-FUS");
  EXPECT_EQ(samples[1].at("sampleName"), "S-STEP");
  EXPECT_EQ(rows_of(result.files[0], "ArArDataPoints").size(), 3u);

  // Two that share the name and nothing else are not one sample.
  submissions[2].sample.igsn = "IEXXX0002";
  out_ = dir_ / "two";
  const Error error = refused(submissions);
  EXPECT_EQ(error.kind, ErrorKind::Config);
  EXPECT_NE(error.what.find("S-STEP"), std::string::npos) << error.what;
  expect_no_file();
  // Without the Sample workbook the question does not arise.
  options_.samples = false;
  EXPECT_EQ(exported(submissions).files.size(), 1u);
}

}  // namespace
}  // namespace pychron::earthbank
