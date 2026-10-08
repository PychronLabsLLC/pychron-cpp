#include "pychron/processing/flux_fit.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <span>
#include <string_view>

namespace pychron::processing {

reduction::Measured MonitorSet::lambda_k() const {
  return {lambda_ec.value + lambda_b.value, std::hypot(lambda_ec.error, lambda_b.error)};
}

reduction::MonitorConstants MonitorSet::constants() const { return {age_ma * 1e6, lambda_k().value}; }

namespace {

using reduction::ModelKind;

struct ModelName {
  ModelKind kind;
  std::string_view legacy;
  std::string_view cli;
};

constexpr ModelName kModelNames[] = {
    {ModelKind::Plane, "Plane", "plane"},
    {ModelKind::Bowl, "Bowl", "bowl"},
    {ModelKind::WeightedMean, "Weighted Mean", "weighted-mean"},
    {ModelKind::Matching, "Matching", "matching"},
    {ModelKind::NearestNeighbors, "Nearest Neighbors", "nearest"},
    {ModelKind::Bracketing, "Bracketing", "bracketing"},
    {ModelKind::LeastSquares1D, "LeastSquares1D", "ls1d"},
    {ModelKind::WeightedMean1D, "WeightedMean1D", "mean1d"},
    {ModelKind::Bracketing1D, "Bracketing1D", "bracketing1d"},
};

bool equal_nocase(std::string_view a, std::string_view b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                    [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

// Legacy EXCLUDE_TAGS: an analysis with one of these starts omitted.
bool tag_omits(std::string_view tag) {
  for (std::string_view t : {"omit", "invalid", "outlier", "skip"})
    if (equal_nocase(tag, t)) return true;
  return false;
}

// Why a rule leaves an analysis out: Used when none applies (or the analysis is in
// `edits.include`), else edit, then saved fit, then tag.
AnalysisState analysis_omission(const LevelPosition& p, const LevelAnalysis& a, const Edits& edits) {
  if (edits.include.contains(a.record_id)) return AnalysisState::Used;
  if (edits.omit.contains(a.record_id)) return AnalysisState::OmittedByEdit;
  if (p.saved && !edits.reset_omits && p.saved->omitted.contains(a.record_id)) return AnalysisState::OmittedBySavedFit;
  if (tag_omits(a.tag)) return AnalysisState::OmittedByTag;
  return AnalysisState::Used;
}

std::string level_name(const LevelInputs& in) { return in.irradiation + in.level; }

}  // namespace

std::string_view legacy_model_name(reduction::ModelKind kind) noexcept {
  for (const auto& m : kModelNames)
    if (m.kind == kind) return m.legacy;
  return {};
}

std::string_view to_string(AnalysisState s) noexcept {
  switch (s) {
    case AnalysisState::Used: return "used";
    case AnalysisState::OmittedByTag: return "omitted by tag";
    case AnalysisState::OmittedBySavedFit: return "omitted by saved fit";
    case AnalysisState::OmittedByEdit: return "omitted here";
    case AnalysisState::NotReduced: return "not reduced";
    case AnalysisState::NoJ: return "no J";
  }
  return "used";
}

std::optional<reduction::ModelKind> parse_model_kind(std::string_view text) noexcept {
  for (const auto& m : kModelNames)
    if (equal_nocase(text, m.legacy) || equal_nocase(text, m.cli)) return m.kind;
  return std::nullopt;
}

FittedPosition evaluate_position(const LevelPosition& p, const MonitorSet& monitor_set, const FluxOptions& options,
                                 const Edits& edits) {
  const auto constants = monitor_set.constants();
  FittedPosition fp;
  fp.hole = p.hole;
  fp.position_uuid = p.position_uuid;
  fp.identifier = p.identifier;
  fp.sample = p.sample;
  fp.x = p.x;
  fp.y = p.y;
  fp.monitor = p.monitor;
  if (p.saved) {
    fp.saved_j = p.saved->j;
    fp.saved_j_err = p.saved->j_err;
    fp.saved_revision = p.saved->revision;
  }
  if (!p.monitor) return fp;
  const bool saved_applies = p.saved && !edits.reset_omits;
  std::vector<reduction::MonitorAnalysis> analyses;
  bool any_unreduced = false, any_usable = false;
  for (const auto& a : p.analyses) {
    const AnalysisState omission = analysis_omission(p, a, edits);
    const bool omitted = omission != AnalysisState::Used;
    // Omitted is by rule only: an analysis that did not reduce takes no
    // part, but a save must not carry it forward as an omission.
    FittedPosition::UsedAnalysis ua;
    ua.uuid = a.uuid;
    ua.record_id = a.record_id;
    ua.tag = a.tag;
    ua.omitted = omitted;
    ua.state = omission;
    if (a.f) {
      if (auto j = reduction::j_of(*a.f, constants)) {
        ua.j = j->nominal();
        ua.j_err = j->std_dev();
      } else if (!omitted) {
        ua.state = AnalysisState::NoJ;
      }
    } else if (!omitted) {
      ua.state = AnalysisState::NotReduced;
      ua.reduction_error = a.reduction_error;
    }
    fp.analyses.push_back(std::move(ua));
    if (!a.f) {
      any_unreduced = true;
      continue;
    }
    analyses.push_back({a.record_id, *a.f, omitted});
    if (!omitted) any_usable = true;
  }
  if (any_unreduced) fp.notes.push_back(PositionNote::AnalysisNotReduced);

  // The user's exclusion, now or carried from the saved fit. A revision
  // saved before `excluded` existed says it by having a mean J and still
  // not being used; `used_in_fit` false alone is also what a monitor with
  // no analyses yet was saved with.
  const auto saved_excluded = [](const SavedFlux& s) {
    if (s.excluded) return *s.excluded;
    return s.used_in_fit == std::optional<bool>(false) && s.mean_j.has_value();
  };
  const bool carried = saved_applies && saved_excluded(*p.saved);
  fp.excluded = !edits.include_positions.contains(p.hole) && (edits.exclude_positions.contains(p.hole) || carried);
  bool left_out = fp.excluded;
  std::optional<reduction::PositionMean> mean;
  if (any_usable) {
    auto m = reduction::mean_j(analyses, constants, options.mean, options.mean_error);
    if (m) mean = std::move(*m);
    else  // every analysis left gave no J: the position takes no part
      for (const auto& a : analyses)
        if (!a.omitted) fp.rejected.push_back(a.record_id);
  }
  if (!mean) {
    fp.notes.push_back(PositionNote::NoUsableAnalysis);
    left_out = true;
  } else {
    fp.n = mean->n;
    fp.mean_j = mean->j;
    fp.mean_j_err = mean->j_err;
    fp.mean_j_mswd = mean->mswd;
    fp.rejected = mean->rejected;
    if (!fp.rejected.empty()) fp.notes.push_back(PositionNote::AnalysisRejected);
    if (mean->n > 1 && !mean->mswd_acceptable) fp.notes.push_back(PositionNote::MeanMswdOutsideLimits);
    if (!left_out) {
      fp.used_in_fit = true;
    }
  }
  if (left_out && mean) fp.notes.push_back(PositionNote::LeftOutOfFit);
  // A rejected analysis gives no usable J: either its F gives none (j
  // stays absent) or the weighted mean refuses a J with no error (j is
  // kept, so a plot can still draw it).
  for (auto& ua : fp.analyses)
    if (!ua.omitted && std::find(fp.rejected.begin(), fp.rejected.end(), ua.record_id) != fp.rejected.end()) {
      ua.state = AnalysisState::NoJ;
    }
  return fp;
}

Result<LevelFit> fit_level(const LevelInputs& in, const FluxOptions& options, const Edits& edits) {
  const std::string where = level_name(in);

  std::set<std::string> records;
  std::string holes, record_list;
  std::set<int> level_holes;
  bool any_monitor = false;
  for (const auto& p : in.positions) {
    level_holes.insert(p.hole);
    holes += (holes.empty() ? "" : ", ") + std::to_string(p.hole);
    if (!p.monitor) continue;
    any_monitor = true;
    for (const auto& a : p.analyses) {
      records.insert(a.record_id);
      record_list += (record_list.empty() ? "" : ", ") + a.record_id;
    }
  }
  if (!any_monitor) return fail(ErrorKind::Config, "flux: " + where + " has no monitor positions");

  for (const auto* ids : {&edits.omit, &edits.include})
    for (const auto& id : *ids)
      if (!records.contains(id))
        return fail(ErrorKind::Config, "flux: " + id + " is not an analysis of the monitors of " + where +
                                           " (analyses: " + record_list + ")");
  for (const auto* chosen : {&edits.exclude_positions, &edits.include_positions})
    for (int hole : *chosen)
      if (!level_holes.contains(hole))
        return fail(ErrorKind::Config, "flux: hole " + std::to_string(hole) + " is not a position of " + where +
                                           " (holes: " + holes + ")");

  LevelFit out;
  out.irradiation = in.irradiation;
  out.level = in.level;
  out.holder = in.holder;
  out.monitor_set = in.monitor_set;
  out.all_positions = in.all_positions;
  out.options = options;

  std::vector<reduction::Monitor> used;
  std::vector<reduction::Point> points;

  for (const auto& p : in.positions) {
    FittedPosition fp = evaluate_position(p, in.monitor_set, options, edits);
    points.push_back({p.x, p.y});
    if (fp.used_in_fit) used.push_back({std::to_string(p.hole), {p.x, p.y}, *fp.mean_j, *fp.mean_j_err});
    out.positions.push_back(std::move(fp));
  }

  auto fit = reduction::fit_flux(used, points, options.fit);
  if (!fit) return fail(fit.error());

  out.parameters = fit->parameters;
  out.mswd = fit->mswd;
  out.dof = fit->dof;
  for (std::size_t i = 0; i < out.positions.size(); ++i) {
    auto& fp = out.positions[i];
    fp.j = fit->at[i].j;
    fp.j_err = fit->at[i].j_err;
    if (fp.saved_j && fp.j != 0.0) fp.dev_percent = (*fp.saved_j - fp.j) / fp.j * 100.0;
  }
  for (const auto& n : fit->notes) {
    if (n.note == reduction::FitNote::MswdOutsideLimits) out.mswd_outside_limits = true;
    else if (n.note == reduction::FitNote::Extrapolated && n.point < out.positions.size()) out.positions[n.point].notes.push_back(PositionNote::Extrapolated);
  }

  out.min_j = std::numeric_limits<double>::infinity();
  out.max_j = -std::numeric_limits<double>::infinity();
  for (const auto& fp : out.positions) {
    out.min_j = std::min(out.min_j, fp.j);
    out.max_j = std::max(out.max_j, fp.j);
  }
  out.delta_j_percent = out.max_j != 0.0 ? (out.max_j - out.min_j) / out.max_j * 100.0 : 0.0;
  return out;
}

}  // namespace pychron::processing
