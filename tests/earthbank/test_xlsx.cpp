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
#include "xlsx_detail.hpp"
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
    {{"Samples", "locationComment"}, {"Location", "description"}},
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
    {"Samples", "person"},
    {"Samples", "personRole"},
    {"Samples", "archiveComment"},
    {"Samples", "Tag"},
};

// The fields no column takes. Beside these, no `id` and no `*Id` has one.
const std::set<RecordField> kNoColumn = {
    {"Sample", "sampleID"},                             // the name again
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

// Every value of the two fixture samples that is written otherwise than the
// mapping gives it: the entry of the template's list it is.
struct Rewrite {
  std::string_view sheet, key, given, written;
};
constexpr Rewrite kRewrites[] = {
    {"ArArAgeCalc", "fluxMonitor", "FC-2 28.201 Ma",
     "Fish Canyon Tuff Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)"},
    {"Samples", "material", "biotite", "Biotite"},
    {"Samples", "material", "sanidine", "Sanidine"},
};

// The value as the workbook shows it: a time to the minute in the
// measurement sheet, a collection date day first, a flag as Yes or No, a
// term as its list spells it.
json shown(const std::string& sheet, const std::string& key, const json& value) {
  if (value.is_boolean()) return value.get<bool>() ? "Yes" : "No";
  if (!value.is_string()) return value;
  const std::string text = value.get<std::string>();
  for (const Rewrite& rewrite : kRewrites) {
    if (rewrite.sheet == sheet && rewrite.key == key && rewrite.given == text) return std::string(rewrite.written);
  }
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

// The notes that have `part` in them, in their order.
std::vector<std::string> notes_with(const std::vector<std::string>& notes, std::string_view part) {
  std::vector<std::string> out;
  for (const std::string& note : notes) {
    if (note.find(part) != std::string::npos) out.push_back(note);
  }
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
                            {"Samples", {24, 6}}}));

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
  EXPECT_EQ(notes_with(result.notes, "no column for"), std::vector<std::string>{});

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
  EXPECT_EQ(samples[1].at("material"), "Sanidine");  // as the template's list spells it
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
  EXPECT_EQ(sorted(notes_with(result.notes, "no column for")),
            sorted({
                "ArArMeasurement: no column for ratio40Arx36ArTrapped",
                "ArArMeasurement: no column for ratio40Arx36ArTrappedUncertainty",
                "ArArAgeCalc: no column for comment",
                "Samples: no column for the location's name",
            }));
  // Every Required column holds something; the made-up text in the columns
  // that have a list is in no list, and each is said (the J uncertainty type
  // is the one real term among them).
  EXPECT_EQ(notes_with(result.notes, "Required"), std::vector<std::string>{});
  const std::vector<std::string> misses = notes_with(result.notes, "is not in the template's list");
  EXPECT_EQ(misses.size(), 7u + 2u * 3u + 5u + 3u + 5u) << ::testing::PrintToString(misses);
  EXPECT_EQ(result.notes.size(), 4u + misses.size()) << ::testing::PrintToString(result.notes);

  // Every named column of the three sheets that have no empty ones is full.
  for (const char* sheet : {"ArArMeasurement", "ArArAliquot", "ArArAgeSummary", "ArArAgeCalc"}) {
    const std::vector<std::string> keys = row_of(open(result.files[0]), sheet, 4);
    for (const Row& row : rows_of(result.files[0], sheet)) EXPECT_EQ(row.size(), keys.size()) << sheet;
  }
  EXPECT_EQ(rows_of(result.files[0], "ArArDataPoints").at(0).size(), 21u);
  EXPECT_EQ(rows_of(result.files[1], "Samples").at(0).size(), 24u);
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
  EXPECT_EQ(notes_with(result.notes, "no column for").size(), 4u);
  EXPECT_EQ(notes_with(result.notes, "written as it is"), std::vector<std::string>{});
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
  EXPECT_EQ(notes_with(result.notes, "formatted rows"),
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
  // The last four keep no letter or digit of their own: wholly outside ASCII,
  // or all punctuation.
  for (const char* name : {"A/B", "A B", "A:B", "a_b", "../../up", "caf\xC3\xA9", "\xE6\xA8\x99\xE6\x9C\xAC",
                           "\xC3\x85\xC3\x98", "///", "\xCE\xB1\xCE\xB2"}) {
    Submission s = fixtures().at(0);
    s.sample.name = name;
    s.sample.sampleID = name;
    s.datapoint_name = std::string("point ") + std::to_string(submissions.size());
    submissions.push_back(std::move(s));
  }
  // And two of them have a data point whose name is no better.
  submissions[7].datapoint_name = "\xC3\x85\xC3\x98 \xCE\xB2";
  submissions[9].datapoint_name = "\xCE\xB1 / \xCE\xB2";
  options_.per_sample = true;
  options_.samples = false;
  const XlsxResult result = exported(submissions);
  // `a_b` is `A_B` to a file system that ignores case. A name that would be
  // underscores alone is the data point's, made safe; failing that too, the
  // sample's place among those exported.
  EXPECT_EQ(file_names(result.files),
            (std::vector<std::string>{"ArArDataPoint_A_B.xlsx", "ArArDataPoint_A_B-2.xlsx", "ArArDataPoint_A_B-3.xlsx",
                                      "ArArDataPoint_a_b-4.xlsx", "ArArDataPoint_______up.xlsx",
                                      "ArArDataPoint_caf__.xlsx", "ArArDataPoint_point_6.xlsx",
                                      "ArArDataPoint_sample-8.xlsx", "ArArDataPoint_point_8.xlsx",
                                      "ArArDataPoint_sample-10.xlsx"}));
  std::set<fs::path> distinct;
  for (std::size_t i = 0; i < result.files.size(); ++i) {
    EXPECT_EQ(result.files[i].parent_path(), out_);
    EXPECT_TRUE(fs::is_regular_file(result.files[i]));
    distinct.insert(result.files[i]);
    EXPECT_EQ(rows_of(result.files[i], "ArArDataPoints").at(0).at("datapointName"), submissions[i].datapoint_name);
    // The name itself is written whole.
    EXPECT_EQ(rows_of(result.files[i], "ArArDataPoints").at(0).at("sampleName"), *submissions[i].sample.name);
  }
  EXPECT_EQ(distinct.size(), submissions.size());
  EXPECT_EQ(std::distance(fs::directory_iterator(out_), fs::directory_iterator()), 10);
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
  EXPECT_EQ(notes_with(result.notes, "no column for"),
            std::vector<std::string>{"ArArAgeSummary: no column for mswd"});  // once, for two samples
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
  EXPECT_EQ(notes_with(result.notes, "no column for"),
            std::vector<std::string>{"ArArMeasurement: no column for datapointName"});
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

  // A sample without a name, or with an empty one: the data point's row
  // would name no sample, and the Samples row nothing. With or without the
  // Sample workbook.
  for (const bool with_samples : {true, false}) {
    options_.samples = with_samples;
    submissions = fixtures();
    submissions[1].sample.name.reset();
    error = refused(submissions);
    EXPECT_EQ(error.kind, ErrorKind::Config);
    EXPECT_EQ(error.what, "earthbank: the sample of data point S-STEP sanidine has no name: the data point could not "
                          "be tied to its sample");
    submissions[1].sample.name = "";
    EXPECT_EQ(refused(submissions).what, error.what);
    expect_no_file();
  }
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

// ---- What the template says a column may hold ------------------------------------

const char* const kKuiper = "Fish Canyon Tuff Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)";

// How the note on a sheet's Required columns begins, after the sheet's name.
const char* const kRequiredLead = ": marked Required by the template's colouring; the importer may refuse a row that "
                                  "leaves one empty: ";

TEST_F(Xlsx, WhatAnExportOfTheFixturesSays) {
  const std::vector<Submission> submissions = fixtures();
  const XlsxResult result = exported(submissions);
  // Everything a user is told about the two samples, in order: what was
  // rewritten, then what the template wants and was not given.
  EXPECT_EQ(result.notes,
            (std::vector<std::string>{
                std::string("ArArAgeCalc.fluxMonitor: 'FC-2 28.201 Ma' written as '") + kKuiper +
                    "' (list: Flux Monitor), found by its name and age",
                "Samples.material: 'biotite' written as 'Biotite' (list: Lithology)",
                "Samples.material: 'sanidine' written as 'Sanidine' (list: Lithology)",
                // S-STEP's analyses name their irradiation; S-FUS's name none.
                std::string("ArArDataPoints") + kRequiredLead +
                    "arArSetUp (Analytical Key) empty in every row, irradiationBatchID (Irradiation Batch ID) empty "
                    "in 1 of 2 rows, analysisScale (Analysis Scale) empty in every row",
                // No step number or share of the 39Ar is wanted of a fusion, and no
                // furnace temperature of a row that has a laser power: the steps
                // have theirs, and every row its power.
                std::string("ArArMeasurement") + kRequiredLead +
                    "beamSize (Beam size) empty in every row, beamSizeUnits (Beam size units) empty in every row",
                std::string("ArArAliquot") + kRequiredLead +
                    "aliquotSize (Aliquot size) empty in every row, aliquotUnits (Aliquot size unit) empty in every "
                    "row",
                // S-STEP has its place; S-FUS has none.
                std::string("Samples") + kRequiredLead +
                    "latitude (Latitude (WGS84)) empty in 1 of 2 rows, longitude (Longitude (WGS84)) empty in 1 of 2 "
                    "rows",
            }));

  // The rewrites, in the cells.
  for (const Row& row : rows_of(result.files[0], "ArArAgeCalc")) EXPECT_EQ(row.at("fluxMonitor"), kKuiper);
  const std::vector<Row> samples = rows_of(result.files[1], "Samples");
  EXPECT_EQ(samples.at(0).at("material"), "Biotite");
  EXPECT_EQ(samples.at(1).at("material"), "Sanidine");
  // A column without a list is written as it was given: the data point's
  // mineral and rock are search terms.
  const std::vector<Row> points = rows_of(result.files[0], "ArArDataPoints");
  EXPECT_EQ(points.at(0).at("mineral"), "biotite");
  EXPECT_EQ(points.at(1).at("mineral"), "sanidine");
  EXPECT_EQ(points.at(1).at("lithology"), "ash-flow tuff");
  // What the mapping writes into a listed column is otherwise an entry of
  // its list already.
  EXPECT_EQ(points.at(0).at("arArMethod"), "Total fusion - laser");
  EXPECT_EQ(rows_of(result.files[0], "ArArAgeSummary").at(1).at("ageCalcType"), "Plateau Age");

  // The same, one workbook per sample: each thing still said once.
  out_ = dir_ / "each";
  options_.per_sample = true;
  EXPECT_EQ(exported(submissions).notes, result.notes);

  // With the set-up and the irradiation named the data point's sheet wants
  // only the scale.
  PublishOptions publish;
  publish.setup = "A Noble Gas Lab, Helix MC";
  publish.samples["S-FUS"].irradiation = "NM-901";
  auto named = build_submissions(testing::both_samples(), publish);
  ASSERT_TRUE(named.has_value());
  out_ = dir_ / "named";
  EXPECT_EQ(notes_with(exported(*named).notes, "ArArDataPoints"),
            std::vector<std::string>{std::string("ArArDataPoints") + kRequiredLead +
                                     "analysisScale (Analysis Scale) empty in every row"});
}

// A column the template colours Required is not wanted of every row: a
// fusion has no step number and no share of the 39Ar, and a row is heated by
// a laser or by a furnace. What is left is said with the number of rows.
TEST_F(Xlsx, ARequiredColumnIsMissedOnlyWhereItApplies) {
  const auto measurement_note = [&](const std::vector<Submission>& submissions) {
    const auto checked = check_against_template(submissions, options_);
    if (!checked) throw std::runtime_error(to_string(checked.error()));
    const std::vector<std::string> said = notes_with(*checked, std::string("ArArMeasurement") + kRequiredLead);
    return said.empty() ? std::string() : said.front().substr(std::string("ArArMeasurement").size() +
                                                               std::string(kRequiredLead).size());
  };
  const std::string beam = "beamSize (Beam size) empty in every row, beamSizeUnits (Beam size units) empty in every row";
  std::vector<Submission> submissions = fixtures();
  ASSERT_EQ(submissions[0].data_point.arMethodName, "Total fusion - laser");
  ASSERT_EQ(submissions[1].data_point.arMethodName, "Step-heating - laser");
  ASSERT_EQ(submissions[0].measurements.size(), 6u);
  ASSERT_EQ(submissions[1].measurements.size(), 10u);
  EXPECT_EQ(measurement_note(submissions), beam);

  // A step without its number or its share of the gas: of the ten rows of
  // the step-heated data point; the fusion's six do not count.
  {
    std::vector<Submission> steps = submissions;
    steps[1].measurements[2].stepNumber.reset();
    steps[1].measurements[7].stepNumber.reset();
    steps[1].measurements[7].ar39KReleased.reset();
    for (Measurement& m : steps[1].measurements) m.cumulative39ArK.reset();
    EXPECT_EQ(measurement_note(steps),
              "stepNumber (Step number) empty in 2 of the 10 step-heating rows, " + beam +
                  ", Ar39KReleased (39ArK released) empty in 1 of the 10 step-heating rows, cumulative39ArK "
                  "(Cumulative 39ArK released) empty in all 10 step-heating rows");
    // The method is what says so: the same rows under a fusion's are not missed,
    // nor under no method at all (the method is then the thing missing).
    steps[1].data_point.arMethodName = "Total fusion - laser";
    EXPECT_EQ(measurement_note(steps), beam);
    steps[1].data_point.arMethodName.reset();
    EXPECT_EQ(measurement_note(steps), beam);
    // Whatever its case, and whatever heats the steps.
    steps[1].data_point.arMethodName = "step-heating - furnace";
    EXPECT_NE(measurement_note(steps).find("stepNumber (Step number) empty in 2 of the 10 step-heating rows"),
              std::string::npos);
  }

  // A furnace's row has no laser power, and is not missed one.
  {
    std::vector<Submission> heated = submissions;
    Measurement& furnace = heated[1].measurements[0];
    furnace.laserPower.reset();
    furnace.laserPowerUnits.reset();
    furnace.furnaceTemperature = 750.0;
    EXPECT_EQ(measurement_note(heated), beam);
    // A row with neither is missed all three; a power without its units, the
    // units. Fifteen rows have no furnace temperature; two have no power, the
    // furnace's and the one with neither.
    heated[1].measurements[1].laserPower.reset();
    heated[1].measurements[1].laserPowerUnits.reset();
    heated[1].measurements[2].laserPowerUnits.reset();
    EXPECT_EQ(measurement_note(heated),
              beam + ", laserPower (Laser power) empty in 1 of the 15 rows that have no furnace temperature, "
                     "laserPowerUnits (Laser power units) empty in 2 of the 15 rows that have no furnace "
                     "temperature, furnaceTemperature (Furnace temperature) empty in 1 of the 2 rows that have no "
                     "laser power");
  }
}

TEST_F(Xlsx, AValueIsWrittenInItsListsSpellingOrSaidNotToBeThere) {
  std::vector<Submission> submissions = fixtures();
  Submission& fusion = submissions[0];
  Submission& step = submissions[1];
  // Another case, spaces around: the list's entry is meant.
  fusion.age_calc.decayConstantName = "steiger and jager, 1977";
  step.age_calc.decayConstantName = "  Min et al. 2000 ";
  for (Measurement& m : step.measurements) m.plateauStepName = *m.plateauStepName == "Yes" ? "YES" : "no";
  fusion.data_point.analysisScaleName = "single grain";
  // The list has `Renne et al. 2011 `, with a space after it: without the
  // space it is the entry and nothing was respelled.
  fusion.age_calc.airRatioName = "Nier 1950";
  step.data_point.arMethodName = "Step-heating - laser";
  // In no list.
  fusion.age_calc.airRatioName = "Mark et al. 2011";
  fusion.age_summary.interpretationName = "eruption age";
  step.age_summary.interpretationName = "eruption age";
  fusion.sample.sampleKindName = "Tephra";
  fusion.sample.materialName = "biotite separate";
  // The Sample template's lists of sample kinds and archives go on below the
  // rows their validations name (`Mine Waste` in A17 of A2:A16, nine archives
  // after F28): what the sheet lists under the same heading is of the list.
  step.sample.sampleKindName = "mine waste";
  step.sample.archiveName = "University of Bergen";
  // Monitors: one of the list as it stands, one found by name and age, one
  // the list has not, one that could be two entries.
  fusion.age_calc.fluxMonitorName = kKuiper;
  step.age_calc.fluxMonitorName = "ACs 1.1851 Ma";
  Submission third = step;
  third.datapoint_name = "S-STEP again";
  third.age_calc.fluxMonitorName = "Bishop Tuff sanidine 0.7666 Ma";
  Submission fourth = step;
  fourth.datapoint_name = "S-STEP once more";
  fourth.age_calc.fluxMonitorName = "Mt Domedary Biotite 99.44 Ma";
  submissions.push_back(third);
  submissions.push_back(fourth);

  const XlsxResult result = exported(submissions);
  const std::vector<Row> calc = rows_of(result.files[0], "ArArAgeCalc");
  ASSERT_EQ(calc.size(), 4u);
  EXPECT_EQ(calc[0].at("decayConstant"), "Steiger and Jager, 1977");
  EXPECT_EQ(calc[1].at("decayConstant"), "Min et al. 2000");
  EXPECT_EQ(calc[0].at("airRatio"), "Mark et al. 2011");  // as given
  EXPECT_EQ(calc[1].at("airRatio"), "Lee et al. 2006");
  EXPECT_EQ(calc[0].at("fluxMonitor"), kKuiper);
  EXPECT_EQ(calc[1].at("fluxMonitor"), "Alder Creek Rhyolite Sanidine (ACs) from Schaen et al. 2020 (1.1851 Ma)");
  EXPECT_EQ(calc[2].at("fluxMonitor"), "Bishop Tuff sanidine 0.7666 Ma");  // as given
  EXPECT_EQ(calc[3].at("fluxMonitor"), "Mt Domedary Biotite 99.44 Ma");    // as given
  const std::vector<Row> measurements = rows_of(result.files[0], "ArArMeasurement");
  EXPECT_EQ(measurements.at(6).at("plateauStep"), "No");
  EXPECT_EQ(measurements.at(6 + 2).at("plateauStep"), "Yes");
  EXPECT_EQ(rows_of(result.files[0], "ArArDataPoints").at(0).at("analysisScale"), "Single Grain");
  EXPECT_EQ(rows_of(result.files[0], "ArArAgeSummary").at(0).at("interpretation"), "eruption age");
  const std::vector<Row> samples = rows_of(result.files[1], "Samples");
  EXPECT_EQ(samples.at(0).at("sampleKind"), "Tephra");
  EXPECT_EQ(samples.at(0).at("material"), "biotite separate");
  EXPECT_EQ(samples.at(1).at("material"), "Sanidine");
  EXPECT_EQ(samples.at(1).at("sampleKind"), "Mine Waste");
  EXPECT_EQ(samples.at(1).at("lastKnownArchive"), "University of Bergen");

  std::vector<std::string> said;
  for (const std::string& note : result.notes) {
    if (note.find("Required") == std::string::npos) said.push_back(note);
  }
  EXPECT_EQ(said, (std::vector<std::string>{
                      // As they came up: sheet by sheet, row by row; a value twice is said once.
                      // As they came up: sheet by sheet, row by row. A value twice is said once (thirty
                      // steps are in or out of the plateau; three data points have the one constant),
                      // and each note names the list by its heading on the lookup sheet.
                      "ArArDataPoints.analysisScale: 'single grain' written as 'Single Grain' (list: Analysis Scale)",
                      "ArArMeasurement.plateauStep: 'no' written as 'No' (list: Yes/No)",
                      "ArArMeasurement.plateauStep: 'YES' written as 'Yes' (list: Yes/No)",
                      "ArArAgeSummary.interpretation: 'eruption age' is not in the template's list 'Age "
                      "Interpretation'; the importer may refuse the value",
                      "ArArAgeSummary: the rows after row 7 are past the template's formatted rows and are plain",
                      "ArArAgeCalc.decayConstant: 'steiger and jager, 1977' written as 'Steiger and Jager, 1977' "
                      "(list: Decay Constant)",
                      "ArArAgeCalc.airRatio: 'Mark et al. 2011' is not in the template's list 'Air Ratio'; the "
                      "importer may refuse the value",
                      "ArArAgeCalc.decayConstant: '  Min et al. 2000 ' written as 'Min et al. 2000' (list: Decay "
                      "Constant)",
                      "ArArAgeCalc.fluxMonitor: 'ACs 1.1851 Ma' written as 'Alder Creek Rhyolite Sanidine (ACs) "
                      "from Schaen et al. 2020 (1.1851 Ma)' (list: Flux Monitor), found by its name and age",
                      "ArArAgeCalc.fluxMonitor: 'Bishop Tuff sanidine 0.7666 Ma' is not in the template's list "
                      "'Flux Monitor'; the importer may refuse the value",
                      "ArArAgeCalc.fluxMonitor: 'Mt Domedary Biotite 99.44 Ma' could be more than one entry of the "
                      "template's list and was written as given",
                      "Samples.sampleKind: 'Tephra' is not in the template's list 'Sample Kind'; the importer may "
                      "refuse the value",
                      "Samples.material: 'biotite separate' is not in the template's list 'Lithology'; the importer "
                      "may refuse the value",
                      "Samples.sampleKind: 'mine waste' written as 'Mine Waste' (list: Sample Kind)",
                      "Samples.material: 'sanidine' written as 'Sanidine' (list: Lithology)",
                  }));
}

TEST_F(Xlsx, TheCheckSaysWhatTheExportWould) {
  std::vector<Submission> submissions = fixtures();
  submissions[0].age_calc.fluxMonitorName = "Bishop Tuff sanidine 0.7666 Ma";
  submissions[1].measurements[0].ratio40Arx36ArTrapped = 300.0;  // a field without a column
  for (const bool per_sample : {false, true}) {
    for (const bool with_samples : {true, false}) {
      options_.per_sample = per_sample;
      options_.samples = with_samples;
      const auto checked = check_against_template(submissions, options_);
      ASSERT_TRUE(checked.has_value()) << to_string(checked.error());
      EXPECT_FALSE(fs::exists(out_)) << "the check wrote something";
      EXPECT_EQ(*checked, exported(submissions).notes);
      EXPECT_EQ(notes_with(*checked, "Bishop").size(), 1u);
      EXPECT_EQ(notes_with(*checked, "no column for").size(), 1u);
      EXPECT_EQ(notes_with(*checked, "Samples").empty(), !with_samples);
      fs::remove_all(out_);
    }
  }
  // And refuses what the export refuses, with its words.
  options_ = XlsxOptions{};
  options_.template_dir = template_dir();
  submissions[1].datapoint_name = submissions[0].datapoint_name;
  const auto same = check_against_template(submissions, options_);
  ASSERT_FALSE(same.has_value());
  EXPECT_EQ(same.error().kind, ErrorKind::Config);
  EXPECT_EQ(same.error().what, refused(submissions).what);
  EXPECT_FALSE(check_against_template({}, options_).has_value());
  options_.template_dir = dir_ / "nowhere";
  const auto missing = check_against_template(fixtures(), options_);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().kind, ErrorKind::Io);
  EXPECT_NE(missing.error().what.find((dir_ / "nowhere" / kArAr).string()), std::string::npos);
  // The default templates, when none are named.
  EXPECT_TRUE(check_against_template(fixtures()).has_value());
}

