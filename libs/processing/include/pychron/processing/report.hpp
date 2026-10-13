#pragma once

// A publication data report after Schaen et al. (2021), "Interpreting and
// reporting 40Ar/39Ar geochronologic data" (GSA Bulletin 133, 461-487): the
// sample and irradiation metadata, the constants the ages used, one row per
// analysis with its corrected intensities, blanks, ratios and ages, and the
// summary ages of every group (integrated, plateau, weighted mean, inverse
// isochron), built from a dataset in one call and written as one sectioned
// CSV file or as JSON.
//
// Everything comes from the dataset: its grouping gives the summary rows, its
// exclusions decide what the statistics use, and its reduction tag and
// constants are what reduce() used. Nothing is interpreted for the reader:
// the plateau, the weighted mean and the isochron are all reported, with
// their MSWD, n and probability, and the publication chooses.
//
// Uncertainties: every "±" column is at `ReportOptions::nsigma` (the
// standard recommends two). Ages carry two: analytical (the measured
// intensities, blanks, baselines and detector factors) and with J (the
// position's J uncertainty added). The decay-constant uncertainty is included
// when the reduction was asked to; the monitor age uncertainty is never
// propagated, and the metadata says so.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/reduction/stats.hpp"

namespace pychron::processing {

struct ReportOptions {
  int nsigma = 2;                  // level of every "±" column
  std::string laboratory;          // who measured; empty: no row
  std::string software;            // what reduced; empty: "pychron-cpp"
  std::vector<std::string> notes;  // free text, one metadata row each
  // Summary statistics, as the spectrum and isochron figures take them.
  reduction::PlateauCriteria plateau;
  reduction::PlateauWeighting plateau_weighting = reduction::PlateauWeighting::InverseVariance;
  reduction::MeanErrorKind mean_error = reduction::MeanErrorKind::Msem;  // plateau and weighted mean
  reduction::YorkMethod york = reduction::YorkMethod::NewYork;
  bool integrated_includes_excluded = true;
};

// A cell: nothing (the source has no value), a number, or text.
using ReportCell = std::variant<std::monostate, double, std::string>;

struct ReportTable {
  std::string name;  // section name: metadata, constants, irradiation, analyses, summary
  std::string title;
  std::vector<std::string> columns;
  std::vector<std::vector<ReportCell>> rows;  // every row has columns.size() cells
};

struct Report {
  std::vector<std::string> header;  // comment lines: the standard, the generator, the date
  ReportTable metadata;     // item, value
  ReportTable constants;    // the decay constants and atmospheric ratios the ages used
  ReportTable irradiation;  // one row per identifier: sample, location, irradiation, J, monitor, production ratios
  ReportTable analyses;     // one row per analysis, in group and step order
  ReportTable summary;      // one row per group
  std::vector<std::string> warnings;  // what could not be reported, and why

  const ReportTable* table(std::string_view name) const;
};

Report make_report(const Dataset& dataset, const ReportOptions& options = {});

// One RFC 4180 field: the text as it is, or quoted (a quote doubled) when it
// holds a comma, a quote or a line break, or starts or ends with a space,
// which some readers trim. Shared by every CSV the application writes.
std::string csv_quote(std::string_view text);

// "# " header lines, then every table as "[name]", its column line and its
// rows (RFC 4180 fields), separated by blank lines.
std::string report_csv(const Report& report);
// {"header": [...], "warnings": [...], "metadata": {item: value},
//  "constants" | "irradiation" | "analyses" | "summary": [{column: cell}]}
std::string report_json(const Report& report);
// JSON when the path ends in .json (any case), CSV otherwise. Error (Io) when
// the file cannot be written.
Result<void> save_report(const Report& report, const std::filesystem::path& path);

}  // namespace pychron::processing
