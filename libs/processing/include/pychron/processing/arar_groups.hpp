#pragma once

// Group-level Ar/Ar values for the ideogram, spectrum and isochron figures
// (data browsing and visualization design, section 5; V2): integrated
// (total-gas) ages, plateau steps, inverse-isochron points with exact
// correlations, and the isochron age.
//
// Ported from legacy pychron:
//   processing/analyses/analysis_group.py  _calculate_integrated_age, _apply_external_err
//   processing/argon_calculations.py       calculate_isochron, extract_isochron_xy
// Deviations are marked "Deviation:".

#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/reduction/stats.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::processing {

using GroupItems = std::vector<const DatasetItem*>;

// Mean error with the relative error of J (and, when `decay`, of lambda_K)
// added in quadrature, as legacy _apply_external_err. `j_relative` is
// sigma(J)/J of the group's first analysis.
double with_external_error(double value, double error, double j_relative);
// sigma(J)/J of the first analysis with a J; 0 when none.
double j_relative_error(const GroupItems& items);

// Total-gas age: F = sum(rad40) / sum(k39) over `items` (legacy weighting
// "None - Iso. Recombination"), then the age equation with the first
// analysis's J and constants. With `include_j_error` false J enters as its
// nominal value (legacy include_j_error_in_integrated defaults to false).
// Error (Config) when no item reduced, sum(k39) is 0, or the age is undefined.
Result<reduction::UFloat> integrated_age(const GroupItems& items, bool include_j_error);

struct IsochronPoint {
  const DatasetItem* item = nullptr;
  double x = 0, sx = 0;  // 39Ar/40Ar
  double y = 0, sy = 0;  // 36Ar/40Ar
  double rho = 0;        // correlation of x and y
};

// One point per item with interference-corrected Ar40, Ar39 and Ar36 (legacy
// extract_isochron_xy) and Ar40 != 0. Deviation: rho is the exact correlation
// of the two UFloat ratios (shared Ar40 and any other shared variable), not
// legacy's relative-error formula, which equals it only when Ar40, Ar39 and
// Ar36 are independent.
std::vector<IsochronPoint> isochron_points(const GroupItems& items);

struct IsochronAge {
  reduction::YorkFit fit;    // y = 36/40 on x = 39/40
  reduction::YorkFit fit_x;  // x on y: its intercept is 39/40 at 36/40 = 0
  Value trapped;             // (40/36)trapped = 1 / fit.intercept
  std::optional<reduction::UFloat> f;    // 40Ar*/39ArK = 1 / fit_x.intercept
  std::optional<reduction::UFloat> age;  // absent without J or with F <= 0
};

// Legacy calculate_isochron: two York fits, F = 1/x-intercept from the fit of
// x on y, age with the first analysis's J (with its error when
// `include_j_error`). `mse` scales errors by sqrt(MSWD) when MSWD > 1 (legacy
// "MSE"). Error (Config) with fewer than 3 points or a failed fit.
Result<IsochronAge> isochron_age(const std::vector<IsochronPoint>& points, reduction::YorkMethod method, bool mse,
                                 bool include_j_error);

}  // namespace pychron::processing