// The columns each template marks as Required, as this reads the mark: the
// title in row 3 filled with the colour of the legend's `Required` cell
// (`Field Definitions`!A5, `Column Descriptions`!A1; FFFAD9D6 in both). A
// submission that holds a name and nothing else leaves every one of them
// empty but the two names.
TEST_F(Xlsx, TheRequiredColumnsAreTheOnesTheTemplateColours) {
  Submission bare;
  bare.datapoint_name = "bare";
  bare.sample.name = "S-BARE";
  bare.aliquots.emplace_back();
  bare.measurements.emplace_back();
  const auto checked = check_against_template({bare}, options_);
  ASSERT_TRUE(checked.has_value()) << to_string(checked.error());
  // `key (title)` for each, as the note has them: all in the one row there is.
  const auto every = [](std::initializer_list<const char*> columns, const char* rows = "every row") {
    std::string out;
    for (const char* column : columns) {
      out += (out.empty() ? "" : ", ") + std::string(column) + " empty in " + rows;
    }
    return out;
  };
  const char* const no_furnace = "the one row that has no furnace temperature";
  const auto expected = [&](bool step_heated) {
    const char* const of_steps = "the one step-heating row";
    return std::vector<std::string>{
        std::string("ArArDataPoints") + kRequiredLead +
            every({"arArSetUp (Analytical Key)"}) + ", " +
            (step_heated ? std::string() : every({"arArMethod (Analytical Method)"}) + ", ") +
            every({"irradiationBatchID (Irradiation Batch ID)", "analysisScale (Analysis Scale)",
                   "analyticalUncertaintyType (Analytical Uncertainty Type)",
                   "analyticalUncertaintyUnit (Analytical Uncertainty Units)",
                   "apparentAgeUncertaintyType (Apparent Age Uncertainty Type)",
                   "apparentAgeUncertaintyUnit (Apparent Age Uncertainty Units)",
                   "jValueUncertaintyType (J Value Uncertainty Type)",
                   "jValueUncertaintyUnit (J Value Uncertainty Units)", "analysisUnits (Units for analyses)"}),
        std::string("ArArMeasurement") + kRequiredLead +
            (step_heated ? every({"stepNumber (Step number)"}, of_steps) + ", " : std::string()) +
            every({"beamSize (Beam size)", "beamSizeUnits (Beam size units)"}) + ", " +
            every({"laserPower (Laser power)", "laserPowerUnits (Laser power units)"}, no_furnace) + ", " +
            every({"furnaceTemperature (Furnace temperature)"}, "the one row that has no laser power") + ", " +
            every({"ar40Corr (40Ar (corrected))", "ar40CorrUncertainty (Uncertainty)", "ar39Corr (39Ar (corrected))",
                   "ar39CorrUncertainty (Uncertainty)", "ar38Corr (38Ar (corrected))",
                   "ar38CorrUncertainty (Uncertainty)", "ar37Corr (37Ar (corrected))",
                   "ar37CorrUncertainty (Uncertainty)", "ar36Corr (36Ar (corrected))",
                   "ar36CorrUncertainty (Uncertainty)", "radiogenicArgon (Radiogenic argon proportion (%40Ar*))",
                   "ratio40Arx39ArK (40Ar*/39ArK)", "ratio40Arx39ArKUncertainty (Uncertainty)"}) +
            ", " +
            (step_heated ? every({"Ar39KReleased (39ArK released)", "cumulative39ArK (Cumulative 39ArK released)"},
                                 of_steps) +
                               ", "
                         : std::string()) +
            every({"jValue (J Value)", "jValueUncertainty (J Value Uncertainty)", "apparentAge (Apparent age)",
                   "apparentAgeUncertainty (Uncertainty)"}),
        std::string("ArArAliquot") + kRequiredLead +
            every({"aliquotSize (Aliquot size)", "aliquotUnits (Aliquot size unit)"}),
        std::string("ArArAgeSummary") + kRequiredLead +
            every({"age (Age (Ma))", "ageUncertainty (Uncertainty of Age (Ma))",
                   "calculatedAgeUncertaintyType (Calculated Age Uncertainty Type)",
                   "calculatedAgeUncertaintyTypeUnits (Calculated Age Uncertainty Units)"}),
        std::string("ArArAgeCalc") + kRequiredLead +
            every({"decayConstant (Decay Constant Reference)", "airRatio (Trapped 40Ar/36Ar (Air) Ratio Reference)",
                   "fluxMonitor (Flux Monitor Name and Reference)"}),
        std::string("Samples") + kRequiredLead +
            every({"material (Rock/Mineral (search term))", "latitude (Latitude (WGS84))",
                   "longitude (Longitude (WGS84))"}),
    };
  };
  // Without a method the three columns of a heating step are not missed (the
  // method is); with a step-heating one every Required column is there.
  EXPECT_EQ(*checked, expected(false));
  bare.data_point.arMethodName = "Step-heating - undefined";
  const auto stepped = check_against_template({bare}, options_);
  ASSERT_TRUE(stepped.has_value()) << to_string(stepped.error());
  EXPECT_EQ(*stepped, expected(true));
  bare.data_point.arMethodName.reset();

  // A sheet that gets no row has nothing left empty.
  bare.aliquots.clear();
  bare.measurements.clear();
  const auto fewer = check_against_template({bare}, options_);
  ASSERT_TRUE(fewer.has_value());
  EXPECT_EQ(fewer->size(), 4u);
  EXPECT_TRUE(notes_with(*fewer, "ArArAliquot").empty() && notes_with(*fewer, "ArArMeasurement").empty());

  // With everything set nothing is.
  const auto full = check_against_template({everything()}, options_);
  ASSERT_TRUE(full.has_value());
  EXPECT_EQ(notes_with(*full, "Required"), std::vector<std::string>{});
}

