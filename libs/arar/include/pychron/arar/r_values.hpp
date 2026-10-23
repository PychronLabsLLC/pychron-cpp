#pragma once

// R-values of an ideogram (spec 2026-10-21-ideogram-r-values-design.md; legacy
// pipeline/plot/editors/rvalues_table.py): per graph, the ratio of every
// group's mean F (40Ar*/39ArK) to every other group's, with the error of the
// two means propagated as independent.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/reduction/stats.hpp"

namespace pychron::processing {

struct GroupF {
  int group = 0;
  std::string name;                   // Dataset::group_name
  std::optional<reduction::Mean> f;   // absent: no included analysis gives a mean of F
  std::string error;                  // why f is absent
};

struct RCell {
  double r = 0;
  double r_err = 0;  // 1 sigma
};

struct RValueMatrix {
  int graph = 0;
  std::string graph_name;
  std::vector<GroupF> groups;  // order of groups_of_graph
  // cells[i][j] = F_j / F_i; empty on the diagonal and in the row and column
  // of a group without f, or where F_i == 0.
  std::vector<std::vector<std::optional<RCell>>> cells;
};

// Over every graph of `dataset`, with the mean and error kind read from
// `ideogram_options` (schema "figure.ideogram"). F is always the quantity, of
// included analyses only, whatever the ideogram plots. A group without F is a
// GroupF::error, not a failure.
Result<std::vector<RValueMatrix>> r_value_matrices(const Dataset& dataset, const Options& ideogram_options);

// "1.234568 ± 0.002000": value and nsigma times the 1-sigma error, six
// decimals each; the text of a cell and of a group's F as the table shows them.
std::string r_value_text(double value, double error_1s, int nsigma);

// The tables as shown, tab-separated, for the clipboard: "# " and the note,
// then per graph with two groups or more its name, a header row and one row
// per group (cells at nsigma, an empty cell empty); a blank line between graphs.
std::string r_values_tsv(const std::vector<RValueMatrix>& matrices, int nsigma);

// "# " and the note, the column line, then one row per ordered pair with a
// cell (numerator column j, denominator row i). Errors 1 sigma, numbers in
// their shortest exact form, text quoted by csv_quote (report.hpp).
std::string r_values_csv(const std::vector<RValueMatrix>& matrices, const Options& ideogram_options);

inline constexpr std::string_view kRValueNote =
    "Uncertainties treat groups as independent; correlated errors (shared irradiation "
    "corrections, blanks, ...) are not accounted for.";

}  // namespace pychron::processing
