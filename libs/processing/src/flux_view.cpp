#include "pychron/processing/flux_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>

#include "pychron/processing/report.hpp"
#include "schema_builder.hpp"

namespace pychron::processing {

namespace r = pychron::reduction;

namespace {

constexpr double kPi = 3.14159265358979323846;

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
        case AnalysisState::NotReduced: why = a.reduction_error.empty() ? "not reduced" : "not reduced: " + a.reduction_error; break;
        case AnalysisState::NoJ: why = "no J"; break;
      }
      out.push_back(hole(p) + ": " + a.record_id + " " + why);
    }
  if (inputs.saved_options && inputs.saved_sd_replaced && !context.fit_error_given &&
      r::is_least_squares(fit.options.fit.kind))
    out.emplace_back("saved fit used SD, which a fitted surface does not have: using msem");
  // The standard is not changed silently; with a monitor set named the user chose it.
  if (inputs.saved_monitor_set_missing && !context.monitor_set_given)
    out.push_back("saved fit used monitor set '" + inputs.saved_monitor_set +
                  "', which the store does not have: using '" + inputs.monitor_set.name + "'");
  return out;
}

std::string csv_field(std::string_view text) { return csv_quote(text); }

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

namespace {

std::string_view axis_name(r::Axis a) { return a == r::Axis::Y ? "y" : "x"; }

}  // namespace

SchemaPtr flux_options_schema() {
  using namespace detail;
  static const SchemaPtr schema = [] {
    std::vector<std::string> models, interpolations, errors = {"sem", "msem", "sd"};
    for (const auto k : {r::ModelKind::Plane, r::ModelKind::Bowl, r::ModelKind::WeightedMean, r::ModelKind::Matching,
                         r::ModelKind::NearestNeighbors, r::ModelKind::Bracketing, r::ModelKind::LeastSquares1D,
                         r::ModelKind::WeightedMean1D, r::ModelKind::Bracketing1D})
      models.emplace_back(model_name(k));
    for (const auto i : {r::Interpolation::WeightedMean, r::Interpolation::Average, r::Interpolation::Linear})
      interpolations.emplace_back(interpolation_name(i));
    const auto help = [](FieldSpec f, std::string text) {
      f.help = std::move(text);
      return f;
    };
    auto s = std::const_pointer_cast<Schema>(make_schema(
        "flux", "Flux",
        {help(choice("model.kind", "Model", "Model", models, "plane"),
              "How the monitors' mean J becomes a J at every position."),
         help(when(boolean("model.weighted", "Weighted fit", "Model", false), "model.kind in plane|bowl|ls1d"),
              "Weight the fit by 1 / j_err^2."),
         help(when(integer("model.neighbors", "Neighbours", "Model", 2, 1, 64), "model.kind == nearest"),
              "How many of the nearest monitors each position takes the inverse-variance mean of."),
         help(when(choice("model.interpolation", "Interpolation", "Model", interpolations, "weighted"),
                   "model.kind == bracketing"),
              "How a position between its two nearest monitors is formed: weighted, average or linear."),
         help(when(choice("model.axis", "Axis", "Model", {"x", "y"}, "x"), "model.kind in ls1d|mean1d|bracketing1d"),
              "The axis the one-dimensional models run along."),
         help(when(integer("model.degree", "Degree", "Model", 1, 1, 4), "model.kind == ls1d"),
              "The degree of the polynomial along the axis, 1 to 4."),
         help(choice("mean.kind", "Mean", "Errors", {"arithmetic", "weighted"}, "arithmetic"),
              "How the analyses of one monitor position are averaged (J, not F)."),
         help(choice("mean.error", "Error of the mean", "Errors", errors, "msem"),
              "The error of that mean; msem is the SEM scaled by sqrt(MSWD) when the MSWD is above 1."),
         help(when(choice("fit.error", "Error of the fit", "Errors", errors, "msem"),
                   "model.kind in plane|bowl|weighted-mean|ls1d|mean1d"),
              "The error of the predicted J: sem or msem of a fitted surface, sem, msem or sd of a mean model.")}));
    s->version = 1;
    s->factory_presets = {{"Default", ""}, {"Weighted plane", "[model]\nweighted = true\n"}};
    return SchemaPtr(s);
  }();
  return schema;
}