TEST_F(Xlsx, ATemplateThatDoesNotSayIsSaidNotTo) {
  // A legend without its `Required` cell (shared string in the Ar-Ar
  // workbook): which columns are required cannot be known, and that is said,
  // once; the lists still are.
  options_.template_dir = edited_templates("xl/sharedStrings.xml", [](std::string& xml) {
    const std::string was = "<si><t>Required</t></si>";
    const std::size_t at = xml.find(was);
    ASSERT_NE(at, std::string::npos);
    xml.replace(at, was.size(), "<si><t>Compulsory</t></si>");
  });
  auto checked = check_against_template(fixtures(), options_);
  ASSERT_TRUE(checked.has_value()) << to_string(checked.error());
  EXPECT_EQ(notes_with(*checked, kArAr),
            std::vector<std::string>{std::string(kArAr) + ": sheet 'Field Definitions' has no cell that says which "
                                                         "colour marks a Required column; columns left empty are "
                                                         "not reported for this workbook"});
  EXPECT_EQ(notes_with(*checked, "marked Required by the template's colouring").size(), 1u);  // the Sample workbook's
  EXPECT_EQ(notes_with(*checked, "fluxMonitor").size(), 1u);

  // A list that cannot be followed (here a name the workbook does not
  // define): the column is written as given, and that it was not checked is
  // said once.
  fs::remove_all(dir_ / "templates");
  options_.template_dir = edited_templates("xl/worksheets/sheet5.xml", [](std::string& xml) {
    const std::string was = "'Lookup Tables'!$C$2:$C$17";
    const std::size_t at = xml.find(was);
    ASSERT_NE(at, std::string::npos);
    xml.replace(at, was.size(), "FluxMonitors");
  });
  const XlsxResult result = exported(fixtures());
  EXPECT_EQ(notes_with(result.notes, "fluxMonitor"),
            std::vector<std::string>{"ArArAgeCalc.fluxMonitor: the template's list could not be read "
                                     "(FluxMonitors); what was written there was not checked"});
  EXPECT_EQ(rows_of(result.files[0], "ArArAgeCalc").at(0).at("fluxMonitor"), "FC-2 28.201 Ma");

  // A list on a sheet the workbook has not is the same case.
  fs::remove_all(dir_ / "templates");
  options_.template_dir = edited_templates("xl/worksheets/sheet5.xml", [](std::string& xml) {
    const std::string was = "'Lookup Tables'!$C$2:$C$17";
    xml.replace(xml.find(was), was.size(), "'Gone'!$C$2:$C$17");
  });
  checked = check_against_template(fixtures(), options_);
  ASSERT_TRUE(checked.has_value()) << to_string(checked.error());
  EXPECT_EQ(notes_with(*checked, "fluxMonitor"),
            std::vector<std::string>{"ArArAgeCalc.fluxMonitor: the template's list could not be read "
                                     "('Gone'!$C$2:$C$17); what was written there was not checked"});
}

