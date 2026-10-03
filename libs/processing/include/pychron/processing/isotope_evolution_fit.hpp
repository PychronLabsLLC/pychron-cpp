#pragma once

// Batch isotope-evolution refits (data browsing and visualization design,
// section 7.5, V3 fit units), after legacy FitIsotopeEvolutionNode: one fit
// per isotope applied to the raw signals of every included analysis, with
// legacy's goodness checks, then saved as intercepts revisions in one
// changeset.
//
// Options: list "isotopes", one row per fit:
//   series      signal (isotope: a key "Ar40" or "H1:Ar40", or a name
//               matching every key of it) or baseline (isotope names a
//               detector; the refit applies to every isotope on it)
//   fit, error, filter_outliers, iterations, std_devs
//   goodness, each optional (legacy IsoFilterFitAuxPlot):
//     max_percent_error         flag when |error / value| x 100 exceeds it
//     smart_filter "a,b,c,d"    flag when error >= a v^b + c v + d
//     max_outliers              flag when the filter removed more points
//     max_slope, slope_intensity  flag a slope at t = 0 above max_slope,
//                               only for values above slope_intensity
//     max_curvature, curvature_at  flag |y''| / (1 + y'^2)^1.5 of the raw
//                               points (numpy.gradient) at an index, or at
//                               that fraction of the points when in (0, 1)
//     min_rsquared              flag an adjusted R^2 at or below it (not
//                               for averages)
//     signal_to_baseline, signal_to_baseline_percent  signals: when the
//                               baseline error is more than the first % of
//                               the value, flag an error of the second % or more
//     max_signal_to_blank       signals: flag blank / value x 100 at or above it
// keep_user_excluded (refit without the points each analysis already
// leaves out; default on), skip_reviewed (leave values marked reviewed
// alone).

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/processing/fit_edit.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/scene.hpp"

namespace pychron::processing {

// One failed goodness check of one refitted isotope.
struct GoodnessFlag {
  std::string key;     // isotope key, or detector for baselines
  // "percent_error", "smart_filter", "outliers", "slope", "curvature",
  // "rsquared", "signal_to_baseline", "signal_to_blank"
  std::string check;
  double value = 0.0;  // the measured quantity
  double threshold = 0.0;
};

struct IsotopeRefit {
  EditedFit fit;             // kind Signal (key: isotope) or Baseline (key: detector)
  Value stored;              // the value before the refit
  double slope = 0.0;        // of the fitted curve at t = 0, fA/s
  std::size_t outliers = 0;  // points the filter removed
  std::optional<double> rsquared_adj;  // absent for averages
  double curvature = 0.0;    // at the row's curvature_at
  std::string label() const;  // "Ar40", "H1 baseline"
};

struct AnalysisRefits {
  std::string uuid, runid;
  std::map<std::string, std::string> heads;  // Analysis::heads when refitted
  std::vector<IsotopeRefit> isotopes;
  std::vector<GoodnessFlag> flags;
  AnalysisPtr edited;  // the analysis with the refitted intercepts (previews)
  bool good() const noexcept { return flags.empty(); }
  std::vector<EditedFit> fits() const;
};

struct IsotopeFitSet {
  std::vector<AnalysisRefits> analyses;  // included analyses with at least one refit
  std::vector<std::string> warnings;     // isotopes that could not be refitted
  int reviewed_kept = 0;
  // "<ISOEVO> refit Ar40(linear),H1 baseline(average)"
  std::string message() const;
  int flagged() const;
};

using IsotopeFitSetPtr = std::shared_ptr<const IsotopeFitSet>;

const SchemaPtr& isotope_evolution_fit_schema();

using RawLoader = std::function<Result<RawData>(const std::string& uuid)>;

struct IsotopeEvolutionFigure {
  Scene scene;  // per isotope: refitted (and stored) intercepts against run time
  IsotopeFitSet fits;
};

// Legacy curvature: |y''| / (1 + y'^2)^1.5 with numpy.gradient (unit
// spacing) at `at` (an index, or a fraction of the points in (0, 1)).
double curvature_at(const std::vector<double>& ys, double at);
// 1 - (1 - R^2)(n - 1)/(n - p) of `fit` over the points it used; nullopt
// for averages or when n <= p.
std::optional<double> adjusted_rsquared(const RawSeries& series, const SeriesFit& fit,
                                        const std::vector<std::size_t>& user_excluded);

// Refits every included analysis of `analyses`; `load_raw` reads its raw
// series. Fails only when cancelled or nothing is configured; per-analysis
// problems (no raw data, a failed fit) become warnings.
Result<IsotopeEvolutionFigure> build_isotope_evolution_fits(const Dataset& analyses, const Options& options,
                                                            const RawLoader& load_raw,
                                                            const std::atomic<bool>* cancel = nullptr);

class Unit;
// isotope_evolution_fit: (analyses) -> (figure, fits); reads the source's raw data.
std::unique_ptr<Unit> make_isotope_evolution_fit_unit();

}  // namespace pychron::processing
