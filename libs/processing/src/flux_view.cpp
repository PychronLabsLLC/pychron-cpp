#include "pychron/processing/flux_view.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace pychron::processing {

namespace r = pychron::reduction;

namespace {

std::string fixed(double v, const char* format) {
  if (!std::isfinite(v)) return "-";
  char buf[64];
  std::snprintf(buf, sizeof buf, format, v);
  return buf;
}

std::string_view model_name(r::ModelKind kind) {
  switch (kind) {
    case r::ModelKind::Plane: return "plane";
    case r::ModelKind::Bowl: return "bowl";
    case r::ModelKind::WeightedMean: return "weighted-mean";
    case r::ModelKind::Matching: return "matching";
    case r::ModelKind::NearestNeighbors: return "nearest";
    case r::ModelKind::Bracketing: return "bracketing";
    case r::ModelKind::LeastSquares1D: return "ls1d";
    case r::ModelKind::WeightedMean1D: return "mean1d";
    case r::ModelKind::Bracketing1D: return "bracketing1d";
  }
  return "plane";
}

std::string_view interpolation_name(r::Interpolation i) {
  switch (i) {
    case r::Interpolation::WeightedMean: return "weighted";
    case r::Interpolation::Average: return "average";
    case r::Interpolation::Linear: return "linear";
  }
  return "weighted";
}

std::string notes_text(const FittedPosition& p) {
  std::string text;
  const auto add = [&](const std::string& s) { text += (text.empty() ? "" : "; ") + s; };
  for (const auto& id : p.rejected) add("rejected " + id);
  for (const auto note : p.notes) {
    switch (note) {
      case PositionNote::Extrapolated: add("extrapolated"); break;
      case PositionNote::MeanMswdOutsideLimits: add("mean MSWD outside limits"); break;
      case PositionNote::NoUsableAnalysis: add("no usable analysis"); break;
      case PositionNote::LeftOutOfFit: add("left out of fit"); break;
      case PositionNote::AnalysisNotReduced: add("analysis not reduced"); break;
      case PositionNote::AnalysisRejected: break;
    }
  }
  return text;
}

// Seventeen significant digits: a double reads back as it was.
std::string number17(double v) {
  if (!std::isfinite(v)) return "";
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}
std::string number17(const std::optional<double>& v) { return v ? number17(*v) : ""; }

}  // namespace

std::string flux_j_text(double v) { return fixed(v, "%.4e"); }
std::string flux_j_text(const std::optional<double>& v) { return v ? flux_j_text(*v) : "-"; }
std::string flux_pct_text(double v) { return fixed(v, "%.2f"); }
std::string flux_pct_text(const std::optional<double>& v) { return v ? flux_pct_text(*v) : "-"; }
std::string flux_percent_of(double err, double value) {
  return value != 0.0 ? flux_pct_text(err / value * 100.0) : "-";
}
std::string flux_percent_of(const std::optional<double>& err, const std::optional<double>& value) {
  return err && value ? flux_percent_of(*err, *value) : "-";
}

std::string flux_model_line(const FluxOptions& o) {
  std::ostringstream s;
  s << model_name(o.fit.kind);
  switch (o.fit.kind) {
    case r::ModelKind::Plane:
    case r::ModelKind::Bowl: s << (o.fit.weighted ? ", weighted" : ", unweighted"); break;
    case r::ModelKind::NearestNeighbors: s << ", " << o.fit.n_neighbors << " neighbors"; break;
    case r::ModelKind::Bracketing: s << ", " << interpolation_name(o.fit.interpolation); break;
    case r::ModelKind::LeastSquares1D:
      s << ", " << (o.fit.weighted ? "weighted" : "unweighted") << ", degree " << o.fit.degree << ", axis "
        << (o.fit.axis == r::Axis::X ? 'x' : 'y');
      break;
    case r::ModelKind::WeightedMean1D:
    case r::ModelKind::Bracketing1D:  // always linear: no interpolation to name
      s << ", axis " << (o.fit.axis == r::Axis::X ? 'x' : 'y');
      break;
    case r::ModelKind::WeightedMean:
    case r::ModelKind::Matching: break;
  }
  s << "; mean " << r::to_string(o.mean) << " (" << r::to_string(o.mean_error) << "); fit error "
    << r::to_string(o.fit.error);
  return s.str();
}

std::string flux_summary(const LevelFit& fit) {
  return "fit MSWD " + flux_pct_text(fit.mswd) + " (" + std::to_string(fit.dof) + " dof)   J min " +
         flux_j_text(fit.min_j) + "  max " + flux_j_text(fit.max_j) + "  delta " + flux_pct_text(fit.delta_j_percent) +
         " %";
}

