// Inverse isochron (legacy pipeline/plot/plotter/isochron.py InverseIsochron,
// options/isochron.py).

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>

#include "figure_common.hpp"
#include "pychron/processing/arar_figures.hpp"
#include "pychron/processing/arar_groups.hpp"
#include "pychron/processing/units.hpp"
#include "schema_builder.hpp"

namespace pychron::processing {

namespace {

using namespace detail;
namespace r = pychron::reduction;

SchemaPtr build_schema() {
  auto s = std::make_shared<Schema>();
  s->kind = "figure.inverse_isochron";
  s->title = "Inverse isochron";
  s->fields = {
      choice("method", "Regression", "Calculations", {"new_york", "york", "reed"}),
      choice("error_kind", "Errors", "Calculations", {"se", "mse"}),
      boolean("include_j_error", "Age with J error", "Calculations", true),
      boolean("exclude_non_plateau", "Leave out steps outside the plateau", "Calculations", false,
              "Plateau by the Fleck criterion: 3 steps, 50% gas, 2 sigma"),
      choice("ellipse", "Error ellipses", "Display", {"1sigma", "2sigma", "95%", "none"}, "1sigma"),
      boolean("fill_ellipses", "Fill ellipses", "Display", false),
      boolean("show_points", "Point markers", "Display", true),
      number("marker_size", "Marker size", "Display", 4.0, 1.0, 30.0, 0.5),
      boolean("show_envelope", "Fit envelope", "Display", true),
      boolean("show_results", "Results text", "Display", true),
      boolean("show_percent_error", "Percent error", "Display", true),
      boolean("show_nominal_intercept", "Atmospheric intercept", "Display", true),
      number("nominal_intercept", "Atmospheric 40/36", "Display", 298.56, 1.0, 1e6),
      optional_number("x.min", "X min", "Axes"),
      optional_number("x.max", "X max", "Axes"),
      optional_number("y.min", "Y min", "Axes"),
      optional_number("y.max", "Y max", "Axes"),
  };
  for (auto& f : common_figure_fields()) s->fields.push_back(std::move(f));
  s->lists = {groups_list()};
  s->factory_presets = {
      {"Default", ""},
      {"Filled 95%", "ellipse = \"95%\"\nfill_ellipses = true\nshow_points = false\n"},
      {"Plateau steps", "exclude_non_plateau = true\n"},
  };
  return s;
}

double ellipse_scale(const std::string& e) {
  if (e == "2sigma") return 2.0;
  if (e == "95%") return 2.4477;  // sqrt(chi2(0.95, 2))
  return 1.0;
}

}  // namespace

const SchemaPtr& isochron_schema() {
  static const SchemaPtr s = build_schema();
  return s;
}

Result<Scene> build_isochron(const Dataset& d, const Options& o) { return detail::build_isochron_scene(d, o, nullptr); }

Result<Scene> detail::build_isochron_scene(const Dataset& d, const Options& o, const std::set<std::string>* plateau_steps) {
  Scene scene;
  scene.kind = "inverse_isochron";
  apply_common_style(o, scene);
  const int nsigma = std::max<int>(1, static_cast<int>(o.get_int("error_bar_nsigma")));
  const bool hide_excluded = o.get_string("excluded_style") == "hidden";
  const auto method = r::parse_york_method(o.get_string("method")).value_or(r::YorkMethod::NewYork);
  const bool mse = o.get_string("error_kind") == "mse";
  const Corner stats_corner = parse_corner(o.get_string("statistics_location")).value_or(Corner::TopRight);
  const std::string ellipse = o.get_string("ellipse");
  MeanTextOptions mt;
  mt.sig = static_cast<int>(o.get_int("statistics_sig_figs"));
  mt.nsigma = nsigma;
  mt.percent = o.get_bool("show_percent_error");
  mt.mswd = false;
  mt.n = false;
  const auto group_rows = o.rows("groups");

  for (int graph : d.graphs()) {
    Graph g;
    std::string title = o.get_string("title");
    const std::string gname = d.graph_name(graph);
    if (auto p = title.find("{graph}"); p != std::string::npos) title.replace(p, 7, gname);
    g.title = title.empty() ? gname : title;
    g.x.title = "39Ar/40Ar";
    Panel p;
    p.id = "isochron";
    p.quantity = "inverse_isochron";
    p.y.title = "36Ar/40Ar";
    double xmax = 0, ymax = 0;
    std::vector<std::string> lines;
    std::size_t missing = 0;

    for (const auto& [group, items] : d.groups_of_graph(graph)) {
      const GroupStyle st = group_style(d, graph, group, group_rows, MarkerShape::Circle);
      auto points = isochron_points(items);
      missing += items.size() - points.size();

      // Steps outside the plateau (by age) are left out of the fit.
      std::vector<bool> outside(points.size(), false);
      if (o.get_bool("exclude_non_plateau") && plateau_steps) {
        for (std::size_t i = 0; i < points.size(); ++i)
          outside[i] = !plateau_steps->contains(points[i].item->analysis->analysis->uuid);
      } else if (o.get_bool("exclude_non_plateau") && points.size() >= 3) {
        std::vector<double> ages, errs, gas;
        std::unique_ptr<bool[]> ex(new bool[points.size()]);
        bool ok = true;
        for (std::size_t i = 0; i < points.size(); ++i) {
          const auto& ra = *points[i].item->analysis;
          if (!ra.arar || !ra.arar->ages) {
            ok = false;
            break;
          }
          ages.push_back(ra.arar->ages->age.nominal());
          errs.push_back(ra.arar->ages->age.std_dev());
          gas.push_back(ra.arar->f.interference_corrected[r::index(r::ArgonIsotope::Ar39)].nominal());
          ex[i] = points[i].item->exclusion.excluded();
        }
        if (ok) {
          const auto plateau = r::find_plateau(ages, errs, gas, std::span<const bool>(ex.get(), points.size()), {});
          for (std::size_t i = 0; i < points.size(); ++i)
            outside[i] = !plateau || i < plateau->first || i > plateau->last;
        }
      }

      std::vector<IsochronPoint> fit_points;
      EllipseLayer el;
      el.scale = ellipse_scale(ellipse);
      el.line = st.color;
      el.fill = with_alpha(st.color, static_cast<std::uint8_t>(st.fill_alpha * 255 / 300));
      el.filled = o.get_bool("fill_ellipses");
      el.group = group;
      PointLayer pts;
      pts.group = group;
      pts.label = st.label;
      pts.marker.shape = st.marker;
      pts.marker.size = o.get_bool("show_points") ? o.get_double("marker_size") : 0.5;
      pts.marker.color = st.color;
      pts.excluded_marker = pts.marker;
      pts.excluded_marker.filled = false;
      pts.excluded_marker.color = Color{150, 150, 150, 255};
      pts.show_excluded = !hide_excluded;
      for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& pt = points[i];
        const bool excluded = pt.item->exclusion.excluded() || outside[i];
        if (!excluded) fit_points.push_back(pt);
        if (excluded && hide_excluded) continue;
        el.x.push_back(pt.x);
        el.y.push_back(pt.y);
        el.sx.push_back(pt.sx);
        el.sy.push_back(pt.sy);
        el.rho.push_back(pt.rho);
        el.refs.push_back({pt.item->analysis->analysis->uuid});
        el.excluded.push_back(excluded);
        pts.x.push_back(pt.x);
        pts.y.push_back(pt.y);
        pts.refs.push_back({pt.item->analysis->analysis->uuid});
        pts.excluded.push_back(excluded);
        pts.tooltips.push_back(pt.item->analysis->analysis->runid + (excluded ? "  excluded" : "") + "\n39/40 " +
                               format_sig(pt.x, 5) + " ± " + format_sig(pt.sx, 2) + "\n36/40 " + format_sig(pt.y, 5) +
                               " ± " + format_sig(pt.sy, 2) + "\nrho " + format_sig(pt.rho, 3));
        xmax = std::max(xmax, pt.x + el.scale * pt.sx);
        ymax = std::max(ymax, pt.y + el.scale * pt.sy);
      }
      if (ellipse != "none") p.layers.emplace_back(std::move(el));
      p.layers.emplace_back(std::move(pts));

      const std::string label = st.label.empty() ? "" : st.label + ": ";
      if (fit_points.size() < 3) {
        if (!points.empty()) lines.push_back(label + "fewer than 3 points for the fit");
        continue;
      }
      auto iso = isochron_age(fit_points, method, mse, o.get_bool("include_j_error"));
      if (!iso) {
        scene.warnings.push_back(label + iso.error().what);
        continue;
      }
      const auto& f = iso->fit;
      const double x_end = std::max(xmax, f.x_intercept > 0 ? f.x_intercept : 0.0) * 1.1;
      LineLayer line;
      BandLayer band;
      band.fill = with_alpha(st.color, 45);
      constexpr int kSamples = 50;
      const double scale = mse && f.mswd > 1 ? std::sqrt(f.mswd) : 1.0;
      for (int k = 0; k < kSamples; ++k) {
        const double x = x_end * k / (kSamples - 1);
        const double y = f.intercept + f.slope * x;
        const double var = f.intercept_err * f.intercept_err + x * x * f.slope_err * f.slope_err + 2 * x * f.covariance;
        const double e = std::sqrt(std::max(0.0, var)) * scale * nsigma;
        line.x.push_back(x);
        line.y.push_back(y);
        band.x.push_back(x);
        band.low.push_back(y - e);
        band.high.push_back(y + e);
      }
      line.style.color = st.color;
      line.style.width = 1.5;
      line.group = band.group = group;
      if (o.get_bool("show_envelope")) p.layers.emplace_back(std::move(band));
      p.layers.emplace_back(std::move(line));
      ymax = std::max(ymax, f.intercept);
      xmax = std::max(xmax, x_end / 1.1);
      if (o.get_bool("show_results")) {
        if (iso->age) lines.push_back(mean_text(label + "age ", iso->age->nominal(), iso->age->std_dev(), nullptr, 0, mt, "Ma"));
        lines.push_back(mean_text(label + "(40/36)trapped ", iso->trapped.value, iso->trapped.error, nullptr, 0, mt));
        std::string stats = "MSWD " + format_sig(f.mswd, 3) + (f.mswd_acceptable ? "" : "*") + "  p " +
                            format_sig(f.probability, 2) + "  n " + std::to_string(f.n) + "/" + std::to_string(points.size());
        lines.push_back(stats);
      }
    }
    if (o.get_bool("show_nominal_intercept")) {
      PointLayer atm;
      atm.x = {0.0};
      atm.y = {1.0 / o.get_double("nominal_intercept")};
      atm.refs = {PointRef{}};
      atm.excluded = {false};
      atm.tooltips = {"atmospheric 40/36 = " + format_sig(o.get_double("nominal_intercept"), 6)};
      atm.marker.shape = MarkerShape::Triangle;
      atm.marker.size = 8;
      atm.marker.color = Color{0, 0, 0, 255};
      atm.marker.filled = false;
      ymax = std::max(ymax, atm.y.front());
      p.layers.emplace_back(std::move(atm));
    }
    if (missing)
      scene.warnings.push_back(std::to_string(missing) + " analyses lack interference-corrected Ar40, Ar39 or Ar36");
    if (!lines.empty()) {
      TextLayer t;
      t.lines = std::move(lines);
      t.corner = stats_corner;
      t.font_size = scene.style.fonts.annotation;
      p.layers.emplace_back(std::move(t));
    }
    g.x.min = o.get_optional_double("x.min").value_or(0.0);
    g.x.max = o.get_optional_double("x.max").value_or(xmax > 0 ? xmax * 1.1 : 1.0);
    p.y.min = o.get_optional_double("y.min").value_or(0.0);
    p.y.max = o.get_optional_double("y.max").value_or(ymax > 0 ? ymax * 1.15 : 1.0 / 295.5 * 1.2);
    g.panels.push_back(std::move(p));
    scene.graphs.push_back(std::move(g));
  }
  if (scene.graphs.empty()) scene.warnings.emplace_back("no analyses");
  return scene;
}

namespace {

class IsochronUnit final : public Unit {
 public:
  std::string_view kind() const override { return "inverse_isochron"; }
  std::string_view title() const override { return "Inverse isochron"; }
  const SchemaPtr& schema() const override { return isochron_schema(); }
  std::vector<PortSpec> inputs() const override { return {{"analyses", PortType::Dataset}}; }
  std::vector<PortSpec> outputs() const override { return {{"figure", PortType::Scene}}; }
  Result<std::vector<PortValue>> execute(const std::vector<PortValue>& in, const Options& o,
                                         RunContext&) const override {
    auto scene = build_isochron(*std::get<DatasetPtr>(in.at(0)), o);
    if (!scene) return fail(scene.error());
    return std::vector<PortValue>{PortValue(std::make_shared<const Scene>(std::move(*scene)))};
  }
};

}  // namespace

std::unique_ptr<Unit> make_isochron_unit() { return std::make_unique<IsochronUnit>(); }

}  // namespace pychron::processing
