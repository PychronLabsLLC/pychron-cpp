#include "pychron/processing/time_series.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>

#include "pychron/processing/quantity.hpp"
#include "pychron/processing/units.hpp"
#include "pychron/reduction/fits.hpp"
#include "pychron/reduction/stats.hpp"
#include "figure_common.hpp"
#include "schema_builder.hpp"

namespace pychron::processing {

namespace {

using namespace detail;
namespace r = pychron::reduction;

const std::vector<std::string> kMarkers{"circle", "square", "diamond", "triangle", "cross", "plus", "star"};
const std::vector<std::string> kFits{"none", "average", "weighted_mean", "linear", "parabolic", "cubic", "exponential"};

SchemaPtr panel_schema() {
  static const SchemaPtr s = make_schema(
      "figure.time_series.panel", "Panel",
      {quantity("quantity", "Quantity", "Panel", "Ar40"),
       boolean("enabled", "Show", "Panel", true),
       text("title", "Title", "Panel", "", "Empty: from the quantity"),
       number("height", "Height", "Panel", 1.0, 0.1, 10.0, 0.1),
       choice("scale", "Scale", "Panel", {"linear", "log"}),
       optional_number("y_min", "Y min", "Panel"),
       optional_number("y_max", "Y max", "Panel"),
       choice("marker", "Marker", "Panel", kMarkers),
       number("marker_size", "Marker size", "Panel", 5.0, 1.0, 30.0, 0.5),
       optional_color("marker_color", "Marker colour", "Panel"),
       boolean("show_errors", "Error bars", "Panel", true),
       choice("fit", "Fit", "Panel", kFits),
       when(choice("fit_error", "Fit error", "Panel", {"sem", "sd", "msem"}), "fit != none"),
       when(boolean("show_envelope", "Fit envelope", "Panel", true), "fit != none"),
       choice("deviation", "Plot as", "Panel", {"none", "absolute", "percent"}),
       boolean("show_statistics", "Statistics", "Panel", true),
       optional_number("reference_value", "Reference line", "Panel")});
  return s;
}


SchemaPtr build_schema() {
  auto s = std::make_shared<Schema>();
  s->kind = "figure.time_series";
  s->title = "Time series";
  s->version = 1;
  s->fields = {
      choice("x.kind", "X axis", "Axes", {"time", "relative", "index"}),
      when(choice("x.origin", "Hours from", "Axes", {"last", "first", "now"}), "x.kind == relative"),
      when(text("x.time_format", "Date format", "Axes", "auto", "auto, or strftime: %Y-%m-%d %H:%M"), "x.kind == time"),
      optional_number("x.min", "X min", "Axes", "epoch seconds (time), hours (relative) or run number (index)"),
      optional_number("x.max", "X max", "Axes"),
      number("x.padding_percent", "X padding (%)", "Axes", 2.0, 0.0, 50.0, 0.5),
      text("x.title", "X title", "Axes", "", "Empty: automatic"),
  };
  for (auto& f : common_figure_fields()) s->fields.push_back(std::move(f));
  ListSpec panels;
  panels.key = "panels";
  panels.label = "Panels";
  panels.section = "Panels";
  panels.row = panel_schema();
  panels.min_rows = 1;
  panels.max_rows = 12;
  panels.default_rows_toml = {"quantity = \"Ar40\""};
  s->lists = {panels, groups_list()};
  s->factory_presets = {
      {"Default",
       "[[panels]]\nquantity = \"Ar40\"\n[[panels]]\nquantity = \"Ar40/Ar36\"\nfit = \"weighted_mean\"\n"},
      {"Air monitor",
       "[[panels]]\nquantity = \"Ar40/Ar36\"\nfit = \"weighted_mean\"\nheight = 2.0\n"
       "[[panels]]\nquantity = \"Ar40\"\n[[panels]]\nquantity = \"Ar36\"\n"},
      {"Blanks",
       "[[panels]]\nquantity = \"Ar40.bs_corrected\"\nfit = \"average\"\n"
       "[[panels]]\nquantity = \"Ar39.bs_corrected\"\nfit = \"average\"\n"
       "[[panels]]\nquantity = \"Ar38.bs_corrected\"\nfit = \"average\"\n"
       "[[panels]]\nquantity = \"Ar37.bs_corrected\"\nfit = \"average\"\n"
       "[[panels]]\nquantity = \"Ar36.bs_corrected\"\nfit = \"average\"\n"},
      {"Unknowns",
       "[[panels]]\nquantity = \"age\"\nfit = \"weighted_mean\"\nheight = 2.0\n"
       "[[panels]]\nquantity = \"kca\"\nscale = \"log\"\n[[panels]]\nquantity = \"radiogenic_yield\"\n"},
      {"Spectrometer",
       "[[panels]]\nquantity = \"Ar40.baseline\"\n[[panels]]\nquantity = \"Ar40.intercept\"\n"
       "[[panels]]\nquantity = \"extract_value\"\nshow_errors = false\n"},
  };
  return s;
}




r::FitKind fit_kind(const std::string& f) {
  if (f == "parabolic") return r::FitKind::Parabolic;
  if (f == "cubic") return r::FitKind::Cubic;
  if (f == "exponential") return r::FitKind::Exponential;
  if (f == "average") return r::FitKind::Average;
  return r::FitKind::Linear;
}

struct Point {
  double x, y, e;
  const DatasetItem* item;
};

struct GroupFit {
  LineLayer line;
  std::optional<BandLayer> band;
  std::vector<std::string> text;
};

// A mean (average / weighted_mean) or a curve fit of the included points.
std::optional<GroupFit> fit_group(const std::vector<Point>& pts, const Options& panel, double x_lo, double x_hi,
                                  Color color, int sig, std::vector<std::string>& warnings, const std::string& gname) {
  const std::string kind = panel.get_string("fit");
  std::vector<double> x, y, e;
  for (const auto& p : pts)
    if (p.item->exclusion.included()) {
      x.push_back(p.x);
      y.push_back(p.y);
      e.push_back(p.e);
    }
  if (x.empty()) return std::nullopt;
  const auto ek = r::parse_mean_error_kind(panel.get_string("fit_error")).value_or(r::MeanErrorKind::Sem);
  GroupFit gf;
  gf.line.style.color = color;
  gf.line.style.width = 1.5;
  if (kind == "average" || kind == "weighted_mean") {
    auto m = kind == "weighted_mean" ? r::weighted_mean(y, e, ek) : r::arithmetic_mean(y, e, ek);
    if (!m) {
      warnings.push_back(gname + ": " + m.error().what);
      return std::nullopt;
    }
    gf.line.x = {x_lo, x_hi};
    gf.line.y = {m->value, m->value};
    if (panel.get_bool("show_envelope") && m->error > 0) {
      BandLayer b;
      b.x = {x_lo, x_hi};
      b.low = {m->value - m->error, m->value - m->error};
      b.high = {m->value + m->error, m->value + m->error};
      b.fill = with_alpha(color, 50);
      gf.band = b;
    }
    std::string line = (kind == "weighted_mean" ? "wtd mean " : "mean ") + format_sig(m->value, sig) + " ± " +
                       format_sig(m->error, 2) + " (" + std::string(r::to_string(ek)) + ")";
    if (m->n > 1) line += "  MSWD " + format_sig(m->mswd, 3) + (m->mswd_acceptable ? "" : "*");
    gf.text.push_back(line);
    return gf;
  }
  // Curve fits in hours from the first included x, refit at shifted origins
  // for the envelope (the intercept error is the fitted value's error at 0).
  r::FitSpec spec;
  spec.kind = fit_kind(kind);
  spec.error = panel.get_string("fit_error") == "sd" ? r::ErrorType::Sd : r::ErrorType::Sem;
  const double x0 = *std::min_element(x.begin(), x.end());
  auto series_at = [&](double origin) {
    r::Series s;
    for (std::size_t i = 0; i < x.size(); ++i) {
      s.x.push_back(x[i] - origin);
      s.y.push_back(y[i]);
    }
    return s;
  };
  auto first = r::fit(series_at(x0), spec);
  if (!first) {
    warnings.push_back(gname + ": " + first.error().what);
    return std::nullopt;
  }
  constexpr int kSamples = 60;
  BandLayer band;
  band.fill = with_alpha(color, 50);
  bool band_ok = panel.get_bool("show_envelope");
  for (int i = 0; i < kSamples; ++i) {
    const double xs = x_lo + (x_hi - x_lo) * i / (kSamples - 1);
    auto f = r::fit(series_at(xs), spec);
    if (!f) {
      band_ok = false;
      continue;
    }
    gf.line.x.push_back(xs);
    gf.line.y.push_back(f->value);
    band.x.push_back(xs);
    band.low.push_back(f->value - f->error);
    band.high.push_back(f->value + f->error);
  }
  if (band_ok && !band.x.empty()) gf.band = band;
  gf.text.push_back(kind + " fit  n " + std::to_string(first->n_used) + "  resid sd " + format_sig(first->residual_sd, 3));
  return gf;
}

Axis make_y_axis(const Options& panel, const Quantity& q, const std::string& deviation) {
  Axis y;
  y.title = panel.get_string("title");
  if (y.title.empty()) {
    y.title = q.label();
    if (deviation == "absolute") y.title = "Δ " + y.title;
    if (deviation == "percent") y.title = "Δ% " + q.label();
  }
  y.scale = panel.get_string("scale") == "log" ? AxisScale::Log : AxisScale::Linear;
  y.min = panel.get_optional_double("y_min");
  y.max = panel.get_optional_double("y_max");
  return y;
}

}  // namespace

const SchemaPtr& time_series_schema() {
  static const SchemaPtr s = build_schema();
  return s;
}

Result<Scene> build_time_series(const Dataset& d, const Options& o, double now) {
  Scene scene;
  scene.kind = "time_series";
  apply_common_style(o, scene);

  const std::string x_kind = o.get_string("x.kind");
  const std::string origin = o.get_string("x.origin");
  const int nsigma = static_cast<int>(o.get_int("error_bar_nsigma"));
  const bool hide_excluded = o.get_string("excluded_style") == "hidden";
  const Corner stats_corner = parse_corner(o.get_string("statistics_location")).value_or(Corner::TopLeft);
  const int sig = static_cast<int>(o.get_int("statistics_sig_figs"));
  const auto group_rows = o.rows("groups");

  struct PanelDef {
    Options row;
    Quantity q;
    std::size_t index;
  };
  std::vector<PanelDef> panels;
  {
    std::size_t i = 0;
    for (const auto& row : o.rows("panels")) {
      const std::size_t idx = i++;
      if (!row.get_bool("enabled")) continue;
      auto q = Quantity::parse(row.get_string("quantity"));
      if (!q) {
        scene.warnings.push_back("panel " + std::to_string(idx + 1) + ": " + q.error().what);
        continue;
      }
      panels.push_back({row, *q, idx});
    }
  }
  if (panels.empty()) return fail(ErrorKind::Config, "time series: no panel to show");

  // Time origin over the whole dataset, so graphs share a clock (legacy uses
  // each group's last analysis).
  double t_first = 0, t_last = 0;
  bool any = false;
  for (const auto& it : d.items()) {
    const double t = it.analysis->analysis->timestamp;
    if (!any || t < t_first) t_first = t;
    if (!any || t > t_last) t_last = t;
    any = true;
  }
  const double t_origin = origin == "first" ? t_first : origin == "now" ? now : t_last;

  for (int graph_index : d.graphs()) {
    Graph g;
    std::string title = o.get_string("title");
    const std::string gname = d.graph_name(graph_index);
    if (auto p = title.find("{graph}"); p != std::string::npos) title.replace(p, 7, gname);
    g.title = title.empty() ? gname : title;

    // x per item: time, hours, or run order within the graph.
    std::map<const DatasetItem*, double> xs;
    {
      std::vector<const DatasetItem*> items;
      for (const auto& it : d.items())
        if (it.path.graph == graph_index) items.push_back(&it);
      std::stable_sort(items.begin(), items.end(), [](const auto* a, const auto* b) {
        return a->analysis->analysis->timestamp < b->analysis->analysis->timestamp;
      });
      for (std::size_t i = 0; i < items.size(); ++i) {
        const double t = items[i]->analysis->analysis->timestamp;
        xs[items[i]] = x_kind == "time" ? t : x_kind == "relative" ? (t - t_origin) / 3600.0 : static_cast<double>(i + 1);
      }
    }
    double x_lo = 0, x_hi = 1;
    if (!xs.empty()) {
      x_lo = x_hi = xs.begin()->second;
      for (const auto& [_, x] : xs) {
        x_lo = std::min(x_lo, x);
        x_hi = std::max(x_hi, x);
      }
    }
    double span = x_hi - x_lo;
    if (span <= 0) span = x_kind == "time" ? 3600.0 : 1.0;
    const double pad = span * o.get_double("x.padding_percent") / 100.0;
    g.x.min = o.get_optional_double("x.min").value_or(x_lo - (x_hi == x_lo ? span / 2 : pad));
    g.x.max = o.get_optional_double("x.max").value_or(x_hi + (x_hi == x_lo ? span / 2 : pad));
    g.x.format = x_kind == "time" ? AxisFormat::Time : AxisFormat::Number;
    const std::string tf = o.get_string("x.time_format");
    // auto: precision follows the span (a queue of minutes needs seconds).
    if (tf != "auto") g.x.time_format = tf;
    else if (span < 2 * 3600.0) g.x.time_format = "%H:%M:%S";
    else if (span < 3 * 86400.0) g.x.time_format = "%m-%d%n%H:%M";
    else g.x.time_format = "%Y-%m-%d";
    g.x.title = o.get_string("x.title");
    if (g.x.title.empty())
      g.x.title = x_kind == "time" ? "Time (UTC)"
                  : x_kind == "relative" ? "Hours from " + std::string(origin == "now" ? "now" : origin + " analysis")
                                         : "Run";

    for (const auto& pd : panels) {
      Panel p;
      p.id = "p" + std::to_string(pd.index);
      p.quantity = pd.q.text();
      p.height = pd.row.get_double("height");
      const std::string deviation = pd.row.get_string("deviation");
      p.y = make_y_axis(pd.row, pd.q, deviation);
      const bool errors = pd.row.get_bool("show_errors") && nsigma > 0;
      const auto shape = parse_marker(pd.row.get_string("marker")).value_or(MarkerShape::Circle);
      const auto panel_color = parse_color(pd.row.get_string("marker_color"));
      std::size_t missing = 0;
      int stack = 0;
      std::vector<std::string> stat_lines;

      for (const auto& [group, items] : d.groups_of_graph(graph_index)) {
        const GroupStyle gs = group_style(d, graph_index, group, group_rows, shape);
        Color color = gs.color;
        const bool group_colored = group >= 0 && static_cast<std::size_t>(group) < group_rows.size() &&
                                   parse_color(group_rows[group].get_string("color"));
        if (panel_color && !group_colored && d.groups_of_graph(graph_index).size() == 1) color = *panel_color;
        const MarkerShape gshape = gs.marker;
        const std::string label = gs.label;

        std::vector<Point> pts;
        for (const auto* it : items) {
          auto v = pd.q.eval(*it->analysis);
          if (!v) {
            ++missing;
            continue;
          }
          pts.push_back({xs[it], v->value, v->error, it});
        }
        if (pts.empty()) continue;

        // Deviation from the included mean of this group.
        if (deviation != "none") {
          std::vector<double> yv, ev;
          for (const auto& pt : pts)
            if (pt.item->exclusion.included()) {
              yv.push_back(pt.y);
              ev.push_back(pt.e);
            }
          auto m = r::weighted_mean(yv, ev);
          if (!m) m = r::arithmetic_mean(yv);
          if (m && m->value != 0) {
            for (auto& pt : pts) {
              if (deviation == "absolute") {
                pt.y -= m->value;
              } else {
                pt.y = (pt.y - m->value) / m->value * 100.0;
                pt.e = pt.e / std::abs(m->value) * 100.0;
              }
            }
          }
        }

        PointLayer layer;
        layer.group = group;
        layer.label = label;
        layer.marker.shape = gshape;
        layer.marker.size = pd.row.get_double("marker_size");
        layer.marker.color = color;
        layer.excluded_marker = layer.marker;
        layer.excluded_marker.filled = false;
        layer.excluded_marker.color = Color{150, 150, 150, 255};
        layer.show_excluded = !hide_excluded;
        std::size_t n_in = 0;
        for (const auto& pt : pts) {
          layer.x.push_back(pt.x);
          layer.y.push_back(pt.y);
          if (errors) layer.y_err.push_back(pt.e * nsigma);
          layer.refs.push_back({pt.item->analysis->analysis->uuid});
          const bool ex = pt.item->exclusion.excluded();
          layer.excluded.push_back(ex);
          if (!ex) ++n_in;
          const Analysis& a = *pt.item->analysis->analysis;
          layer.tooltips.push_back(a.runid + (a.tag != "ok" ? "  [" + a.tag + "]" : "") + (ex ? "  excluded" : "") +
                                   "\n" + pd.q.label() + " " + format_sig(pt.y, sig + 2) + " ± " +
                                   format_sig(pt.e, 3) + "\n" + format_utc(a.timestamp));
        }

        if (pd.row.get_string("fit") != "none") {
          if (auto gf = fit_group(pts, pd.row, *g.x.min, *g.x.max, color, sig, scene.warnings, label)) {
            if (gf->band) {
              gf->band->group = group;
              p.layers.emplace_back(std::move(*gf->band));
            }
            gf->line.group = group;
            p.layers.emplace_back(std::move(gf->line));
            if (pd.row.get_bool("show_statistics"))
              for (auto& t : gf->text) stat_lines.push_back(label.empty() ? t : label + ": " + t);
          }
        } else if (pd.row.get_bool("show_statistics")) {
          std::vector<double> yv, ev;
          for (const auto& pt : pts)
            if (pt.item->exclusion.included()) {
              yv.push_back(pt.y);
              ev.push_back(pt.e);
            }
          if (auto m = r::arithmetic_mean(yv, ev, r::MeanErrorKind::Sd)) {
            stat_lines.push_back((label.empty() ? std::string() : label + ": ") + "mean " + format_sig(m->value, sig) +
                                 " ± " + format_sig(m->sd, 2) + " (sd)  n " + std::to_string(n_in) + "/" +
                                 std::to_string(pts.size()));
          }
        }
        p.layers.emplace_back(std::move(layer));
      }

      if (const auto ref = pd.row.get_optional_double("reference_value")) {
        GuideLayer gl;
        gl.value = *ref;
        gl.style.color = Color{120, 120, 120, 255};
        gl.style.dash = LineDash::Dash;
        p.layers.emplace_back(gl);
      }
      if (!stat_lines.empty()) {
        TextLayer t;
        t.lines = std::move(stat_lines);
        t.corner = stats_corner;
        t.stack = stack++;
        t.font_size = scene.style.fonts.annotation;
        p.layers.emplace_back(std::move(t));
      }
      if (missing)
        scene.warnings.push_back(pd.q.text() + ": " + std::to_string(missing) + " analyses have no value; not plotted");
      g.panels.push_back(std::move(p));
    }
    scene.graphs.push_back(std::move(g));
  }
  if (scene.graphs.empty()) scene.warnings.push_back("no analyses");
  return scene;
}

namespace {

class TimeSeriesUnit final : public Unit {
 public:
  std::string_view kind() const override { return "time_series"; }
  std::string_view title() const override { return "Time series"; }
  const SchemaPtr& schema() const override { return time_series_schema(); }
  std::vector<PortSpec> inputs() const override { return {{"analyses", PortType::Dataset}}; }
  std::vector<PortSpec> outputs() const override { return {{"figure", PortType::Scene}}; }

  Result<std::vector<PortValue>> execute(const std::vector<PortValue>& in, const Options& o,
                                         RunContext&) const override {
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto scene = build_time_series(*std::get<DatasetPtr>(in.at(0)), o, now);
    if (!scene) return fail(scene.error());
    return std::vector<PortValue>{PortValue(std::make_shared<const Scene>(std::move(*scene)))};
  }
};

}  // namespace

std::unique_ptr<Unit> make_time_series_unit() { return std::make_unique<TimeSeriesUnit>(); }

}  // namespace pychron::processing
