#include "pychron/processing/isotope_evolution_fit.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>
#include <utility>

#include "pychron/processing/units.hpp"
#include "schema_builder.hpp"

namespace pychron::processing {

namespace r = pychron::reduction;
using namespace detail;

namespace {

struct RowSpec {
  std::string isotope;
  bool baseline = false;
  r::FitSpec fit;
  bool auto_n = false;
  std::int64_t n_threshold = 0;
  r::FitKind n_true = r::FitKind::Parabolic, n_false = r::FitKind::Linear;
  std::optional<double> max_percent_error, max_outliers, max_slope, slope_intensity, max_curvature, min_rsquared,
      signal_to_baseline, signal_to_baseline_percent, max_signal_to_blank;
  double curvature_at = 0.5;
  std::optional<std::array<double, 4>> smart_filter;
};

// "a,b,c,d" -> coefficients; nullopt when empty or malformed.
std::optional<std::array<double, 4>> parse_smart_filter(const std::string& text) {
  if (text.empty()) return std::nullopt;
  std::array<double, 4> c{};
  std::size_t i = 0, start = 0;
  while (i < 4) {
    const auto end = text.find(',', start);
    const std::string part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    char* stop = nullptr;
    c[i] = std::strtod(part.c_str(), &stop);
    if (stop == part.c_str()) return std::nullopt;
    ++i;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (i != 4) return std::nullopt;
  return c;
}

std::vector<RowSpec> row_specs(const Options& o) {
  std::vector<RowSpec> out;
  for (const auto& row : o.rows("isotopes")) {
    RowSpec s;
    s.isotope = row.get_string("isotope");
    s.baseline = row.get_string("series") == "baseline";
    s.auto_n = row.get_string("fit") == "auto_n";
    s.fit.kind = r::parse_fit_kind(row.get_string("fit")).value_or(r::FitKind::Linear);
    s.n_threshold = row.get_int("n_threshold");
    s.n_true = r::parse_fit_kind(row.get_string("n_true")).value_or(r::FitKind::Parabolic);
    s.n_false = r::parse_fit_kind(row.get_string("n_false")).value_or(r::FitKind::Linear);
    s.fit.error = row.get_string("error") == "SD" ? r::ErrorType::Sd : r::ErrorType::Sem;
    s.fit.outliers.enabled = row.get_bool("filter_outliers");
    s.fit.outliers.iterations = static_cast<int>(row.get_int("iterations"));
    s.fit.outliers.std_devs = row.get_double("std_devs");
    s.max_percent_error = row.get_optional_double("max_percent_error");
    s.max_outliers = row.get_optional_double("max_outliers");
    s.max_slope = row.get_optional_double("max_slope");
    s.slope_intensity = row.get_optional_double("slope_intensity");
    s.max_curvature = row.get_optional_double("max_curvature");
    s.curvature_at = row.get_double("curvature_at");
    s.min_rsquared = row.get_optional_double("min_rsquared");
    s.signal_to_baseline = row.get_optional_double("signal_to_baseline");
    s.signal_to_baseline_percent = row.get_optional_double("signal_to_baseline_percent");
    s.max_signal_to_blank = row.get_optional_double("max_signal_to_blank");
    s.smart_filter = parse_smart_filter(row.get_string("smart_filter"));
    out.push_back(std::move(s));
  }
  return out;
}

// Slope of the fitted curve at t = 0.
double slope_at_zero(const r::Intercept& f) {
  switch (f.kind) {
    case r::FitKind::Average: return 0.0;
    case r::FitKind::Exponential: return f.params.size() >= 2 ? -f.params[0] * f.params[1] : 0.0;
    default: return f.params.size() >= 2 ? f.params[1] : 0.0;
  }
}

SchemaPtr make_schema_impl() {
  auto row = make_schema(
      "figure.isotope_evolution_fit.isotope", "Fit",
      {choice("series", "Series", "Fit", {"signal", "baseline"}, "signal"),
       text("isotope", "Isotope (detector for baselines)", "Fit", "Ar40",
            "Signals: a key (Ar40, H1:Ar40) or an isotope name; baselines: a detector"),
       choice("fit", "Fit", "Fit", {"average", "linear", "parabolic", "cubic", "exponential", "auto_n"}, "linear"),
       when(integer("n_threshold", "Auto: points at least", "Fit", 30, 0, 100000), "fit == auto_n"),
       when(choice("n_true", "Auto: then", "Fit", {"average", "linear", "parabolic", "cubic", "exponential"},
                   "parabolic"),
            "fit == auto_n"),
       when(choice("n_false", "Auto: else", "Fit", {"average", "linear", "parabolic", "cubic", "exponential"},
                   "linear"),
            "fit == auto_n"),
       choice("error", "Error", "Fit", {"SEM", "SD"}, "SEM"),
       boolean("filter_outliers", "Filter outliers", "Fit", false),
       when(integer("iterations", "Iterations", "Fit", 1, 1, 10), "filter_outliers"),
       when(number("std_devs", "Std devs", "Fit", 2.0, 0.5, 10.0, 0.5), "filter_outliers"),
       optional_number("max_percent_error", "Flag error above (%)", "Goodness"),
       text("smart_filter", "Smart filter a,b,c,d", "Goodness", "",
            "Flag an error >= a v^b + c v + d; legacy default 0.0003,0.5,0.00005,0.015; empty: off"),
       optional_number("max_outliers", "Flag outliers above", "Goodness"),
       optional_number("max_slope", "Flag slope above (fA/s)", "Goodness",
                       "Legacy slope goodness: a growing signal is suspect"),
       optional_number("slope_intensity", "Slope check above (fA)", "Goodness",
                       "Check the slope only for values above this"),
       optional_number("max_curvature", "Flag curvature above", "Goodness"),
       [] {
         auto f = number("curvature_at", "Curvature at", "Goodness", 0.5, 0.0, 100000.0, 0.1);
         f.help = "A point index, or a fraction of the points when between 0 and 1";
         return f;
       }(),
       optional_number("min_rsquared", "Flag adjusted R² at or below", "Goodness"),
       optional_number("signal_to_baseline", "Baseline error above (% of signal)", "Goodness"),
       optional_number("signal_to_baseline_percent", "... then flag error from (%)", "Goodness"),
       optional_number("max_signal_to_blank", "Flag blank at or above (% of signal)", "Goodness")});
  ListSpec list;
  list.key = "isotopes";
  list.label = "Fits";
  list.section = "Isotopes";
  list.row = row;
  list.min_rows = 1;
  list.max_rows = 32;
  for (const char* iso : {"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"})
    list.default_rows_toml.push_back(std::string("isotope = \"") + iso + "\"");
  auto s = std::const_pointer_cast<Schema>(make_schema(
      "figure.isotope_evolution_fit", "Isotope evolutions",
      {boolean("keep_user_excluded", "Keep left-out points", "Fit", true,
               "Refit without the points each analysis already leaves out"),
       boolean("skip_reviewed", "Keep reviewed values", "Fit", false,
               "Intercepts and baselines already marked reviewed are not refitted or saved"),
       boolean("use_classifier", "Classify sniffs", "Classifier", false,
               "Flag isotopes the trained classifier calls bad (legacy IsotopeClassifier)"),
       when(text("classifier_file", "Training file", "Classifier", ""), "use_classifier"),
       when(text("classifier_stamp", "Training version", "Classifier", "",
                 "Changes when the training changes; set by the window"),
            "use_classifier"),
       integer("nsigma", "Error bars (σ)", "Display", 1, 1, 3),
       boolean("show_current", "Show current values", "Display", true)},
      {list}));
  s->factory_presets = {{"Default", ""}};
  return s;
}

}  // namespace

double curvature_at(const std::vector<double>& ys, double at) {
  const std::size_t n = ys.size();
  if (n < 2) return 0.0;
  auto gradient = [n](const std::vector<double>& y) {
    std::vector<double> g(n);
    g[0] = y[1] - y[0];
    g[n - 1] = y[n - 1] - y[n - 2];
    for (std::size_t i = 1; i + 1 < n; ++i) g[i] = (y[i + 1] - y[i - 1]) / 2.0;
    return g;
  };
  const auto d = gradient(ys);
  const auto dd = gradient(d);
  double x = at > 0.0 && at < 1.0 ? static_cast<double>(n) * at : at;
  const auto i = static_cast<std::size_t>(std::clamp(x, 0.0, static_cast<double>(n - 1)));
  return std::abs(dd[i] / std::pow(1.0 + d[i] * d[i], 1.5));
}

std::optional<double> adjusted_rsquared(const RawSeries& s, const SeriesFit& fit,
                                        const std::vector<std::size_t>& user_excluded) {
  if (fit.intercept.kind == r::FitKind::Average) return std::nullopt;
  std::vector<bool> drop(s.t.size(), false);
  for (auto i : user_excluded)
    if (i < drop.size()) drop[i] = true;
  for (auto i : fit.outliers)
    if (i < drop.size()) drop[i] = true;
  std::vector<double> x, y;
  for (std::size_t i = 0; i < s.t.size(); ++i)
    if (!drop[i]) {
      x.push_back(s.t[i]);
      y.push_back(s.v[i]);
    }
  const std::size_t n = y.size();
  r::FitSpec spec;
  spec.kind = fit.intercept.kind;
  spec.degree = static_cast<int>(fit.intercept.params.size()) - 1;
  const std::size_t p = r::parameter_count(spec);
  if (n <= p) return std::nullopt;
  double mean = 0.0;
  for (double v : y) mean += v;
  mean /= static_cast<double>(n);
  double ss_res = 0.0, ss_tot = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double e = y[i] - r::predict(fit.intercept, x[i]);
    ss_res += e * e;
    ss_tot += (y[i] - mean) * (y[i] - mean);
  }
  if (ss_tot <= 0.0) return std::nullopt;
  const double r2 = 1.0 - ss_res / ss_tot;
  return 1.0 - (1.0 - r2) * static_cast<double>(n - 1) / static_cast<double>(n - p);
}

std::string IsotopeRefit::label() const { return fit.kind == SeriesKind::Baseline ? fit.key + " baseline" : fit.key; }

std::vector<EditedFit> AnalysisRefits::fits() const {
  std::vector<EditedFit> out;
  for (const auto& i : isotopes) out.push_back(i.fit);
  return out;
}

std::string IsotopeFitSet::message() const {
  std::string out = "<ISOEVO> refit ";
  std::set<std::string> seen;
  bool first = true;
  for (const auto& a : analyses)
    for (const auto& i : a.isotopes) {
      if (!seen.insert(i.label()).second) continue;
      out += (first ? "" : ",") + i.label() + "(" + std::string(r::to_string(i.fit.fit.kind)) + ")";
      first = false;
    }
  return out;
}

int IsotopeFitSet::flagged() const {
  return static_cast<int>(std::count_if(analyses.begin(), analyses.end(), [](const AnalysisRefits& a) { return !a.good(); }));
}

const SchemaPtr& isotope_evolution_fit_schema() {
  static const SchemaPtr s = make_schema_impl();
  return s;
}

Result<IsotopeEvolutionFigure> build_isotope_evolution_fits(const Dataset& analyses, const Options& o,
                                                            const RawLoader& load_raw,
                                                            const std::atomic<bool>* cancel) {
  const auto specs = row_specs(o);
  if (specs.empty()) return fail(ErrorKind::Config, "no isotopes to fit");
  const bool keep_excluded = o.get_bool("keep_user_excluded");
  const bool skip_reviewed = o.get_bool("skip_reviewed");
  const auto nsigma = static_cast<double>(o.get_int("nsigma"));
  std::optional<IsotopeClassifier> classifier;
  const bool show_current = o.get_bool("show_current");

  IsotopeEvolutionFigure fig;
  fig.scene.kind = "isotope_evolution_fit";
  if (o.get_bool("use_classifier")) {
    auto loaded = IsotopeClassifier::load(o.get_string("classifier_file"));
    if (!loaded) {
      fig.fits.warnings.push_back("classifier: " + loaded.error().what);
    } else if (loaded->empty()) {
      fig.fits.warnings.emplace_back("classifier: no training samples yet");
    } else {
      classifier = std::move(*loaded);
    }
  }
  // Per spec row: (x, refitted, stored, uuid, flagged) for the panels.
  struct Point {
    double t;
    Value refit, stored;
    std::string uuid, label;
    bool flagged;
  };
  std::vector<std::vector<Point>> panel_points(specs.size());

  for (const auto& item : analyses.items()) {
    if (cancel && cancel->load()) return fail(ErrorKind::Cancelled, "isotope evolution fits: cancelled");
    if (item.exclusion.excluded()) continue;
    const Analysis& a = *item.analysis->analysis;
    // What to refit: (series kind, key, stored value, user exclusions,
    // reviewed, the isotope for signal-only checks), with its row.
    struct Target {
      SeriesKind kind;
      std::string key;
      Value stored;
      std::vector<std::size_t> excluded;
      const IsotopeData* iso;
      std::size_t row;
    };
    std::vector<Target> targets;
    for (std::size_t s = 0; s < specs.size(); ++s) {
      const RowSpec& spec = specs[s];
      if (spec.baseline) {
        for (const auto& iso : a.isotopes)
          if (iso.detector == spec.isotope) {
            if (skip_reviewed && iso.baseline_reviewed) {
              ++fig.fits.reviewed_kept;
            } else {
              targets.push_back({SeriesKind::Baseline, iso.detector, iso.baseline, iso.baseline_user_excluded, &iso, s});
            }
            break;  // one baseline per detector
          }
        continue;
      }
      for (const auto& iso : a.isotopes) {
        if (iso.key != spec.isotope && iso.isotope != spec.isotope) continue;
        const bool taken = std::any_of(targets.begin(), targets.end(), [&](const Target& t) {
          return t.kind == SeriesKind::Signal && t.key == iso.key;
        });
        if (taken) continue;  // an earlier row configures it
        if (skip_reviewed && iso.intercept_reviewed) {
          ++fig.fits.reviewed_kept;
        } else {
          targets.push_back({SeriesKind::Signal, iso.key, iso.intercept, iso.user_excluded, &iso, s});
        }
      }
    }
    if (targets.empty()) continue;
    auto raw = load_raw(a.uuid);
    if (!raw) {
      fig.fits.warnings.push_back(a.runid + ": " + raw.error().what);
      continue;
    }
    AnalysisRefits refits;
    refits.uuid = a.uuid;
    refits.runid = a.runid;
    refits.heads = a.heads;
    std::vector<FitEdit> edits;
    std::vector<std::size_t> rows;
    for (const auto& target : targets) {
      const RowSpec& spec = specs[target.row];
      const std::string label = target.kind == SeriesKind::Baseline ? target.key + " baseline" : target.key;
      const RawSeries* series = raw->find(target.kind, target.key);
      if (!series) {
        fig.fits.warnings.push_back(a.runid + " " + label + ": no raw data");
        continue;
      }
      r::FitSpec resolved = spec.fit;
      if (spec.auto_n)
        resolved.kind = std::cmp_greater_equal(series->t.size(), spec.n_threshold) ? spec.n_true : spec.n_false;
      FitEdit edit{target.kind, target.key, resolved, keep_excluded ? target.excluded : std::vector<std::size_t>{}};
      auto shape = fit_series(*series, resolved, edit.user_excluded);
      auto one = apply_fit_edits(a, *raw, {edit});
      if (!shape || !one) {
        fig.fits.warnings.push_back(a.runid + " " + (one ? shape.error().what : one.error().what));
        continue;
      }
      IsotopeRefit refit;
      refit.fit = one->fits.front();
      refit.stored = target.stored;
      refit.slope = slope_at_zero(shape->intercept);
      refit.outliers = shape->outliers.size();
      refit.rsquared_adj = adjusted_rsquared(*series, *shape, edit.user_excluded);
      refit.curvature = curvature_at(series->v, spec.curvature_at);
      const double v = refit.fit.value.value, e = refit.fit.value.error;
      const double pe = v != 0.0 ? std::abs(e / v) * 100.0 : std::numeric_limits<double>::infinity();
      auto flag = [&](const char* check, double value, double threshold) {
        refits.flags.push_back({target.key, check, value, threshold});
      };
      if (spec.max_percent_error && pe > *spec.max_percent_error) flag("percent_error", pe, *spec.max_percent_error);
      if (spec.smart_filter) {
        const auto& c = *spec.smart_filter;
        const double limit = c[0] * std::pow(v, c[1]) + c[2] * v + c[3];
        if (std::isfinite(limit) && e >= limit) flag("smart_filter", e, limit);
      }
      if (spec.max_outliers && static_cast<double>(refit.outliers) > *spec.max_outliers)
        flag("outliers", static_cast<double>(refit.outliers), *spec.max_outliers);
      if (spec.max_slope && (!spec.slope_intensity || v > *spec.slope_intensity) && refit.slope > *spec.max_slope)
        flag("slope", refit.slope, *spec.max_slope);
      if (spec.max_curvature && refit.curvature >= *spec.max_curvature)
        flag("curvature", refit.curvature, *spec.max_curvature);
      if (spec.min_rsquared && refit.rsquared_adj && *refit.rsquared_adj <= *spec.min_rsquared)
        flag("rsquared", *refit.rsquared_adj, *spec.min_rsquared);
      if (classifier && target.kind == SeriesKind::Signal) {
        refit.classification = classifier->classify(make_isotope_sample(*raw, target.key, target.iso->isotope));
        if (refit.classification && refit.classification->klass == 0)
          flag("classifier", refit.classification->probability, 0.5);
      }
      if (target.kind == SeriesKind::Signal && v != 0.0) {
        const double stb = std::abs(target.iso->baseline.error / v) * 100.0;
        if (spec.signal_to_baseline && spec.signal_to_baseline_percent && stb > *spec.signal_to_baseline &&
            pe >= *spec.signal_to_baseline_percent)
          flag("signal_to_baseline", pe, *spec.signal_to_baseline_percent);
        const double stbk = target.iso->blank.value / v * 100.0;
        if (spec.max_signal_to_blank && stbk >= *spec.max_signal_to_blank)
          flag("signal_to_blank", stbk, *spec.max_signal_to_blank);
      }
      edits.push_back(std::move(edit));
      rows.push_back(target.row);
      refits.isotopes.push_back(std::move(refit));
    }
    if (refits.isotopes.empty()) continue;
    auto all = apply_fit_edits(a, *raw, edits);
    if (!all) {
      fig.fits.warnings.push_back(a.runid + ": " + all.error().what);
      continue;
    }
    refits.edited = all->analysis;
    for (std::size_t k = 0; k < refits.isotopes.size(); ++k) {
      const auto& refit = refits.isotopes[k];
      const bool flagged = std::any_of(refits.flags.begin(), refits.flags.end(),
                                       [&](const GoodnessFlag& f) { return f.key == refit.fit.key; });
      panel_points[rows[k]].push_back({a.timestamp, refit.fit.value, refit.stored, a.uuid,
                                       a.runid + " " + refit.label(), flagged});
    }
    fig.fits.analyses.push_back(std::move(refits));
  }

  Graph g;
  g.title = "Isotope evolutions";
  g.x.title = "Run time";
  g.x.format = AxisFormat::Time;
  for (std::size_t s = 0; s < specs.size(); ++s) {
    const auto& spec = specs[s];
    const auto& pts = panel_points[s];
    const Color color = palette_color(static_cast<int>(s));
    Panel p;
    p.id = "p" + std::to_string(s);
    p.quantity = spec.isotope;
    p.y.title = spec.baseline ? spec.isotope + " baseline (fA)" : spec.isotope + " intercept (fA)";
    PointLayer refit, stored, flagged;
    refit.marker.color = color;
    refit.label = "refit";
    flagged.marker = refit.marker;
    flagged.marker.color = Color{214, 39, 40, 255};
    flagged.marker.shape = MarkerShape::Diamond;
    flagged.marker.size = 7;
    flagged.label = "flagged";
    stored.marker.shape = MarkerShape::Square;
    stored.marker.color = Color{140, 140, 140, 255};
    stored.marker.filled = false;
    stored.label = "current";
    int nflagged = 0;
    for (const auto& pt : pts) {
      PointLayer& layer = pt.flagged ? flagged : refit;
      nflagged += pt.flagged;
      layer.x.push_back(pt.t);
      layer.y.push_back(pt.refit.value);
      layer.y_err.push_back(pt.refit.error * nsigma);
      layer.refs.push_back(PointRef{pt.uuid});
      layer.tooltips.push_back(pt.label);
      if (show_current) {
        stored.x.push_back(pt.t);
        stored.y.push_back(pt.stored.value);
        stored.y_err.push_back(pt.stored.error * nsigma);
        stored.refs.emplace_back();
        stored.tooltips.push_back(pt.label + " current");
      }
    }
    TextLayer t;
    char buf[160];
    const std::string fit_name = spec.auto_n ? "auto (>= " + std::to_string(spec.n_threshold) + ": " +
                                                   std::string(r::to_string(spec.n_true)) + ", else " +
                                                   std::string(r::to_string(spec.n_false)) + ")"
                                             : std::string(r::to_string(spec.fit.kind));
    std::snprintf(buf, sizeof buf, "%s %s%s  %zu refitted, %d flagged", fit_name.c_str(),
                  spec.fit.error == r::ErrorType::Sd ? "SD" : "SEM", spec.fit.outliers.enabled ? " (outliers)" : "",
                  pts.size(), nflagged);
    t.lines.emplace_back(buf);
    p.layers.emplace_back(std::move(t));
    for (auto* layer : {&stored, &refit, &flagged}) {
      if (layer->x.empty()) continue;
      layer->excluded.assign(layer->x.size(), false);
      p.layers.emplace_back(std::move(*layer));
    }
    g.panels.push_back(std::move(p));
  }
  fig.scene.graphs.push_back(std::move(g));
  fig.scene.warnings = fig.fits.warnings;
  if (fig.fits.reviewed_kept > 0)
    fig.scene.warnings.push_back(std::to_string(fig.fits.reviewed_kept) + " reviewed intercept(s) kept");
  return fig;
}

namespace {

class IsotopeEvolutionFitUnit final : public Unit {
 public:
  std::string_view kind() const override { return "isotope_evolution_fit"; }
  std::string_view title() const override { return "Isotope evolutions"; }
  const SchemaPtr& schema() const override { return isotope_evolution_fit_schema(); }
  std::vector<PortSpec> inputs() const override { return {{"analyses", PortType::Dataset}}; }
  std::vector<PortSpec> outputs() const override {
    return {{"figure", PortType::Scene}, {"fits", PortType::IsotopeFits}};
  }
  bool reads_source() const override { return true; }
  Result<std::vector<PortValue>> execute(const std::vector<PortValue>& in, const Options& o,
                                         RunContext& ctx) const override {
    if (!ctx.source) return fail(ErrorKind::Config, "isotope evolution fits need a source");
    IAnalysisSource* source = ctx.source;
    auto fig = build_isotope_evolution_fits(
        *std::get<DatasetPtr>(in.at(0)), o, [source](const std::string& uuid) { return source->load_raw(uuid); },
        ctx.cancel);
    if (!fig) return fail(fig.error());
    for (const auto& w : fig->fits.warnings) ctx.diagnostics.push_back(w);
    return std::vector<PortValue>{PortValue(std::make_shared<const Scene>(std::move(fig->scene))),
                                  PortValue(std::make_shared<const IsotopeFitSet>(std::move(fig->fits)))};
  }
};

}  // namespace

std::unique_ptr<Unit> make_isotope_evolution_fit_unit() { return std::make_unique<IsotopeEvolutionFitUnit>(); }

}  // namespace pychron::processing
