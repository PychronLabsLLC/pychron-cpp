#pragma once

// Flux fitting (flux fitting design): the monitor standard a lab irradiates
// beside its samples. Qt-free and JSON-free; the store adapter reads and
// writes the document.

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/reduction/arar_types.hpp"
#include "pychron/reduction/flux.hpp"
#include "pychron/reduction/stats.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::processing {

// One monitor standard: its age and the decay constants it is quoted with.
struct MonitorSet {
  std::string name, sample, material;
  double age_ma = 0, age_err_ma = 0;
  reduction::Measured lambda_ec, lambda_b;  // 1/a, 1 sigma
  reduction::Measured lambda_k() const;     // sum, errors in quadrature
  reduction::MonitorConstants constants() const;  // {age_ma * 1e6, lambda_k().value}

  friend bool operator==(const MonitorSet& a, const MonitorSet& b) {
    return a.name == b.name && a.sample == b.sample && a.material == b.material && a.age_ma == b.age_ma &&
           a.age_err_ma == b.age_err_ma && a.lambda_ec.value == b.lambda_ec.value &&
           a.lambda_ec.error == b.lambda_ec.error && a.lambda_b.value == b.lambda_b.value &&
           a.lambda_b.error == b.lambda_b.error;
  }
};

// ---- Fitting a level (design section 6.2) -----------------------------------

// What the user picks: the model and how a position's mean J is formed.
struct FluxOptions {
  reduction::FitOptions fit;
  reduction::MeanKind mean = reduction::MeanKind::Arithmetic;
  reduction::MeanErrorKind mean_error = reduction::MeanErrorKind::Msem;
  friend bool operator==(const FluxOptions&, const FluxOptions&) = default;
};

// The legacy model strings ("Plane", "Nearest Neighbors", ...). parse_model_kind
// reads those (any case) and the `elctl flux` spellings: plane, bowl,
// weighted-mean, matching, nearest, bracketing, ls1d, mean1d, bracketing1d.
std::string_view legacy_model_name(reduction::ModelKind kind) noexcept;
std::optional<reduction::ModelKind> parse_model_kind(std::string_view text) noexcept;

struct LevelAnalysis {
  std::string uuid, record_id, tag;
  std::optional<reduction::UFloat> f;  // nullopt: the reduction failed
  std::string reduction_error;
};

// The head flux_position revision of a position, as read.
struct SavedFlux {
  std::string revision;  // uuid text; the compare-and-swap expectation
  std::optional<double> j, j_err, mean_j, mean_j_err, mean_j_mswd;
  std::optional<FluxOptions> options;  // nullopt: none saved, or not one of the nine models
  std::optional<bool> used_in_fit;  // information: false too for a position nobody left out
  std::optional<bool> excluded;     // the user left the monitor out; nullopt: saved before the key existed
  std::string monitor_set;       // options' monitor_reference; may be empty
  std::set<std::string> omitted;  // record ids saved with is_omitted
  std::string saved_by, saved_utc;
};

struct LevelPosition {
  int hole = 0;
  std::string position_uuid, identifier, sample;
  double x = 0, y = 0;
  bool monitor = false;
  std::vector<LevelAnalysis> analyses;  // monitors only
  std::optional<SavedFlux> saved;
};

struct LevelInputs {
  std::string irradiation, level, holder;
  MonitorSet monitor_set;     // `sample` is the one the monitors were chosen by
  bool all_positions = false;  // the monitors are every position that has analyses, not the sample's
  std::vector<LevelPosition> positions;      // by hole
  std::optional<FluxOptions> saved_options;  // of the level's last fit (any monitor position's)
  std::string saved_monitor_set;             // the set that fit named; may be empty
  // The store has no set of that name (an imported "FC Min"): unless the
  // caller named one, `monitor_set` is the default in its place.
  bool saved_monitor_set_missing = false;
  bool saved_sd_replaced = false;            // the saved fit was least squares saved with SD (not read here)
};

struct Edits {
  std::set<std::string> omit, include;
  std::set<int> exclude_positions;
  bool reset_omits = false;  // ignore the omissions and exclusions of the saved fit
};

enum class PositionNote {
  Extrapolated,
  MeanMswdOutsideLimits,
  NoUsableAnalysis,
  LeftOutOfFit,
  AnalysisRejected,
  AnalysisNotReduced
};

// Why a monitor analysis takes part in the mean or does not. When more than
// one omission applies, the edit is named before the saved fit, and that
// before the tag. An analysis omitted by rule that also failed to reduce is
// named by the omission.
enum class AnalysisState { Used, OmittedByTag, OmittedBySavedFit, OmittedByEdit, NotReduced, NoJ };
// "used", "omitted by tag", "omitted by saved fit", "omitted here", "not reduced", "no J"
std::string_view to_string(AnalysisState) noexcept;

struct FittedPosition {
  int hole = 0;
  std::string position_uuid, identifier, sample;
  double x = 0, y = 0;
  bool monitor = false;
  int n = 0;  // analyses in the mean
  std::optional<double> saved_j, saved_j_err, mean_j, mean_j_err, mean_j_mswd;
  double j = 0, j_err = 0;  // predicted
  std::optional<double> dev_percent;  // (saved - predicted) / predicted * 100
  bool used_in_fit = false;
  // The user left this monitor out (Edits::exclude_positions, or the saved
  // fit's exclusion carried forward). A monitor that is out for want of a
  // usable analysis is not excluded.
  bool excluded = false;
  struct UsedAnalysis {
    std::string uuid, record_id, tag;  // tag as loaded
    bool omitted = false;  // by rule (tag, Edits::omit, the saved fit's); not "could not be used"
    AnalysisState state = AnalysisState::Used;
    // Absent for NotReduced and for NoJ because F gives no J. NoJ: no usable J,
    // either F gives none (j absent) or the weighted mean refuses a J with no
    // error (j kept, j_err 0). Present for an omitted analysis with an F.
    std::optional<double> j, j_err;
    std::string reduction_error;     // NotReduced only
  };
  std::vector<UsedAnalysis> analyses;
  std::vector<PositionNote> notes;
  std::vector<std::string> rejected;  // record ids
  std::optional<std::string> saved_revision;
};

struct LevelFit {
  std::string irradiation, level, holder;
  MonitorSet monitor_set;
  bool all_positions = false;  // LevelInputs::all_positions, saved with the fit
  FluxOptions options;
  std::vector<FittedPosition> positions;  // by hole
  std::vector<double> parameters;
  double mswd = 0;
  int dof = 0;
  bool mswd_outside_limits = false;
  double min_j = 0, max_j = 0, delta_j_percent = 0;  // (max - min) / max * 100 over predicted J
};

// Pure: the monitor and unknown tables of a level. Never reads
// `inputs.saved_options`; the caller resolves the options.
// What a saved fit carries forward (unless `edits.reset_omits`): the analyses
// it saved omitted, and a monitor it saved `excluded`. A revision saved
// before `excluded` existed is read as excluding a monitor when it says
// `used_in_fit` false and has a mean J (the monitor had analyses and still
// was not used). A position that was merely not used is not carried.
Result<LevelFit> fit_level(const LevelInputs& inputs, const FluxOptions& options, const Edits& edits);

}  // namespace pychron::processing