// ---- Smaller things ------------------------------------------------------------

// The XML of the cell at `reference` in a sheet's part.
std::string cell_xml(const std::string& sheet_xml, const std::string& reference) {
  const std::size_t from = sheet_xml.find("<c r=\"" + reference + "\"");
  if (from == std::string::npos) return {};
  const std::size_t self = sheet_xml.find("/>", from);
  const std::size_t tag = sheet_xml.find('>', from);
  if (self != std::string::npos && self + 1 == tag) return sheet_xml.substr(from, tag + 1 - from);
  const std::size_t to = sheet_xml.find("</c>", from);
  return to == std::string::npos ? std::string() : sheet_xml.substr(from, to + 4 - from);
}

TEST_F(Xlsx, ANumberIsWrittenAsANumber) {
  const std::vector<Submission> submissions = fixtures();
  const XlsxResult result = exported(submissions);
  const auto part = [](const fs::path& file, const std::string& name) {
    const auto entries = ooxml::read_zip(file);
    if (!entries) throw std::runtime_error(to_string(entries.error()));
    for (const ooxml::ZipEntry& entry : *entries) {
      if (entry.name == name) return entry.data;
    }
    throw std::runtime_error("no part " + name);
  };
  // A number is a cell with a <v> and no type; text is an inline string.
  const auto expect_number = [](const std::string& xml, const std::string& reference, double value) {
    const std::string cell = cell_xml(xml, reference);
    ASSERT_FALSE(cell.empty()) << reference;
    EXPECT_EQ(cell.find(" t=\""), std::string::npos) << cell;
    EXPECT_EQ(cell.find("<is>"), std::string::npos) << cell;
    const std::size_t v = cell.find("<v>");
    ASSERT_NE(v, std::string::npos) << cell;
    EXPECT_EQ(std::stod(cell.substr(v + 3)), value) << cell;
  };
  const auto expect_text = [](const std::string& xml, const std::string& reference) {
    const std::string cell = cell_xml(xml, reference);
    ASSERT_FALSE(cell.empty()) << reference;
    EXPECT_NE(cell.find(" t=\"inlineStr\""), std::string::npos) << cell;
    EXPECT_EQ(cell.find("<v>"), std::string::npos) << cell;
  };

  // ArArAgeSummary, row 5 (S-FUS): the count, the age and its uncertainty,
  // MSWD and p.
  const AgeSummary& summary = submissions[0].age_summary;
  const std::string ages = part(result.files[0], "xl/worksheets/sheet4.xml");
  expect_number(ages, "C5", static_cast<double>(*summary.nAnalysisGrouped));
  expect_number(ages, "E5", *summary.age);
  expect_number(ages, "F5", *summary.ageUncertainty);
  expect_number(ages, "I5", *summary.mswd);
  expect_number(ages, "J5", *summary.agepvalue);
  expect_text(ages, "A5");
  expect_text(ages, "D5");
  expect_text(ages, "K5");  // the flag, as Yes or No
  // ArArMeasurement: every number of the first step of S-STEP (row 11).
  const std::string measured = part(result.files[0], "xl/worksheets/sheet2.xml");
  const Measurement& m = submissions[1].measurements[0];
  expect_number(measured, "D11", *m.stepNumber);
  expect_number(measured, "G11", *m.laserPower);
  expect_number(measured, "J11", *m.ar40Corr);
  expect_number(measured, "K11", *m.ar40CorrUncertainty);
  expect_number(measured, "AY11", *m.jValue);
  expect_number(measured, "BA11", *m.apparentAge);
  expect_text(measured, "C11");
  int numbers = 0;
  const json fields = to_json(m);
  for (const auto& [key, value] : fields.items()) numbers += value.is_number() ? 1 : 0;
  const std::size_t row = measured.find("<row r=\"11\"");
  const std::string eleventh = measured.substr(row, measured.find("</row>", row) - row);
  int valued = 0;
  for (std::size_t at = eleventh.find("<v>"); at != std::string::npos; at = eleventh.find("<v>", at + 1)) ++valued;
  EXPECT_EQ(valued, numbers);
  EXPECT_GT(numbers, 40);
  // Samples, row 6 (S-STEP): the place and the elevation.
  const std::string samples = part(result.files[1], "xl/worksheets/sheet1.xml");
  expect_number(samples, "G6", 37.75);
  expect_number(samples, "H6", -106.9);
  expect_number(samples, "J6", 2850.0);
  expect_text(samples, "A6");
}