Options to_options(const FluxOptions& o) {
  Options out(flux_options_schema());
  // Every value is one of its field's own choices or in range, so set cannot refuse.
  (void)out.set("model.kind", std::string(model_name(o.fit.kind)));
  (void)out.set("model.weighted", o.fit.weighted);
  (void)out.set("model.neighbors", std::int64_t{o.fit.n_neighbors});
  (void)out.set("model.interpolation", std::string(interpolation_name(o.fit.interpolation)));
  (void)out.set("model.axis", std::string(axis_name(o.fit.axis)));
  (void)out.set("model.degree", std::int64_t{o.fit.degree});
  (void)out.set("mean.kind", std::string(r::to_string(o.mean)));
  (void)out.set("mean.error", std::string(r::to_string(o.mean_error)));
  (void)out.set("fit.error", std::string(r::to_string(o.fit.error)));
  return out;
}

Result<FluxOptions> flux_options_from(const Options& options) {
  FluxOptions o;
  const auto kind = parse_model_kind(options.get_string("model.kind"));
  const auto mean = r::parse_mean_kind(options.get_string("mean.kind"));
  const auto mean_error = r::parse_mean_error_kind(options.get_string("mean.error"));
  const auto fit_error = r::parse_mean_error_kind(options.get_string("fit.error"));
  if (!kind) return fail(ErrorKind::Config, "flux: unknown model '" + options.get_string("model.kind") + "'");
  if (!mean) return fail(ErrorKind::Config, "flux: unknown mean '" + options.get_string("mean.kind") + "'");
  if (!mean_error || !fit_error) return fail(ErrorKind::Config, "flux: unknown error kind");
  o.fit.kind = *kind;
  o.fit.weighted = options.get_bool("model.weighted");
  o.fit.n_neighbors = static_cast<int>(options.get_int("model.neighbors"));
  const std::string interpolation = options.get_string("model.interpolation");
  for (const auto i : {r::Interpolation::WeightedMean, r::Interpolation::Average, r::Interpolation::Linear})
    if (interpolation_name(i) == interpolation) o.fit.interpolation = i;
  o.fit.axis = options.get_string("model.axis") == "y" ? r::Axis::Y : r::Axis::X;
  o.fit.degree = static_cast<int>(options.get_int("model.degree"));
  o.mean = *mean;
  o.mean_error = *mean_error;
  o.fit.error = *fit_error;
  if (r::is_least_squares(o.fit.kind) && o.fit.error == r::MeanErrorKind::Sd)
    return fail(ErrorKind::Config, "flux: sd is not an error kind of a fitted surface");
  return o;
}

// ---- The scene --------------------------------------------------------------

FluxAbscissa flux_abscissa(const FluxOptions& options) {
  switch (options.fit.kind) {
    case r::ModelKind::LeastSquares1D:
    case r::ModelKind::WeightedMean1D:
    case r::ModelKind::Bracketing1D: return options.fit.axis == r::Axis::Y ? FluxAbscissa::Y : FluxAbscissa::X;
    default: return FluxAbscissa::Angle;
  }
}

double flux_hole_abscissa(FluxAbscissa kind, double x, double y) {
  switch (kind) {
    case FluxAbscissa::X: return x;
    case FluxAbscissa::Y: return y;
    case FluxAbscissa::Angle: {
      const double a = std::atan2(x, y) * 180.0 / kPi;
      return a <= -180.0 ? 180.0 : a;  // (-180, 180]: atan2(-0, y < 0) is -180
    }
  }
  return x;
}

