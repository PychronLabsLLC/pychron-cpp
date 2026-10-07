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

std::string level_name(const LevelInputs& in) { return in.irradiation + in.level; }

}  // namespace

std::string_view legacy_model_name(reduction::ModelKind kind) noexcept {
  for (const auto& m : kModelNames)
    if (m.kind == kind) return m.legacy;
  return {};
}

std::optional<reduction::ModelKind> parse_model_kind(std::string_view text) noexcept {
  for (const auto& m : kModelNames)
    if (equal_nocase(text, m.legacy) || equal_nocase(text, m.cli)) return m.kind;
  return std::nullopt;
}

Result<LevelFit> fit_level(const LevelInputs& in, const FluxOptions& options, const Edits& edits) {
  const std::string where = level_name(in);

  std::set<std::string> records;
  std::string holes;
  std::set<int> monitor_holes;
  for (const auto& p : in.positions) {
    if (!p.monitor) continue;
    monitor_holes.insert(p.hole);
    holes += (holes.empty() ? "" : ", ") + std::to_string(p.hole);
    for (const auto& a : p.analyses) records.insert(a.record_id);
  }
  if (monitor_holes.empty()) return fail(ErrorKind::Config, "flux: " + where + " has no monitor positions");

  for (const auto* ids : {&edits.omit, &edits.include})
    for (const auto& id : *ids)
      if (!records.contains(id))
        return fail(ErrorKind::Config, "flux: " + id + " is not an analysis of the monitors of " + where);
  for (int hole : edits.exclude_positions)
    if (!monitor_holes.contains(hole))
      return fail(ErrorKind::Config,
                  "flux: hole " + std::to_string(hole) + " is not a monitor position of " + where + " (monitor holes: " +
                      holes + ")");

  const auto constants = in.monitor_set.constants();

  LevelFit out;
  out.irradiation = in.irradiation;
  out.level = in.level;
  out.holder = in.holder;
  out.monitor_set = in.monitor_set;
  out.options = options;

  std::vector<reduction::Monitor> used;
  std::vector<reduction::Point> points;
  std::vector<std::size_t> used_index;  // into out.positions

  for (const auto& p : in.positions) {
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
    points.push_back({p.x, p.y});

    if (p.monitor) {
      const bool saved_applies = p.saved && !edits.reset_omits;
      std::vector<reduction::MonitorAnalysis> analyses;
      bool any_unreduced = false, any_usable = false;
      for (const auto& a : p.analyses) {
        bool omitted = tag_omits(a.tag) || edits.omit.contains(a.record_id) ||
                       (saved_applies && p.saved->omitted.contains(a.record_id));
        if (edits.include.contains(a.record_id)) omitted = false;
        if (!a.f) {
          any_unreduced = true;
          fp.analyses.push_back({a.uuid, a.record_id, true});
          continue;
        }
        fp.analyses.push_back({a.uuid, a.record_id, omitted});
        analyses.push_back({a.record_id, *a.f, omitted});
        if (!omitted) any_usable = true;
      }
      if (any_unreduced) fp.notes.push_back(PositionNote::AnalysisNotReduced);

      bool left_out = edits.exclude_positions.contains(p.hole) ||
                      (saved_applies && p.saved->used_in_fit.has_value() && !*p.saved->used_in_fit);
      if (!any_usable) {
        fp.notes.push_back(PositionNote::NoUsableAnalysis);
        left_out = true;
      } else {
        auto mean = reduction::mean_j(analyses, constants, options.mean, options.mean_error);
        if (!mean) return fail(ErrorKind::Config, "flux: hole " + std::to_string(p.hole) + " of " + where + ": " +
                                                      mean.error().what);
        fp.n = mean->n;
        fp.mean_j = mean->j;
        fp.mean_j_err = mean->j_err;
        fp.mean_j_mswd = mean->mswd;
        fp.rejected = mean->rejected;
        if (!fp.rejected.empty()) fp.notes.push_back(PositionNote::AnalysisRejected);
        if (mean->n > 1 && !mean->mswd_acceptable) fp.notes.push_back(PositionNote::MeanMswdOutsideLimits);
        if (!left_out) {
          fp.used_in_fit = true;
          used.push_back({std::to_string(p.hole), {p.x, p.y}, mean->j, mean->j_err});
        }
      }
      if (left_out && any_usable) fp.notes.push_back(PositionNote::LeftOutOfFit);
    }
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
    else if (n.point < out.positions.size()) out.positions[n.point].notes.push_back(PositionNote::Extrapolated);
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