TEST_F(Xlsx, AFailedSaveTakesBackWhatWasWritten) {
  // The second workbook cannot be written: the name its bytes go to first is
  // a directory.
  fs::create_directories(out_ / (std::string("Samples_") + kStamp + ".xlsx.part"));
  Error error = refused(fixtures());
  EXPECT_EQ(error.kind, ErrorKind::Io);
  EXPECT_NE(error.what.find("earthbank: "), std::string::npos) << error.what;
  EXPECT_NE(error.what.find(std::string("Samples_") + kStamp + ".xlsx"), std::string::npos) << error.what;
  EXPECT_EQ(error.what.find("could not be removed"), std::string::npos) << error.what;
  // The first is gone again, and nothing else came or went.
  EXPECT_FALSE(fs::exists(out_ / (std::string("ArArDataPoint_") + kStamp + ".xlsx")));
  EXPECT_EQ(std::distance(fs::directory_iterator(out_), fs::directory_iterator()), 1);

  // Per sample, the third of three: both written before it are taken back.
  options_.per_sample = true;
  error = refused(fixtures());
  EXPECT_EQ(error.kind, ErrorKind::Io);
  EXPECT_EQ(std::distance(fs::directory_iterator(out_), fs::directory_iterator()), 1);

  // A directory this call made goes with them.
  fs::remove_all(out_);
  options_.per_sample = false;
  options_.template_dir = dir_ / "nowhere";
  refused(fixtures());
  EXPECT_FALSE(fs::exists(out_));

  // What cannot be taken back is named. (A directory that is not empty
  // stands in for a file that will not go.)
  const fs::path gone = dir_ / "written.xlsx";
  const fs::path stuck = dir_ / "stuck.xlsx";
  spit(gone, "a workbook");
  fs::create_directories(stuck / "inside");
  EXPECT_EQ(detail::take_back({gone, stuck, dir_ / "never-there.xlsx"}), std::vector<fs::path>{stuck});
  EXPECT_FALSE(fs::exists(gone));
  EXPECT_TRUE(fs::exists(stuck));
  EXPECT_EQ(detail::take_back({}), std::vector<fs::path>{});
}

