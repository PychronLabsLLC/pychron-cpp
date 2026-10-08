#include "pychron/earthbank/xlsx.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ooxml.hpp"
#include "pychron/core/env.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/process.hpp"
#include "pychron/earthbank/model.hpp"
#include "pychron/earthbank/vocab.hpp"

namespace pychron::earthbank {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// ---------------------------------------------------------------- the templates

// What a column's value is a field of. `Submission` is what a submission
// carries beside its records: the data point's name, the set-up's and the
// irradiation's; `None` is a column nothing fills.
enum class Rec { None, Submission, Sample, Location, DataPoint, Aliquot, Measurement, AgeCalc, AgeSummary };

// A data sheet: the record its rows are of and the last of the template's
// formatted (placeholder) rows. A row written past that is a plain one; the
// number only decides whether a note says so, and is the vendor file's as of
// the templates named below.
struct SheetSpec {
  std::string_view name;
  Rec record;
  int formatted_to;
};

constexpr SheetSpec kArArSheets[] = {
    {"ArArDataPoints", Rec::DataPoint, 927}, {"ArArMeasurement", Rec::Measurement, 863},
    {"ArArAliquot", Rec::Aliquot, 957},      {"ArArAgeSummary", Rec::AgeSummary, 7},
    {"ArArAgeCalc", Rec::AgeCalc, 955},
};
constexpr SheetSpec kSampleSheets[] = {{"Samples", Rec::Sample, 883}};

constexpr std::string_view kArArTemplate = "ArArDataPoint_Template2026.xlsx";
constexpr std::string_view kSampleTemplate = "Sample_Template_v2025-04-16.xlsx";

// Row 4 of a data sheet names the field of each column; data starts below.
// (`Datapoint Props` and `Sample Props` name theirs in row 1. Nothing is
// written to them: a submission has no such rows.)
constexpr int kFieldRow = 4;
constexpr int kFirstDataRow = 5;

// The column every Ar-Ar sheet has for the data point's name, and the three
// things of a submission a column can take.
constexpr std::string_view kLinkColumn = "datapointName";
constexpr std::string_view kDatapointName = "datapoint_name";
constexpr std::string_view kSetupName = "setup_name";
constexpr std::string_view kIrradiationName = "irradiation_name";

// How a text value is written where the sheet asks for another form than the
// record's (`YYYY-MM-DD`, `YYYY-MM-DDTHH:MM:SSZ`):
//   Date       `YYYY-MM-DD`: ArArDataPoints `analysisDate` ("Date of data
//              acquisition YYYY-MM-DD"), as the record has it;
//   MinuteUtc  `YYYY-MM-DD hh:mm UTC`: ArArMeasurement `analysisTime`, whose
//              row 2 reads "YYYY-MM-DD hh:mm z" ("e.g. 2023-07-02 14:30 AEST");
//   DayFirst   `dd.mm.yyyy`: Samples `collectDateMin` and `collectDateMax`,
//              whose row 2 reads "Date dd.mm.yyyy".
enum class Form { Plain, Date, MinuteUtc, DayFirst };

// The columns that do not take their sheet's record's field of the column's
// own name, or that name plus `Name`. Matching is exact, case included.
// From the legacy exporter's SHEET_OVERRIDES and from the templates' row 4;
// what differs from legacy:
//   - `arArSetUp` and `irradiationBatchID` take the names the submission
//     carries. The sheet asks for a search term that finds the set-up or the
//     batch; legacy wrote the record's `arArAnalyticalSetUpId` and
//     `irradiationBatchIDId` there, which are row numbers of the service and
//     not terms (its upload sent the names, as here).
//   - `sampleName` of ArArDataPoints takes the sample's name (legacy left the
//     column empty and the importer had nothing to find the sample by).
//   - `jValueUncertaintyType` and `jValueUncertaintyUnit` are columns of the
//     data point in this template, with a capital V the fields do not have.
//     The type is a field of the measurement only: the column takes what the
//     data point's measurements all say.
//   - `elevationDepthAccuracy` had an entry naming a field no record has.
struct Exception {
  std::string_view sheet, key;
  Rec record;
  std::string_view field;
  Form form = Form::Plain;
};

constexpr Exception kExceptions[] = {
    {"ArArDataPoints", kLinkColumn, Rec::Submission, kDatapointName},
    {"ArArDataPoints", "sampleName", Rec::Sample, "name"},
    {"ArArDataPoints", "arArSetUp", Rec::Submission, kSetupName},
    {"ArArDataPoints", "arArMethod", Rec::DataPoint, "arMethodName"},
    {"ArArDataPoints", "irradiationBatchID", Rec::Submission, kIrradiationName},
    {"ArArDataPoints", "analysisDate", Rec::DataPoint, "analysisDate", Form::Date},
    {"ArArDataPoints", "jValueUncertaintyType", Rec::Measurement, "jvalueUncertaintyTypeName"},
    {"ArArDataPoints", "jValueUncertaintyUnit", Rec::DataPoint, "jvalueUncertaintyUnitName"},
    {"ArArDataPoints", "datapoint.literature", Rec::DataPoint, "literatureName"},
    {"ArArDataPoints", "datapoint.funding", Rec::DataPoint, "fundingName"},

    {"ArArMeasurement", kLinkColumn, Rec::Submission, kDatapointName},
    {"ArArMeasurement", "analysisTime", Rec::Measurement, "analysisTime", Form::MinuteUtc},
    {"ArArMeasurement", "Ar39KReleased", Rec::Measurement, "ar39KReleased"},

    {"ArArAliquot", kLinkColumn, Rec::Submission, kDatapointName},
    {"ArArAgeSummary", kLinkColumn, Rec::Submission, kDatapointName},
    {"ArArAgeCalc", kLinkColumn, Rec::Submission, kDatapointName},

    {"Samples", "sampleName", Rec::Sample, "name"},
    {"Samples", "sampleComment", Rec::Sample, "description"},
    {"Samples", "latitude", Rec::Location, "lat"},
    {"Samples", "longitude", Rec::Location, "lon"},
    {"Samples", "latLonPrecision", Rec::Location, "latLonPrecision"},
    {"Samples", "elevationGround", Rec::Sample, "referenceElevation"},
    {"Samples", "elevationDepthMin", Rec::Sample, "relativeElevationMin"},
    {"Samples", "elevationDepthMax", Rec::Sample, "relativeElevationMax"},
    {"Samples", "unitName", Rec::Sample, "stratographicUnitName"},
    {"Samples", "ageMin", Rec::Sample, "rockUnitAgeMin"},
    {"Samples", "ageMax", Rec::Sample, "rockUnitAgeMax"},
    {"Samples", "unitAgeDescription", Rec::Sample, "rockUnitAgeDescription"},
    {"Samples", "collectDateMin", Rec::Sample, "collectDateMin", Form::DayFirst},
    {"Samples", "collectDateMax", Rec::Sample, "collectDateMax", Form::DayFirst},
    {"Samples", "lastKnownArchive", Rec::Sample, "archiveName"},
    {"Samples", "Tag", Rec::None, ""},
};

// Fields that say again what another field's column holds. Such a field has
// no column of its own and is not missed while the two agree.
struct Restated {
  Rec record;
  std::string_view field;
  Rec as_record;
  std::string_view as_field;
};

constexpr Restated kRestated[] = {
    {Rec::Measurement, "analysisUnits", Rec::DataPoint, "analysisUnits"},
    {Rec::Measurement, "apparentAgeUncertainityTypeName", Rec::DataPoint, "apparentAgeUncertaintyTypeName"},
    {Rec::AgeSummary, "ageUncertaintyTypeName", Rec::AgeSummary, "calculatedAgeUncertaintyTypeName"},
    {Rec::Sample, "sampleID", Rec::Sample, "name"},
};

// ---------------------------------------------------------------- the records

#define PYCHRON_EARTHBANK_FIELD_NAME(kind, name) std::string_view(#name),
constexpr std::string_view kSampleFields[] = {PYCHRON_EARTHBANK_SAMPLE_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
constexpr std::string_view kLocationFields[] = {PYCHRON_EARTHBANK_LOCATION_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
constexpr std::string_view kDataPointFields[] = {PYCHRON_EARTHBANK_DATA_POINT_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
constexpr std::string_view kAliquotFields[] = {PYCHRON_EARTHBANK_ALIQUOT_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
constexpr std::string_view kMeasurementFields[] = {
    PYCHRON_EARTHBANK_MEASUREMENT_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
constexpr std::string_view kAgeCalcFields[] = {PYCHRON_EARTHBANK_AGE_CALC_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
constexpr std::string_view kAgeSummaryFields[] = {PYCHRON_EARTHBANK_AGE_SUMMARY_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
#undef PYCHRON_EARTHBANK_FIELD_NAME

std::span<const std::string_view> fields_of(Rec record) {
  switch (record) {
    case Rec::Sample: return kSampleFields;
    case Rec::Location: return kLocationFields;
    case Rec::DataPoint: return kDataPointFields;
    case Rec::Aliquot: return kAliquotFields;
    case Rec::Measurement: return kMeasurementFields;
    case Rec::AgeCalc: return kAgeCalcFields;
    case Rec::AgeSummary: return kAgeSummaryFields;
    case Rec::None:
    case Rec::Submission: break;
  }
  return {};
}

bool has_field(Rec record, std::string_view field) {
  const auto fields = fields_of(record);
  return std::find(fields.begin(), fields.end(), field) != fields.end();
}

std::string_view name_of(Rec record) {
  switch (record) {
    case Rec::Submission: return "Submission";
    case Rec::Sample: return "Sample";
    case Rec::Location: return "Location";
    case Rec::DataPoint: return "DataPoint";
    case Rec::Aliquot: return "Aliquot";
    case Rec::Measurement: return "Measurement";
    case Rec::AgeCalc: return "AgeCalc";
    case Rec::AgeSummary: return "AgeSummary";
    case Rec::None: break;
  }
  return "";
}

// The sheet whose rows are of `record`; a location is written in its
// sample's row.
std::string_view sheet_of(Rec record) {
  if (record == Rec::Location) record = Rec::Sample;
  for (const SheetSpec& sheet : kArArSheets) {
    if (sheet.record == record) return sheet.name;
  }
  for (const SheetSpec& sheet : kSampleSheets) {
    if (sheet.record == record) return sheet.name;
  }
  return "";
}

// The service's row numbers. They are what an upload sends; a workbook names
// things, and the website finds them.
bool is_id(std::string_view field) { return field == "id" || field.ends_with("Id"); }

// A submission's records as to_json gives them: only what is set, by service
// name.
struct Records {
  const Submission* submission = nullptr;
  json sample, location, data_point, age_calc, age_summary;
  std::vector<json> aliquots, measurements;
};

Records records_of(const Submission& s) {
  Records out;
  out.submission = &s;
  out.sample = to_json(s.sample);
  out.location = s.location ? to_json(*s.location) : json::object();
  out.data_point = to_json(s.data_point);
  out.age_calc = to_json(s.age_calc);
  out.age_summary = to_json(s.age_summary);
  for (const Aliquot& a : s.aliquots) out.aliquots.push_back(to_json(a));
  for (const Measurement& m : s.measurements) out.measurements.push_back(to_json(m));
  return out;
}

// The records of kind `record` that a submission has: one, or one per row.
std::span<const json> all_of(const Records& r, Rec record) {
  switch (record) {
    case Rec::Sample: return {&r.sample, 1};
    case Rec::Location: return {&r.location, 1};
    case Rec::DataPoint: return {&r.data_point, 1};
    case Rec::AgeCalc: return {&r.age_calc, 1};
    case Rec::AgeSummary: return {&r.age_summary, 1};
    case Rec::Aliquot: return r.aliquots;
    case Rec::Measurement: return r.measurements;
    case Rec::None:
    case Rec::Submission: break;
  }
  return {};
}

// ---------------------------------------------------------------- notes

// What the user is told, each thing once, in the order it came up.
class Notes {
 public:
  void add(std::string note) {
    if (seen_.insert(note).second) list_.push_back(std::move(note));
  }
  void no_column(Rec record, std::string_view field) {
    const std::string whose = record == Rec::Location ? "the location's " : "";
    add(std::string(sheet_of(record)) + ": no column for " + whose + std::string(field));
  }
  std::vector<std::string> take() { return std::move(list_); }

 private:
  std::vector<std::string> list_;
  std::set<std::string> seen_;
};

// ---------------------------------------------------------------- what goes where

struct Column {
  std::size_t index = 0;  // from 0: column A
  std::string key;
  Rec record = Rec::None;
  std::string field;
  Form form = Form::Plain;
};

struct SheetPlan {
  SheetSpec spec;
  std::size_t width = 0;  // columns from A to the last that is named
  std::vector<Column> columns;
  bool linked = false;  // has the data point's name
};

// A template's data sheets, read from its row 4, and every field some column
// of them takes.
struct Plan {
  std::vector<SheetPlan> sheets;
  std::set<std::pair<Rec, std::string>> held;
};

Column column_of(const SheetSpec& sheet, std::size_t index, const std::string& key) {
  Column out{index, key, Rec::None, {}, Form::Plain};
  for (const Exception& e : kExceptions) {
    if (e.sheet != sheet.name || e.key != key) continue;
    out.record = e.record;
    out.field = std::string(e.field);
    out.form = e.form;
    return out;
  }
  for (const std::string& field : {key, key + "Name"}) {
    if (!has_field(sheet.record, field) || is_id(field)) continue;
    out.record = sheet.record;
    out.field = field;
    return out;
  }
  return out;
}

Unexpected<Error> failed(Error error, const std::string& context = {}) {
  error.what = "earthbank: " + context + error.what;
  return fail(std::move(error));
}

Result<Plan> plan_of(const ooxml::Workbook& book, std::span<const SheetSpec> sheets, const fs::path& file) {
  const std::vector<std::string> present = book.sheet_names();
  Plan plan;
  for (const SheetSpec& sheet : sheets) {
    if (std::find(present.begin(), present.end(), sheet.name) == present.end()) {
      return fail(ErrorKind::Protocol,
                  "earthbank: " + file.string() + " is not the template expected: it has no sheet " +
                      std::string(sheet.name));
    }
    auto keys = book.row_values(sheet.name, kFieldRow);
    if (!keys) return failed(keys.error(), file.string() + ": ");
    SheetPlan out{sheet, keys->size(), {}, false};
    for (std::size_t i = 0; i < keys->size(); ++i) {
      if ((*keys)[i].empty()) continue;
      out.columns.push_back(column_of(sheet, i, (*keys)[i]));
      const Column& column = out.columns.back();
      if (column.record == Rec::Submission && column.field == kDatapointName) out.linked = true;
      if (column.record != Rec::None) plan.held.emplace(column.record, column.field);
    }
    plan.sheets.push_back(std::move(out));
  }
  return plan;
}

// ---------------------------------------------------------------- a cell

bool digits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

int number(std::string_view text) {
  int out = 0;
  for (const char c : text) out = out * 10 + (c - '0');
  return out;
}

// A date or a time as the records carry one: `YYYY-MM-DD`, alone or followed
// by `THH:MM:SS` (with or without a fraction) and `Z`.
struct IsoTime {
  std::string_view year, month, day;
  std::string_view hour, minute;  // empty for a date alone
};

std::optional<IsoTime> parse_iso(std::string_view text) {
  if (text.size() < 10 || text[4] != '-' || text[7] != '-') return std::nullopt;
  IsoTime out{text.substr(0, 4), text.substr(5, 2), text.substr(8, 2), {}, {}};
  if (!digits(out.year) || !digits(out.month) || !digits(out.day)) return std::nullopt;
  const std::chrono::year_month_day date(std::chrono::year(number(out.year)),
                                         std::chrono::month(static_cast<unsigned>(number(out.month))),
                                         std::chrono::day(static_cast<unsigned>(number(out.day))));
  if (!date.ok()) return std::nullopt;
  if (text.size() == 10) return out;

  // THH:MM:SS[.fff]Z
  if (text.size() < 20 || text[10] != 'T' || text[13] != ':' || text[16] != ':' || text.back() != 'Z') {
    return std::nullopt;
  }
  out.hour = text.substr(11, 2);
  out.minute = text.substr(14, 2);
  const std::string_view second = text.substr(17, 2);
  if (!digits(out.hour) || !digits(out.minute) || !digits(second)) return std::nullopt;
  if (number(out.hour) > 23 || number(out.minute) > 59 || number(second) > 60) return std::nullopt;
  const std::string_view fraction = text.substr(19, text.size() - 20);
  if (!fraction.empty() && (fraction.front() != '.' || !digits(fraction.substr(1)))) return std::nullopt;
  return out;
}

// The text of `column` in the form its sheet asks for. A value that is not
// in the records' form cannot be turned into the sheet's: it is written as
// it is, and said.
std::string in_form(const Column& column, const std::string& text, Notes& notes) {
  if (column.form == Form::Plain) return text;
  const auto iso = parse_iso(text);
  const bool needs_time = column.form == Form::MinuteUtc;
  if (!iso || (needs_time && iso->hour.empty())) {
    notes.add(std::string(sheet_of(column.record)) + ": " + column.field + " " + text +
              (needs_time ? " is not a UTC time (YYYY-MM-DDTHH:MM:SSZ)" : " is not a date (YYYY-MM-DD)") +
              " and was written as it is");
    return text;
  }
  const std::string year(iso->year), month(iso->month), day(iso->day);
  switch (column.form) {
    case Form::Date: return year + "-" + month + "-" + day;
    case Form::DayFirst: return day + "." + month + "." + year;
    case Form::MinuteUtc:
      return year + "-" + month + "-" + day + " " + std::string(iso->hour) + ":" + std::string(iso->minute) + " UTC";
    case Form::Plain: break;
  }
  return text;
}

// A number as a number (an integer too), a flag as the template's `Yes` or
// `No` (its `Yes/No` list; `preferredAge` is the one flag there is), text in
// the column's form.
ooxml::Cell cell_of(const Column& column, const json& value, Notes& notes) {
  if (value.is_boolean()) return {std::string(value.get<bool>() ? kYes : kNo)};
  if (value.is_number()) return {value.get<double>()};
  if (value.is_string()) return {in_form(column, value.get<std::string>(), notes)};
  return {};
}

// ---------------------------------------------------------------- a row

// What the records of `list` say for `field` when they say one thing; none
// when no record says anything, or when two disagree (`disagree` set).
const json* shared(std::span<const json> list, const std::string& field, bool& disagree) {
  const json* found = nullptr;
  for (const json& one : list) {
    const auto it = one.find(field);
    if (it == one.end()) continue;
    if (found != nullptr && *found != *it) {
      disagree = true;
      return nullptr;
    }
    found = &*it;
  }
  return found;
}

// The value `record`.`field` has for a row of `own` (a record of kind
// `own_record`): the row's own, or the one its submission has. A field of a
// record the submission has several of (the measurements, from the data
// point's row) is what they share.
const json* value_of(Rec record, const std::string& field, Rec own_record, const json& own, const Records& r,
                     bool& disagree) {
  if (record == own_record) {
    const auto it = own.find(field);
    return it == own.end() ? nullptr : &*it;
  }
  return shared(all_of(r, record), field, disagree);
}

std::vector<ooxml::Cell> row_of(const SheetPlan& sheet, const json& own, const Records& r, Notes& notes) {
  std::vector<ooxml::Cell> cells(sheet.width);
  for (const Column& column : sheet.columns) {
    if (column.record == Rec::None) continue;
    if (column.record == Rec::Submission) {
      const Submission& s = *r.submission;
      cells[column.index] = {column.field == kDatapointName ? s.datapoint_name
                             : column.field == kSetupName   ? s.setup_name
                                                            : s.irradiation_name};
      continue;
    }
    bool disagree = false;
    const json* value = value_of(column.record, column.field, sheet.spec.record, own, r, disagree);
    // One column and two values: it holds neither, and the field is without
    // a column after all.
    if (disagree) notes.no_column(column.record, column.field);
    if (value != nullptr) cells[column.index] = cell_of(column, *value, notes);
  }
  return cells;
}

// Notes each field of `own` that is set and that no column of the template
// takes.
void note_unheld(Rec record, const json& own, const Records& r, const Plan& plan, Notes& notes) {
  for (const auto& [field, value] : own.items()) {
    if (is_id(field) || plan.held.contains({record, field})) continue;
    bool said_elsewhere = false;
    for (const Restated& again : kRestated) {
      if (again.record != record || again.field != field) continue;
      if (!plan.held.contains({again.as_record, std::string(again.as_field)})) continue;
      bool disagree = false;
      const json* other = value_of(again.as_record, std::string(again.as_field), record, own, r, disagree);
      said_elsewhere = other != nullptr && *other == value;
    }
    if (!said_elsewhere) notes.no_column(record, field);
  }
}

// The rows `r` adds to `sheet`, with the notes they give rise to.
void add_rows(const SheetPlan& sheet, const Records& r, const Plan& plan, std::vector<std::vector<ooxml::Cell>>& rows,
              Notes& notes) {
  for (const json& own : all_of(r, sheet.spec.record)) {
    rows.push_back(row_of(sheet, own, r, notes));
    note_unheld(sheet.spec.record, own, r, plan, notes);
  }
  if (sheet.spec.record == Rec::Sample) note_unheld(Rec::Location, r.location, r, plan, notes);
}

// Writes the rows of `records` into every data sheet of `book`. `linked`:
// the sheets are tied together by the data point's name.
Result<void> fill(ooxml::Workbook& book, const Plan& plan, std::span<const Records* const> records, bool linked,
                  Notes& notes) {
  for (const SheetPlan& sheet : plan.sheets) {
    std::vector<std::vector<ooxml::Cell>> rows;
    for (const Records* r : records) add_rows(sheet, *r, plan, rows, notes);
    if (linked && !sheet.linked) {
      notes.add(std::string(sheet.spec.name) + ": no column for " + std::string(kLinkColumn));
    }
    if (kFirstDataRow + static_cast<int>(rows.size()) - 1 > sheet.spec.formatted_to) {
      notes.add(std::string(sheet.spec.name) + ": the rows after row " + std::to_string(sheet.spec.formatted_to) +
                " are past the template's formatted rows and are plain");
    }
    if (rows.empty()) continue;  // the sheet stays the template's
    if (auto wrote = book.write_rows(sheet.spec.name, kFirstDataRow, rows); !wrote) return failed(wrote.error());
  }
  return {};
}

// ---------------------------------------------------------------- file names

// `yyyymmdd-hhmmss`, UTC.
std::string stamp_of(std::chrono::system_clock::time_point now) {
  const auto second = std::chrono::floor<std::chrono::seconds>(now);
  const auto midnight = std::chrono::floor<std::chrono::days>(second);
  const std::chrono::year_month_day day(midnight);
  const std::chrono::hh_mm_ss clock(second - midnight);
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%04d%02u%02u-%02d%02d%02d", static_cast<int>(day.year()),
                static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()),
                static_cast<int>(clock.hours().count()), static_cast<int>(clock.minutes().count()),
                static_cast<int>(clock.seconds().count()));
  return buffer;
}

// A sample's name as part of a file name: ASCII letters, digits, `-` and `_`
// stay and every other byte is `_`, so no separator, no `..`, no drive letter
// and nothing a shell or a file system reads its own way survives. Cut to a
// length every file system takes.
std::string safe_name(std::string_view name) {
  constexpr std::size_t kLongest = 80;
  std::string out;
  for (const char c : name.substr(0, kLongest)) {
    const auto u = static_cast<unsigned char>(c);
    const bool keep = (u < 0x80 && std::isalnum(u) != 0) || c == '-' || c == '_';
    out.push_back(keep ? c : '_');
  }
  return out.empty() ? std::string("sample") : out;
}

// File names in one directory that are not taken, by a file there or by a
// name already given out. Names that differ only in case are one name: they
// are to the file systems of macOS and Windows.
class FileNames {
 public:
  explicit FileNames(fs::path dir) : dir_(std::move(dir)) {}

  // `<stem>.xlsx`, or `<stem>-2.xlsx`, `<stem>-3.xlsx`, ...
  fs::path next(const std::string& stem) {
    for (int n = 1;; ++n) {
      const std::string name = stem + (n == 1 ? std::string() : "-" + std::to_string(n)) + ".xlsx";
      std::string folded = name;
      std::transform(folded.begin(), folded.end(), folded.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      std::error_code ec;
      // symlink_status: a link that leads nowhere is a name taken too.
      if (given_.contains(folded) || fs::exists(fs::symlink_status(dir_ / name, ec))) continue;
      given_.insert(std::move(folded));
      return dir_ / name;
    }
  }

 private:
  fs::path dir_;
  std::set<std::string> given_;
};

// ---------------------------------------------------------------- the workbooks

struct Pending {
  ooxml::Workbook book;
  std::string stem;
};

Result<ooxml::Workbook> open_template(const fs::path& file) {
  auto book = ooxml::Workbook::open(file);
  if (!book) return failed(book.error());
  return book;
}

// A workbook cannot tie rows together by a name two data points share, or by
// none.
Result<void> check_names(const std::vector<Submission>& submissions) {
  if (submissions.empty()) return fail(ErrorKind::Config, "earthbank: no submission to write");
  std::set<std::string_view> names;
  for (const Submission& s : submissions) {
    if (s.datapoint_name.empty()) {
      return fail(ErrorKind::Config, "earthbank: the data point of sample " + s.sample.name.value_or("(unnamed)") +
                                         " has no name: its rows could not be tied to it");
    }
    if (!names.insert(s.datapoint_name).second) {
      return fail(ErrorKind::Config, "earthbank: two data points are named " + s.datapoint_name +
                                         ": their rows could not be told apart");
    }
  }
  return {};
}

// The submissions whose sample gets a row: each sample once. Two data points
// of one sample carry the same sample; two different samples of one name
// cannot both be imported (the name is the sample's key on the service).
Result<std::vector<const Records*>> samples_of(const std::vector<Records>& records) {
  std::vector<const Records*> out;
  for (const Records& r : records) {
    bool written = false;
    for (const Records* earlier : out) {
      if (earlier->sample == r.sample && earlier->location == r.location) {
        written = true;
        break;
      }
      const auto name = r.sample.find("name");
      if (name == r.sample.end() || earlier->sample.value("name", std::string()) != name->get<std::string>()) continue;
      return fail(ErrorKind::Config, "earthbank: two different samples are named " + name->get<std::string>() +
                                         " (data points " + earlier->submission->datapoint_name + " and " +
                                         r.submission->datapoint_name + ")");
    }
    if (!written) out.push_back(&r);
  }
  return out;
}

}  // namespace

fs::path default_template_dir() { return default_template_dir(executable_dir()); }

fs::path default_template_dir(const fs::path& program_dir) {
  if (auto env = env_var("PYCHRON_EARTHBANK_TEMPLATES"); env && !env->empty()) return *env;
  if (!program_dir.empty()) {
    // As cmake/PychronInstall.cmake lays an install out: share/pychron beside
    // bin, or the Resources of a macOS bundle.
    for (const fs::path& base : {program_dir / ".." / "share" / "pychron", program_dir / ".." / "Resources"}) {
      const fs::path dir = base / "earthbank" / "templates";
      std::error_code ec;
      if (!fs::is_directory(dir, ec)) continue;
      const fs::path canonical = fs::weakly_canonical(dir, ec);
      return ec ? dir : canonical;
    }
  }
  return fs::path(PYCHRON_EARTHBANK_SOURCE_TEMPLATE_DIR);
}

Result<std::vector<XlsxColumn>> xlsx_columns(const fs::path& template_dir) {
  const fs::path dir = template_dir.empty() ? default_template_dir() : template_dir;
  const std::pair<std::string_view, std::span<const SheetSpec>> templates[] = {{kArArTemplate, kArArSheets},
                                                                              {kSampleTemplate, kSampleSheets}};
  std::vector<XlsxColumn> out;
  for (const auto& [name, sheets] : templates) {
    const fs::path file = dir / name;
    const auto book = open_template(file);
    if (!book) return fail(book.error());
    const auto plan = plan_of(*book, sheets, file);
    if (!plan) return fail(plan.error());
    for (const SheetPlan& sheet : plan->sheets) {
      for (const Column& column : sheet.columns) {
        out.push_back({std::string(sheet.spec.name), column.key, std::string(name_of(column.record)), column.field});
      }
    }
  }
  return out;
}

Result<XlsxResult> export_xlsx(const std::vector<Submission>& submissions, const fs::path& out_dir,
                               const XlsxOptions& options) {
  if (auto checked = check_names(submissions); !checked) return fail(checked.error());
  const fs::path dir = options.template_dir.empty() ? default_template_dir() : options.template_dir;
  const std::string stamp = stamp_of(options.now);

  std::vector<Records> records;
  for (const Submission& s : submissions) records.push_back(records_of(s));

  // Every workbook is made in memory before any is written: an error so far
  // leaves nothing on disk.
  Notes notes;
  std::vector<Pending> pending;
  {
    const fs::path file = dir / kArArTemplate;
    const auto book = open_template(file);
    if (!book) return fail(book.error());
    const auto plan = plan_of(*book, kArArSheets, file);
    if (!plan) return fail(plan.error());
    if (options.per_sample) {
      for (const Records& r : records) {
        const Records* const one[] = {&r};
        // Named for its sample; a sample without a name, for its data point.
        const std::string& sample = r.submission->sample.name.value_or(std::string());
        const std::string& named = sample.empty() ? r.submission->datapoint_name : sample;
        pending.push_back({*book, "ArArDataPoint_" + safe_name(named)});
        if (auto filled = fill(pending.back().book, *plan, one, true, notes); !filled) return fail(filled.error());
      }
    } else {
      std::vector<const Records*> all;
      for (const Records& r : records) all.push_back(&r);
      pending.push_back({*book, "ArArDataPoint_" + stamp});
      if (auto filled = fill(pending.back().book, *plan, all, true, notes); !filled) return fail(filled.error());
    }
  }
  if (options.samples) {
    const auto samples = samples_of(records);
    if (!samples) return fail(samples.error());
    const fs::path file = dir / kSampleTemplate;
    auto book = open_template(file);
    if (!book) return fail(book.error());
    const auto plan = plan_of(*book, kSampleSheets, file);
    if (!plan) return fail(plan.error());
    pending.push_back({std::move(*book), "Samples_" + stamp});
    if (auto filled = fill(pending.back().book, *plan, *samples, false, notes); !filled) return fail(filled.error());
  }

  std::error_code ec;
  const bool made = fs::create_directories(out_dir, ec);
  if (ec || !fs::is_directory(out_dir, ec)) {
    return fail(ErrorKind::Io, "earthbank: cannot make the directory " + out_dir.string() +
                                   (ec ? ": " + ec.message() : std::string(": it is not a directory")));
  }

  // Workbook::save writes beside the file and renames, and marks the file as
  // the user's. A failure part of the way takes back what was written.
  XlsxResult result;
  FileNames names(out_dir);
  for (const Pending& one : pending) {
    const fs::path file = names.next(one.stem);
    if (auto saved = one.book.save(file); !saved) {
      for (const fs::path& written : result.files) fs::remove(written, ec);
      if (made) fs::remove(out_dir, ec);
      return failed(saved.error());
    }
    result.files.push_back(file);
  }
  result.notes = notes.take();
  return result;
}

}  // namespace pychron::earthbank
