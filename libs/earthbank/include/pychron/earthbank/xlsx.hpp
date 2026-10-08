// Submissions (model.hpp) written into EarthBank's own workbooks: the two
// import templates the service hands out, `ArArDataPoint_Template2026.xlsx`
// and `Sample_Template_v2025-04-16.xlsx`, each copied and filled in. The
// files are what a user imports on the service's website; nothing here talks
// to the service.
//
// A workbook is never built from nothing. The template is copied, and in each
// data sheet the rows from 5 down are filled; row 4 of the sheet names the
// field each column takes. Everything else in the copy (the four header rows,
// the sheets of definitions and lookup lists, the two `Props` sheets, the
// styles) is the template's, byte for byte.
//
// Which value goes under which column:
//
//   - a column takes the field of its sheet's record that has the column's
//     name, or that name with `Name` added (the column `mineral` takes
//     `mineralName`: a controlled term is written by name, never by id);
//   - where the template names a column otherwise, a table in xlsx.cpp says
//     which field it takes (`arArMethod` takes `arMethodName`, the Samples
//     sheet's `sampleName` the sample's `name`, its `latitude` the location's
//     `lat`), or that nothing fills it (`Tag`);
//   - `datapointName`, the first column of every Ar-Ar sheet, takes
//     Submission::datapoint_name on every row: it is what ties a data point's
//     measurements, aliquots, age summary and age calculation to it;
//   - `arArSetUp` and `irradiationBatchID` of the data point's sheet take
//     Submission::setup_name and irradiation_name: the terms the website
//     finds the lab's set-up and the irradiation batch by.
//
// xlsx_columns gives the whole of it for a pair of templates.
//
// Rows. `ArArDataPoints`, `ArArAgeSummary` and `ArArAgeCalc` get one row per
// submission, `ArArMeasurement` and `ArArAliquot` one per measurement and per
// aliquot in the submission's order, `Samples` one per sample; submissions
// follow one another in the order given.
//
// Values are written as the records hold them, with three exceptions the
// sheets ask for: a measurement's time as `YYYY-MM-DD hh:mm UTC` (to the
// minute; the record's seconds are cut), a collection date as `dd.mm.yyyy`,
// and a flag as `Yes` or `No`. Numbers are numbers, the rest text.
//
// Nothing is dropped without a word. A field that is set and has no column
// (the template lacks one, or a newer template dropped it) is named in
// XlsxResult::notes, once. Three kinds of field are passed over in silence:
// an `id` or `*Id` (the service's row numbers, for an upload); a field that
// says again what a column already holds (a measurement's `analysisUnits`
// when it is the data point's), noted only when the two differ; and the
// measurements' `jvalueUncertaintyTypeName`, which the template asks for once
// per data point: the column takes the value the measurements share, and is
// left empty, with a note, when they do not share one.
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/model.hpp"

namespace pychron::earthbank {

struct XlsxOptions {
  std::filesystem::path template_dir;  // holds the two templates; empty: default_template_dir()
  bool per_sample = false;             // one Ar-Ar workbook per submission, not one for all
  bool samples = true;                 // the Sample workbook as well (always one, for all the samples)
  // Stamps the file names (UTC); nothing else reads it.
  std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
};

struct XlsxResult {
  std::vector<std::filesystem::path> files;  // the Ar-Ar workbooks in the submissions' order, then the Sample one
  std::vector<std::string> notes;            // each once: fields without a column, rows past the formatted ones
};

// Writes the workbooks into `out_dir` (made if it is not there):
//   ArArDataPoint_<yyyymmdd-hhmmss>.xlsx   every submission, or
//   ArArDataPoint_<sample>.xlsx            one per submission (per_sample),
//   Samples_<yyyymmdd-hhmmss>.xlsx         the samples.
// A sample's name is made safe for a file name: ASCII letters, digits, `-`
// and `_` stay, anything else becomes `_`. A name already taken, by another
// sample or by a file in the directory, gets `-2`, `-3`, ...: nothing is ever
// overwritten. Each file is marked as the user's (core user_file.hpp).
//
// All the files are written or none: on an error none is left behind. Errors,
// each "earthbank: ...": Config for no submission, a submission without a
// data point name, two with the same one, two different samples of one name,
// text a workbook cannot hold; Io or Protocol, naming the file, for a
// template that is missing or is not the workbook expected, and for a file
// that cannot be written.
Result<XlsxResult> export_xlsx(const std::vector<Submission>& submissions, const std::filesystem::path& out_dir,
                               const XlsxOptions& options = {});

// Where the templates are, the first of: the directory the environment
// variable PYCHRON_EARTHBANK_TEMPLATES names; the installed ones beside the
// running program (`../share/pychron/earthbank/templates`, or in a macOS
// bundle `../Resources/earthbank/templates`, from the program's directory);
// the source tree's, which is what a program that is not installed finds.
std::filesystem::path default_template_dir();
std::filesystem::path default_template_dir(const std::filesystem::path& program_dir);

// One column of a data sheet and where its value comes from. `record` is
// `Sample`, `Location`, `DataPoint`, `Aliquot`, `Measurement`, `AgeCalc` or
// `AgeSummary` and `field` the field's service name; or `Submission` and
// `datapoint_name`, `setup_name` or `irradiation_name`; or both are empty:
// nothing fills the column.
struct XlsxColumn {
  std::string sheet;
  std::string key;  // the name in row 4
  std::string record;
  std::string field;
  bool operator==(const XlsxColumn&) const = default;
};

// Every named column of every data sheet of the two templates in
// `template_dir` (empty: default_template_dir()), sheet by sheet from column
// A: what export_xlsx does with these templates, for a guide or a check.
Result<std::vector<XlsxColumn>> xlsx_columns(const std::filesystem::path& template_dir = {});

}  // namespace pychron::earthbank