std::string flux_status_line(const LevelFit& fit) {
  constexpr const char* kSeparator = " \xC2\xB7 ";  // U+00B7
  const std::string model = flux_model_line(fit.options);
  std::string text = model.substr(0, model.find(';'));
  if (fit.dof != 0 || fit.mswd != 0.0)
    text += kSeparator + std::string("fit MSWD ") + flux_pct_text(fit.mswd) + " (" + std::to_string(fit.dof) + " dof)";
  text += kSeparator + std::string("J ") + flux_j_text(fit.min_j) + " \xE2\x80\x93 " + flux_j_text(fit.max_j) + " (" +
          flux_pct_text(fit.delta_j_percent) + " %)";
  return text;
}

std::vector<std::string> flux_warnings(const LevelInputs& inputs, const LevelFit& fit, const FluxWarningContext& context) {
  std::vector<std::string> out;
  const auto hole = [](const FittedPosition& p) { return "hole " + std::to_string(p.hole); };
  for (const auto& p : fit.positions) {
    for (const auto note : p.notes) {
      switch (note) {
        case PositionNote::AnalysisNotReduced: break;  // named below, by record id
        case PositionNote::NoUsableAnalysis: out.push_back(hole(p) + " has no usable analysis"); break;
        case PositionNote::LeftOutOfFit: out.push_back(hole(p) + " left out of the fit"); break;
        case PositionNote::MeanMswdOutsideLimits:
          out.push_back(hole(p) + ": mean MSWD " + flux_pct_text(p.mean_j_mswd) + " is outside its limits");
          break;
        case PositionNote::Extrapolated: out.push_back(hole(p) + " is extrapolated (outside the monitors)"); break;
        case PositionNote::AnalysisRejected: break;  // named below, by record id
      }
    }
  }
  if (fit.mswd_outside_limits) out.push_back("fit MSWD " + flux_pct_text(fit.mswd) + " is outside its limits");
  for (const auto& p : fit.positions)
    for (const auto& a : p.analyses) {
      std::string why;
      switch (a.state) {
        case AnalysisState::Used: continue;
        case AnalysisState::OmittedByTag: why = "omitted (tag " + a.tag + ")"; break;
        case AnalysisState::OmittedBySavedFit: why = "omitted (saved fit)"; break;
        case AnalysisState::OmittedByEdit: why = "omitted (here)"; break;
        case AnalysisState::NotReduced: why = "not reduced: " + a.reduction_error; break;
        case AnalysisState::NoJ: why = "no J"; break;
      }
      out.push_back(hole(p) + ": " + a.record_id + " " + why);
    }
  if (inputs.saved_options && inputs.saved_sd_replaced && !context.fit_error_given &&
      r::is_least_squares(fit.options.fit.kind))
    out.push_back("saved fit used SD, which a fitted surface does not have: using msem");
  // The standard is not changed silently; with a monitor set named the user chose it.
  if (inputs.saved_monitor_set_missing && !context.monitor_set_given)
    out.push_back("saved fit used monitor set '" + inputs.saved_monitor_set +
                  "', which the store does not have: using '" + inputs.monitor_set.name + "'");
  return out;
}

std::string csv_field(std::string_view text) {
  if (text.find_first_of(",\"\r\n") == std::string_view::npos) return std::string(text);
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + '"';
}

std::string flux_csv_header() {
  return "kind,irradiation,level,hole,identifier,sample,x,y,n,saved_j,saved_j_err,mean_j,mean_j_err,mean_j_mswd,j,j_err,"
         "dev_percent,used_in_fit,notes\r\n";
}

std::string flux_csv_rows(const LevelFit& fit) {
  std::string out;
  for (const auto& p : fit.positions) {
    const std::vector<std::string> cells = {p.monitor ? "monitor" : "unknown",
                                            fit.irradiation,
                                            fit.level,
                                            std::to_string(p.hole),
                                            p.identifier,
                                            p.sample,
                                            number17(p.x),
                                            number17(p.y),
                                            p.monitor ? std::to_string(p.n) : "",
                                            number17(p.saved_j),
                                            number17(p.saved_j_err),
                                            number17(p.mean_j),
                                            number17(p.mean_j_err),
                                            number17(p.mean_j_mswd),
                                            number17(p.j),
                                            number17(p.j_err),
                                            number17(p.dev_percent),
                                            p.monitor ? (p.used_in_fit ? "yes" : "no") : "",
                                            notes_text(p)};
    for (std::size_t i = 0; i < cells.size(); ++i) out += (i ? "," : "") + csv_field(cells[i]);
    out += "\r\n";
  }
  return out;
}

}  // namespace pychron::processing
