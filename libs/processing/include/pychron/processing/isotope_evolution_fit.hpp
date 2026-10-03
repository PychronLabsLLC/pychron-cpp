#pragma once

// Batch isotope-evolution refits (data browsing and visualization design,
// section 7.5, V3 fit units), after legacy FitIsotopeEvolutionNode: one fit
// per isotope applied to the raw signals of every included analysis, with
// legacy's goodness checks, then saved as intercepts revisions in one
// changeset.
//
// Options: list "isotopes" (isotope: a key "Ar40" or "H1:Ar40", or an
// isotope name matching every key of it; fit; error; outlier filter,
// iterations, std devs; goodness thresholds: max_percent_error,
// max_outliers, max_slope, each optional), keep_user_excluded (refit
// without the points each analysis already leaves out; default on),
// skip_reviewed (leave intercepts marked reviewed alone).

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
  std::string key;     // isotope key
  std::string check;   // "percent_error", "outliers", "slope"
  double value = 0.0;  // the measured quantity
  double threshold = 0.0;
};

struct IsotopeRefit {
  EditedFit fit;
  Value stored;            // the intercept before the refit
  double slope = 0.0;      // of the fitted curve at t = 0, fA/s
  std::size_t outliers = 0;  // points the filter removed
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
  // "<ISOEVO> refit Ar40(linear),Ar36(parabolic)"
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