namespace {

constexpr int kCurvePoints = 361;

std::string analysis_tooltip(const FittedPosition::UsedAnalysis& a) {
  std::string t = a.record_id + "\nJ " + flux_j_text(*a.j) + " \xC2\xB1 " + flux_j_text(a.j_err.value_or(0.0));
  // A J is drawn only for a state with one; NoJ with a J is the weighted mean refusing it.
  switch (a.state) {
    case AnalysisState::Used: break;
    case AnalysisState::OmittedByTag: t += "\nomitted (tag " + a.tag + ")"; break;
    case AnalysisState::OmittedBySavedFit: t += "\nomitted (saved fit)"; break;
    case AnalysisState::OmittedByEdit: t += "\nomitted (here)"; break;
    case AnalysisState::NotReduced: t += "\nnot reduced"; break;
    case AnalysisState::NoJ: t += "\nnot used: J has no error"; break;
  }
  return t;
}

std::string hole_head(const FittedPosition& p) { return "hole " + std::to_string(p.hole) + " \xC2\xB7 " + p.identifier; }

MarkerStyle marker(MarkerShape shape, double size, const Color& color) {
  MarkerStyle m;
  m.shape = shape;
  m.size = size;
  m.color = color;
  return m;
}

PointLayer layer_of(std::string label, MarkerStyle style) {
  PointLayer l;
  l.label = std::move(label);
  l.marker = style;
  l.excluded_marker = style;
  l.excluded_marker.filled = false;
  return l;
}

bool has_curve(r::ModelKind kind) {
  switch (kind) {
    case r::ModelKind::Plane:
    case r::ModelKind::Bowl:
    case r::ModelKind::WeightedMean:
    case r::ModelKind::LeastSquares1D:
    case r::ModelKind::WeightedMean1D: return true;
    default: return false;
  }
}

// The fit's band and line, from the monitors used; nothing when the model has
// no curve or cannot be evaluated along it.
void add_curve(Panel& panel, const LevelFit& fit, FluxAbscissa kind, double lo, double hi) {
  if (!has_curve(fit.options.fit.kind)) return;
  std::vector<r::Monitor> monitors;
  double radius = 0;
  for (const auto& p : fit.positions) {
    if (!p.monitor || !p.used_in_fit || !p.mean_j || !p.mean_j_err) continue;
    monitors.push_back({std::to_string(p.hole), {p.x, p.y}, *p.mean_j, *p.mean_j_err});
    radius += std::hypot(p.x, p.y);
  }
  if (monitors.empty()) return;
  if (kind != FluxAbscissa::Angle && !(hi > lo)) return;  // every position at one coordinate: no curve
  radius /= static_cast<double>(monitors.size());

  std::vector<double> xs;
  std::vector<r::Point> at;
  for (int i = 0; i < kCurvePoints; ++i) {
    const double t = static_cast<double>(i) / (kCurvePoints - 1);
    if (kind == FluxAbscissa::Angle) {
      const double a = -180.0 + 360.0 * t;
      const double rad = a * kPi / 180.0;
      xs.push_back(a);
      at.push_back({radius * std::sin(rad), radius * std::cos(rad)});
    } else {
      const double c = lo + (hi - lo) * t;
      xs.push_back(c);
      at.push_back(kind == FluxAbscissa::X ? r::Point{c, 0.0} : r::Point{0.0, c});
    }
  }
  const auto curve = r::fit_flux(monitors, at, fit.options.fit);
  if (!curve) return;

  const Color base = palette_color(0);
  BandLayer band;
  band.fill = {base.r, base.g, base.b, 48};
  band.x = xs;
  LineLayer line;
  line.label = "Fit";
  line.style.color = base;
  line.style.width = 2.0;
  line.x = xs;
  for (const auto& v : curve->at) {
    band.low.push_back(v.j - v.j_err);
    band.high.push_back(v.j + v.j_err);
    line.y.push_back(v.j);
  }
  panel.layers.emplace_back(std::move(band));
  panel.layers.emplace_back(std::move(line));
}

ScenePtr build_flux_scene(const std::vector<FittedPosition>& positions, const LevelFit* fit,
                          const FluxOptions& options, const FluxSceneOptions& so) {
  const FluxAbscissa kind = flux_abscissa(options);
  double lo = 0, hi = 0;
  bool first = true;
  for (const auto& p : positions) {
    const double c = flux_hole_abscissa(kind == FluxAbscissa::Angle ? FluxAbscissa::X : kind, p.x, p.y);
    lo = first ? c : std::min(lo, c);
    hi = first ? c : std::max(hi, c);
    first = false;
  }

  Scene scene;
  scene.kind = "flux";
  Graph graph;
  graph.x.title = kind == FluxAbscissa::Angle ? "Hole angle (degrees)" : kind == FluxAbscissa::X ? "X" : "Y";
  Panel panel;
  panel.id = "p0";
  panel.quantity = "J";
  panel.y.title = "J";

  if (fit) add_curve(panel, *fit, kind, lo, hi);

  const Color c_analyses = palette_color(1), c_means = palette_color(2), c_unknowns = palette_color(3),
              c_highlight = palette_color(4);
  PointLayer analyses = layer_of("Analyses", marker(MarkerShape::Circle, 4, c_analyses));
  PointLayer means = layer_of("Monitor means", marker(MarkerShape::Diamond, 8, c_means));
  PointLayer unknowns = layer_of("Unknowns", marker(MarkerShape::Square, 6, c_unknowns));
  PointLayer h_analyses = layer_of("", marker(MarkerShape::Circle, 9, c_highlight));
  PointLayer h_mean = layer_of("", marker(MarkerShape::Diamond, 13, c_highlight));
  PointLayer h_unknown = layer_of("", marker(MarkerShape::Square, 11, c_highlight));

  // `y_err` absent: the layer has no error bars (the analyses').
  const auto push_point = [](PointLayer& l, double x, double y, std::optional<double> y_err, PointRef ref,
                             bool excluded, std::string tip) {
    l.x.push_back(x);
    l.y.push_back(y);
    if (y_err) l.y_err.push_back(*y_err);
    l.refs.push_back(std::move(ref));
    l.excluded.push_back(excluded);
    l.tooltips.push_back(std::move(tip));
  };

  for (const auto& p : positions) {
    const double at = flux_hole_abscissa(kind, p.x, p.y);
    const bool highlighted = so.highlight_hole && *so.highlight_hole == p.hole;
    if (!p.monitor) {
      if (!fit) continue;
      std::string tip = hole_head(p) + "\npredicted J " + flux_j_text(p.j) + " \xC2\xB1 " + flux_j_text(p.j_err);
      if (p.dev_percent) tip += "\ndev " + flux_pct_text(*p.dev_percent) + " %";
      push_point(unknowns, at, p.j, p.j_err, {}, false, tip);
      if (highlighted) push_point(h_unknown, at, p.j, p.j_err, {}, false, tip);
      continue;
    }

    // Every analysis with a J, about the hole in record-id order.
    std::vector<const FittedPosition::UsedAnalysis*> drawn;
    for (const auto& a : p.analyses)
      if (a.j) drawn.push_back(&a);
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order): ordered by record id, not by address
    std::sort(drawn.begin(), drawn.end(), [](auto* a, auto* b) { return a->record_id < b->record_id; });
    const auto n = static_cast<double>(drawn.size());
    const double step = (kind == FluxAbscissa::Angle ? 4.0 : (hi - lo) * 0.04) / std::max(n - 1.0, 1.0);
    for (std::size_t i = 0; i < drawn.size(); ++i) {
      const auto& a = *drawn[i];
      const double x = at + (static_cast<double>(i) - (n - 1.0) / 2.0) * step;
      const std::string tip = analysis_tooltip(a);
      const bool out = a.state != AnalysisState::Used;
      push_point(analyses, x, *a.j, std::nullopt, PointRef{a.uuid}, out, tip);
      if (highlighted) push_point(h_analyses, x, *a.j, std::nullopt, PointRef{a.uuid}, out, tip);
    }

    if (!p.mean_j) continue;
    const double err = p.mean_j_err.value_or(0.0);
    const std::string tip = hole_head(p) + "\nn " + std::to_string(p.n) + " \xC2\xB7 mean J " + flux_j_text(*p.mean_j) +
                            " \xC2\xB1 " + flux_j_text(err) + "\nMSWD " + flux_pct_text(p.mean_j_mswd);
    push_point(means, at, *p.mean_j, err, {}, !p.used_in_fit, tip);
    if (highlighted) push_point(h_mean, at, *p.mean_j, err, {}, !p.used_in_fit, tip);
  }

  panel.layers.emplace_back(std::move(analyses));
  panel.layers.emplace_back(std::move(means));
  if (fit) panel.layers.emplace_back(std::move(unknowns));
  for (auto* l : {&h_analyses, &h_mean, &h_unknown})
    if (!l->x.empty()) panel.layers.emplace_back(std::move(*l));

  graph.panels.push_back(std::move(panel));
  scene.graphs.push_back(std::move(graph));
  return std::make_shared<const Scene>(std::move(scene));
}

}  // namespace

ScenePtr flux_scene(const LevelInputs& /*inputs*/, const LevelFit& fit, const FluxSceneOptions& options) {
  return build_flux_scene(fit.positions, &fit, fit.options, options);
}

ScenePtr flux_scene(const LevelInputs& inputs, const FluxOptions& options, const Edits& edits,
                    const FluxSceneOptions& scene_options) {
  std::vector<FittedPosition> positions;
  positions.reserve(inputs.positions.size());
  for (const auto& p : inputs.positions) positions.push_back(evaluate_position(p, inputs.monitor_set, options, edits));
  return build_flux_scene(positions, nullptr, options, scene_options);
}

}  // namespace pychron::processing