TEST_F(Xlsx, TemplatesThatAreNowhereAreSaidWithEveryPlaceLooked) {
  const fs::path bin = dir_ / "prefix" / "bin";
  fs::create_directories(bin);
  const fs::path share = dir_ / "prefix" / "share" / "pychron" / "earthbank" / "templates";
  const fs::path bundle = dir_ / "prefix" / "Resources" / "earthbank" / "templates";
  const fs::path source = dir_ / "build-machine" / "src" / "libs" / "earthbank" / "templates";

  // Nothing set, nothing installed, and the source tree is another
  // machine's: no directory, and its path is not given out.
  detail::TemplateSearch search = detail::find_templates(std::nullopt, bin, source);
  EXPECT_TRUE(search.found.empty());
  EXPECT_EQ(search.tried,
            (std::vector<std::string>{"the directory PYCHRON_EARTHBANK_TEMPLATES names (it is not set)",
                                      fs::weakly_canonical(share).string(), fs::weakly_canonical(bundle).string(),
                                      "the source tree this program was built from (it is not on this machine)"}));
  const std::string message = detail::templates_not_found(search);
  EXPECT_EQ(message, "earthbank: the workbook templates were not found. Looked in: " + search.tried[0] + "; " +
                         search.tried[1] + "; " + search.tried[2] + "; " + search.tried[3]);
  EXPECT_EQ(message.find("build-machine"), std::string::npos) << message;
  // An empty name is no name.
  EXPECT_EQ(detail::find_templates(std::string(), bin, source).tried, search.tried);
  // A program that does not know where it is.
  search = detail::find_templates(std::nullopt, {}, source);
  EXPECT_TRUE(search.found.empty());
  EXPECT_EQ(search.tried,
            (std::vector<std::string>{"the directory PYCHRON_EARTHBANK_TEMPLATES names (it is not set)",
                                      "beside the program (where it runs from is not known)",
                                      "the source tree this program was built from (it is not on this machine)"}));

  // Each place in turn, the earlier one first.
  fs::create_directories(source);
  search = detail::find_templates(std::nullopt, bin, source);
  EXPECT_EQ(search.found, source);
  EXPECT_EQ(search.tried.size(), 4u);
  EXPECT_EQ(search.tried.back(), source.string());
  fs::create_directories(bundle);
  EXPECT_EQ(detail::find_templates(std::nullopt, bin, source).found, fs::weakly_canonical(bundle));
  fs::create_directories(share);
  search = detail::find_templates(std::nullopt, bin, source);
  EXPECT_EQ(search.found, fs::weakly_canonical(share));
  EXPECT_EQ(search.tried.size(), 2u);
  // A named directory is taken at the user's word, there or not.
  search = detail::find_templates((dir_ / "named").string(), bin, source);
  EXPECT_EQ(search.found, dir_ / "named");
  EXPECT_EQ(search.tried, std::vector<std::string>{"the directory PYCHRON_EARTHBANK_TEMPLATES names (" +
                                                   (dir_ / "named").string() + ")"});
}

}  // namespace
}  // namespace pychron::earthbank
