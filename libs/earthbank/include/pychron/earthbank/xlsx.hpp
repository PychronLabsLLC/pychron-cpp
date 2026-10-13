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
//     `lat`, its `locationComment` the location's `description`), or that
//     nothing fills it (`Tag`);
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
// Lists. A workbook is imported as it is written: no service resolves a name
// on the way. The template carries the service's lists (`Lookup Tables`,
// `Lookup tables`) and ties a column to its list by a data validation, so a
// value going into such a column is checked against that list first:
//   - one the list has is written as it is;
//   - one that is an entry but for the case of its letters or the spaces
//     around it is written as the list spells it, and what became what is
//     said, each value once, with the list's heading on the lookup sheet
//     (`Samples.material: 'sanidine' written as 'Sanidine' (list:
//     Lithology)`);
//   - a flux monitor, whose entries are whole citations, is also found by
//     its name and age, or its name and paper (vocab.hpp
//     match_flux_monitor), and the same note ends `, found by its name and
//     age` or `, found by its name and paper`, whichever it was;
//   - anything else is written as it was given and said
//     (`ArArAgeCalc.airRatio: 'Mark et al. 2011' is not in the template's
//     list 'Air Ratio'; the importer may refuse the value`), each value
//     once. Nothing is rewritten on a guess: a value that could be two
//     entries is left alone and said too.
// A column without a list (the data point's `mineral` and `lithology`, which
// the template calls search terms) is written as given. The list is the
// cells the validation names and those that follow them in the lookup sheet
// without a gap.
//
// Required columns. The templates colour the title of a column (row 3) by
// its obligation, with a legend in the sheet of definitions. A column in the
// colour of the legend's `Required` that a written row leaves empty is named
// in the notes, once per sheet for the whole export, with its title and in
// how many rows (`ArArDataPoints: marked Required by the template's
// colouring; the importer may refuse a row that leaves one empty: arArSetUp
// (Analytical Key) empty in every row, irradiationBatchID (Irradiation Batch
// ID) empty in 1 of 2 rows`). The colour is per column; a few columns are
// wanted only of some rows, and are not reported for the others: a row with
// a furnace temperature is not missed its laser power and units, nor one
// with a laser power its furnace temperature, and the step number and the
// two shares of the 39Ar are wanted only of a data point whose method is a
// step heating. Their counts are of the rows they are wanted in, and say so
// (`empty in 2 of the 10 rows of a step-heating data point`, `empty in 1 of
// the 2 rows that have no laser power`).
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
  // For the user, each once and in the order it came up: fields without a
  // column, rows past the formatted ones, values that are not in a column's
  // list, or were written in its spelling or found in it by name; then,
  // sheet by sheet, the Required columns a row left empty.
  std::vector<std::string> notes;
};

// Writes the workbooks into `out_dir` (made if it is not there):
//   ArArDataPoint_<yyyymmdd-hhmmss>.xlsx   every submission, or
//   ArArDataPoint_<sample>.xlsx            one per submission (per_sample),
//   Samples_<yyyymmdd-hhmmss>.xlsx         the samples.
// A sample's name is made safe for a file name: ASCII letters, digits, `-`
// and `_` stay, anything else becomes `_`. A name that keeps no letter or
// digit (one wholly outside ASCII) gives way to the data point's, made safe
// the same way, and that failing too to `sample-<n>`, n the submission's
// place from 1. A name already taken, by another sample or by a file in the
// directory, gets `-2`, `-3`, ...: nothing is ever overwritten. Each file is
// marked as the user's (core user_file.hpp).
//
// All the files are written or none: on an error those already written are
// removed, and one that could not be is named in the error. Errors, each
// "earthbank: ...": Config for no submission, a submission without a data
// point name or whose sample has no name, two with the same data point name,
// two different samples of one name, text a workbook cannot hold; Io or
// Protocol, naming the file, for a template that is missing or is not the
// workbook expected, and for a file that cannot be written; Io, naming every
// place looked in, when no template directory is given and none is found.
Result<XlsxResult> export_xlsx(const std::vector<Submission>& submissions, const std::filesystem::path& out_dir,
                               const XlsxOptions& options = {});

// What an export of `submissions` with `options` would say, without writing
// anything: the notes export_xlsx would return (values that are not in the
// template's lists, Required columns left empty, fields without a column) or
// the error it would refuse with. For showing before the export.
Result<std::vector<std::string>> check_against_template(const std::vector<Submission>& submissions,
                                                        const XlsxOptions& options = {});

// Where the templates are, the first of: the directory the environment
// variable PYCHRON_EARTHBANK_TEMPLATES names (taken at its word); the
// installed ones beside the running program
// (`../share/pychron/earthbank/templates`, or in a macOS bundle
// `../Resources/earthbank/templates`, from the program's directory); the
// source tree's, when that directory is on this machine, which is what a
// program that is not installed finds. Empty when none of them is there: an
// export then fails saying where it looked.
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
