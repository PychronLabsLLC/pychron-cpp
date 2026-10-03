#include "pychron/processing/isotope_evolution_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "pychron/processing/units.hpp"
#include "schema_builder.hpp"

namespace pychron::processing {

namespace r = pychron::reduction;
using namespace detail;

namespace {

struct RowSpec {
  std::string isotope;
  r::FitSpec fit;
  std::optional<double> max_percent_error, max_outliers, max_slope;
};

std::vector<RowSpec> row_specs(const Options& o) {
  std::vector<RowSpec> out;
  for (const auto& row : o.rows("isotopes")) {
    RowSpec s;
    s.isotope = row.get_string("isotope");
    s.fit.kind = r::parse_fit_kind(row.get_string("fit")).value_or(r::FitKind::Linear);
    s.fit.error = row.get_string("error") == "SD" ? r::ErrorType::Sd : r::ErrorType::Sem;
    s.fit.outliers.enabled = row.get_bool("filter_outliers");
    s.fit.outliers.iterations = static_cast<int>(row.get_int("iterations"));
    s.fit.outliers.std_devs = row.get_double("std_devs");
    s.max_percent_error = row.get_optional_double("max_percent_error");
    s.max_outliers = row.get_optional_double("max_outliers");
    s.max_slope = row.get_optional_double("max_slope");
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
      "figure.isotope_evolution_fit.isotope", "Isotope",
      {text("isotope", "Isotope", "Isotope", "Ar40", "A key (Ar40, H1:Ar40) or an isotope name"),
       choice("fit", "Fit", "Isotope", {"average", "linear", "parabolic", "cubic", "exponential"}, "linear"),
       choice("error", "Error", "Isotope", {"SEM", "SD"}, "SEM"),
       boolean("filter_outliers", "Filter outliers", "Isotope", false),
       when(integer("iterations", "Iterations", "Isotope", 1, 1, 10), "filter_outliers"),
       when(number("std_devs", "Std devs", "Isotope", 2.0, 0.5, 10.0, 0.5), "filter_outliers"),
       optional_number("max_percent_error", "Flag error above (%)", "Goodness"),
       optional_number("max_outliers", "Flag outliers above", "Goodness"),
       optional_number("max_slope", "Flag slope above (fA/s)", "Goodness",
                       "Legacy slope goodness: a growing signal is suspect")});
  ListSpec list;
  list.key = "isotopes";
  list.label = "Isotopes";
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
       boolean("skip_reviewed", "Keep reviewed intercepts", "Fit", false,
               "Intercepts already marked reviewed are not refitted or saved"),
       integer("nsigma", "Error bars (σ)", "Display", 1, 1, 3),
       boolean("show_current", "Show current values", "Display", true)},
      {list}));
  s->factory_presets = {{"Default", ""}};
  return s;
}

}  // namespace

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
      if (!seen.insert(i.fit.key).second) continue;
      out += (first ? "" : ",") + i.fit.key + "(" + std::string(r::to_string(i.fit.fit.kind)) + ")";
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
  const double nsigma = static_cast<double>(o.get_int("nsigma"));
  const bool show_current = o.get_bool("show_current");

  IsotopeEvolutionFigure fig;
  fig.scene.kind = "isotope_evolution_fit";
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
    // The isotopes to refit, with the row that configures each.
    std::vector<std::pair<const IsotopeData*, std::size_t>> targets;
    for (const auto& iso : a.isotopes)
      for (std::size_t s = 0; s < specs.size(); ++s)
        if (iso.key == specs[s].isotope || iso.isotope == specs[s].isotope) {
          if (skip_reviewed && iso.intercept_reviewed) {
            ++fig.fits.reviewed_kept;
          } else {
            targets.emplace_back(&iso, s);
          }
          break;
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
    for (const auto& [iso, s] : targets) {
      const RowSpec& spec = specs[s];
      FitEdit edit{SeriesKind::Signal, iso->key, spec.fit, keep_excluded ? iso->user_excluded : std::vector<std::size_t>{}};
      const RawSeries* series = raw->find(SeriesKind::Signal, iso->key);
      if (!series) {
        fig.fits.warnings.push_back(a.runid + " " + iso->key + ": no raw signal");
        continue;
      }
      auto shape = fit_series(*series, spec.fit, edit.user_excluded);
      auto one = apply_fit_edits(a, *raw, {edit});
      if (!shape || !one) {
        fig.fits.warnings.push_back(a.runid + " " + (one ? shape.error().what : one.error().what));
        continue;
      }
      IsotopeRefit refit;
      refit.fit = one->fits.front();
      refit.stored = iso->intercept;
      refit.slope = slope_at_zero(shape->intercept);
      refit.outliers = shape->outliers.size();
      const double v = refit.fit.value.value;
      const double pe = v != 0.0 ? std::abs(refit.fit.value.error / v) * 100.0 : 0.0;
      if (spec.max_percent_error && pe > *spec.max_percent_error)
        refits.flags.push_back({iso->key, "percent_error", pe, *spec.max_percent_error});
      if (spec.max_outliers && static_cast<double>(refit.outliers) > *spec.max_outliers)
        refits.flags.push_back({iso->key, "outliers", static_cast<double>(refit.outliers), *spec.max_outliers});
      if (spec.max_slope && refit.slope > *spec.max_slope)
        refits.flags.push_back({iso->key, "slope", refit.slope, *spec.max_slope});
      edits.push_back(std::move(edit));
      rows.push_back(s);
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
                                       a.runid + " " + refit.fit.key, flagged});
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
    p.y.title = spec.isotope + " intercept (fA)";
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
        stored.refs.push_back(PointRef{});
        stored.tooltips.push_back(pt.label + " current");
      }
    }
    TextLayer t;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s %s%s  %zu refitted, %d flagged", std::string(r::to_string(spec.fit.kind)).c_str(),
                  spec.fit.error == r::ErrorType::Sd ? "SD" : "SEM", spec.fit.outliers.enabled ? " (outliers)" : "",
                  pts.size(), nflagged);
    t.lines.push_back(buf);
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
