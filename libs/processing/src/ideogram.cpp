// Ideogram (legacy pipeline/plot/plotter/ideogram.py, options/ideogram.py).

#include <algorithm>
#include <cmath>
#include <numeric>

#include "figure_common.hpp"
#include "pychron/processing/arar_figures.hpp"
#include "pychron/processing/arar_groups.hpp"
#include "pychron/processing/quantity.hpp"
#include "pychron/processing/units.hpp"
#include "schema_builder.hpp"

namespace pychron::processing {

namespace {

using namespace detail;
namespace r = pychron::reduction;

const std::vector<std::string> kMarkers{"circle", "square", "diamond", "triangle", "cross", "plus", "star"};

SchemaPtr panel_schema() {
  static const SchemaPtr s = make_schema(
      "figure.ideogram.panel", "Panel",
      {choice("kind", "Panel", "Panel", {"probability", "analysis_number", "analysis_number_nonsorted", "value"}),
       when(quantity("quantity", "Quantity (value panels)", "Panel", "kca"), "kind == value"),
       number("height", "Height", "Panel", 1.0, 0.1, 10.0, 0.1),
       when(choice("scale", "Scale", "Panel", {"linear", "log"}), "kind == value"),
       optional_number("y_min", "Y min", "Panel"),
       optional_number("y_max", "Y max", "Panel"),
       choice("marker", "Marker", "Panel", kMarkers),
       number("marker_size", "Marker size", "Panel", 5.0, 1.0, 30.0, 0.5),
       boolean("show_errors", "Error bars", "Panel", true)});
  return s;
}

// A span marks a range of the x axis (ages) on every panel, or a rectangle on
// one panel when that panel is named and y bounds are given.
SchemaPtr span_schema() {
  static const SchemaPtr s = make_schema(
      "figure.ideogram.span", "Span",
      {text("label", "Label", "Span", "", "Drawn at the top of the span"),
       optional_number("min", "X min", "Span", "Unset: from the left edge"),
       optional_number("max", "X max", "Span", "Unset: to the right edge"),
       choice("panel", "Panel (from the top)", "Span", {"all", "1", "2", "3", "4", "5", "6", "7", "8"}),
       when(optional_number("y_min", "Y min", "Span", "Unset: from the bottom of the panel"), "panel != all"),
       when(optional_number("y_max", "Y max", "Span", "Unset: to the top of the panel"), "panel != all"),
       color("color", "Color", "Span", "#f5a524"),
       number("opacity", "Opacity (%)", "Span", 25.0, 0.0, 100.0, 5.0)});
  return s;
}

SchemaPtr build_schema() {
  auto s = std::make_shared<Schema>();
  s->kind = "figure.ideogram";
  s->title = "Ideogram";
  s->fields = {
      quantity("quantity", "Quantity", "Calculations", "age"),
      choice("mean", "Mean", "Calculations", {"weighted", "arithmetic"}),
      choice("error_kind", "Mean error", "Calculations", {"msem", "sem", "sd"}),
      boolean("j_error_in_mean", "Add J error to the mean", "Calculations", true,
              "For ages without J error: sigma(J)/J of the group added in quadrature (legacy include_j_error_in_mean)"),
      choice("probability", "Curve", "Calculations", {"cumulative", "kernel"}),
      choice("x.limits", "X limits", "Axes", {"auto", "asymptotic", "centered"}),
      when(number("x.asymptotic_percent", "Asymptotic at (% of peak)", "Axes", 10, 0.1, 50, 0.5),
           "x.limits == asymptotic"),
      when(number("x.centered_range", "Centered half-range", "Axes", 1.0, 1e-9, 1e12), "x.limits == centered"),
      optional_number("x.min", "X min", "Axes"),
      optional_number("x.max", "X max", "Axes"),
      number("x.padding_percent", "X padding (%)", "Axes", 5.0, 0.0, 50.0, 0.5),
      text("x.title", "X title", "Axes", "", "Empty: from the quantity"),
      boolean("show_mean_indicator", "Mean indicator", "Display", true),
      boolean("show_mean_text", "Mean text", "Display", true),
      boolean("show_percent_error", "Percent error", "Display", true),
      boolean("show_mswd", "MSWD", "Display", true),
      boolean("show_probability", "MSWD probability", "Display", false),
      boolean("show_n", "n", "Display", true),
      boolean("show_original_curve", "Curve with excluded analyses (dashed)", "Display", true),
      boolean("fill_curve", "Fill curve", "Display", false),
  };
  for (auto& f : common_figure_fields()) s->fields.push_back(std::move(f));
  ListSpec panels;
  panels.key = "panels";
  panels.label = "Panels (top to bottom)";
  panels.section = "Panels";
  panels.row = panel_schema();
  panels.min_rows = 1;
  panels.max_rows = 8;
  panels.default_rows_toml = {"kind = \"analysis_number\"", "kind = \"probability\"\nheight = 2.0"};
  ListSpec spans;
  spans.key = "spans";
  spans.label = "Spans (shaded ranges)";
  spans.section = "Spans";
  spans.row = span_schema();
  spans.max_rows = 32;
  s->lists = {panels, groups_list(), spans};
  s->factory_presets = {
      {"Default", ""},
      {"Probability only", "[[panels]]\nkind = \"probability\"\n"},
      {"Ages only", "[[panels]]\nkind = \"analysis_number\"\n"},
      {"With K/Ca",
       "[[panels]]\nkind = \"value\"\nquantity = \"kca\"\nscale = \"log\"\n"
       "[[panels]]\nkind = \"analysis_number\"\n[[panels]]\nkind = \"probability\"\nheight = 2.0\n"},
      {"Presentation",
       "font.title = 18\nfont.axis = 16\nfont.tick = 14\nfont.annotation = 14\nerror_bar_nsigma = 2\nfill_curve = true\n"
       "[[panels]]\nkind = \"analysis_number\"\nmarker_size = 7\n[[panels]]\nkind = \"probability\"\nheight = 2.0\n"},
  };
  return s;
}

struct Val {
  const DatasetItem* item;
  double v, e;
};

bool is_age(const Quantity& q) { return q.text() == "age"; }

}  // namespace

const SchemaPtr& ideogram_schema() {
  static const SchemaPtr s = build_schema();
  return s;
}

Result<Scene> build_ideogram(const Dataset& d, const Options& o) {
  auto q = Quantity::parse(o.get_string("quantity"));
  if (!q) return fail(q.error());
  Scene scene;
  scene.kind = "ideogram";
  apply_common_style(o, scene);
  const int nsigma = std::max<int>(1, static_cast<int>(o.get_int("error_bar_nsigma")));
  const bool hide_excluded = o.get_string("excluded_style") == "hidden";
  const auto ek = r::parse_mean_error_kind(o.get_string("error_kind")).value_or(r::MeanErrorKind::Msem);
  const bool weighted = o.get_string("mean") == "weighted";
  const Corner stats_corner = parse_corner(o.get_string("statistics_location")).value_or(Corner::TopLeft);
  MeanTextOptions mt;
  mt.sig = static_cast<int>(o.get_int("statistics_sig_figs"));
  mt.nsigma = nsigma;
  mt.percent = o.get_bool("show_percent_error");
  mt.mswd = o.get_bool("show_mswd");
  mt.probability = o.get_bool("show_probability");
  mt.n = o.get_bool("show_n");
  const auto group_rows = o.rows("groups");
  const auto panel_rows = o.rows("panels");
  const auto span_rows = o.rows("spans");
  const bool kernel = o.get_string("probability") == "kernel";
  // With no curve panel the mean text has nowhere of its own: the first
  // analysis-number panel carries it.
  const bool has_curve = std::any_of(panel_rows.begin(), panel_rows.end(),
                                     [](const Options& row) { return row.get_string("kind") == "probability"; });
  bool mean_text_placed = has_curve;

  for (int graph : d.graphs()) {
    Graph g;
    std::string title = o.get_string("title");
    const std::string gname = d.graph_name(graph);
    if (auto p = title.find("{graph}"); p != std::string::npos) title.replace(p, 7, gname);
    g.title = title.empty() ? gname : title;
    g.x.title = o.get_string("x.title").empty() ? q->label() : o.get_string("x.title");

    struct GroupData {
      int group;
      GroupStyle style;
      GroupItems items;
      std::vector<Val> all, included;
      std::optional<r::Mean> mean;
      double mean_error = 0;
    };
    std::vector<GroupData> groups;
    std::size_t missing = 0;
    for (const auto& [group, items] : d.groups_of_graph(graph)) {
      GroupData gd;
      gd.group = group;
      gd.style = group_style(d, graph, group, group_rows, MarkerShape::Circle);
      gd.items = items;
      for (const auto* it : items) {
        auto v = q->eval(*it->analysis);
        if (!v) {
          ++missing;
          continue;
        }
        gd.all.push_back({it, v->value, v->error});
        if (it->exclusion.included()) gd.included.push_back({it, v->value, v->error});
      }
      std::vector<double> vs, es;
      for (const auto& x : gd.included) {
        vs.push_back(x.v);
        es.push_back(x.e);
      }
      auto m = weighted ? r::weighted_mean(vs, es, ek) : r::arithmetic_mean(vs, es, ek);
      if (m) {
        gd.mean = *m;
        gd.mean_error = m->error;
        if (is_age(*q) && o.get_bool("j_error_in_mean"))
          gd.mean_error = with_external_error(m->value, m->error, j_relative_error(items));
      }
      groups.push_back(std::move(gd));
    }
    if (missing)
      scene.warnings.push_back(q->text() + ": " + std::to_string(missing) + " analyses have no value; not plotted");

    // X limits.
    double lo = 0, hi = 1;
    bool any = false;
    for (const auto& gd : groups)
      for (const auto& x : gd.all) {
        const double a = x.v - 2 * x.e, b = x.v + 2 * x.e;
        if (!any || a < lo) lo = a;
        if (!any || b > hi) hi = b;
        any = true;
      }
    if (any && hi <= lo) {
      lo -= 1;
      hi += 1;
    }
    auto curve_of = [&](const std::vector<Val>& vals, double a, double b) {
      std::vector<double> vs, es;
      for (const auto& x : vals) {
        vs.push_back(x.v);
        es.push_back(x.e);
      }
      return kernel ? r::kernel_density(vs, a, b) : r::cumulative_probability(vs, es, a, b);
    };
    const std::string limits = o.get_string("x.limits");
    if (limits == "centered" && !groups.empty() && groups.front().mean) {
      const double c = groups.front().mean->value, h = o.get_double("x.centered_range");
      lo = c - h;
      hi = c + h;
    } else {
      double pad = (hi - lo) * o.get_double("x.padding_percent") / 100.0;
      lo -= pad;
      hi += pad;
      if (limits == "asymptotic" && any) {
        // Grow both ends until the curves fall below the threshold (legacy
        // _calculate_asymptotic_limits: 0.5% steps, at most 200).
        const double pct = o.get_double("x.asymptotic_percent") / 100.0;
        for (int it = 0; it < 200; ++it) {
          bool grow_lo = false, grow_hi = false;
          for (const auto& gd : groups) {
            const auto c = curve_of(gd.included, lo, hi);
            if (c.y.empty()) continue;
            const double peak = *std::max_element(c.y.begin(), c.y.end());
            if (peak <= 0) continue;
            if (c.y.front() > pct * peak) grow_lo = true;
            if (c.y.back() > pct * peak) grow_hi = true;
          }
          if (!grow_lo && !grow_hi) break;
          const double step = (hi - lo) * 0.005;
          if (grow_lo) lo -= step;
          if (grow_hi) hi += step;
        }
      }
    }
    g.x.min = o.get_optional_double("x.min").value_or(lo);
    g.x.max = o.get_optional_double("x.max").value_or(hi);
    lo = *g.x.min;
    hi = *g.x.max;

    for (std::size_t pi = 0; pi < panel_rows.size(); ++pi) {
      const Options& row = panel_rows[pi];
      const std::string kind = row.get_string("kind");
      Panel p;
      p.id = "p" + std::to_string(pi);
      p.height = row.get_double("height");
      p.y.min = row.get_optional_double("y_min");
      p.y.max = row.get_optional_double("y_max");
      const auto shape = parse_marker(row.get_string("marker")).value_or(MarkerShape::Circle);
      const bool errors = row.get_bool("show_errors");

      if (kind == "probability") {
        p.quantity = "probability";
        p.y.title = kernel ? "Kernel density" : "Relative probability";
        double top = 0;
        std::vector<std::pair<const GroupData*, double>> peaks;
        std::vector<std::string> lines;
        for (const auto& gd : groups) {
          if (gd.all.empty()) continue;
          const auto c = curve_of(gd.included, lo, hi);
          double peak = c.y.empty() ? 0 : *std::max_element(c.y.begin(), c.y.end());
          if (o.get_bool("fill_curve") && !c.x.empty()) {
            BandLayer band;
            band.x = c.x;
            band.low.assign(c.x.size(), 0.0);
            band.high = c.y;
            band.fill = with_alpha(gd.style.color, static_cast<std::uint8_t>(gd.style.fill_alpha * 255 / 200));
            band.group = gd.group;
            p.layers.emplace_back(std::move(band));
          }
          if (o.get_bool("show_original_curve") && gd.all.size() != gd.included.size()) {
            const auto orig = curve_of(gd.all, lo, hi);
            LineLayer l;
            l.x = orig.x;
            l.y = orig.y;
            l.style.color = gd.style.color;
            l.style.dash = LineDash::Dash;
            l.group = gd.group;
            if (!orig.y.empty()) peak = std::max(peak, *std::max_element(orig.y.begin(), orig.y.end()));
            p.layers.emplace_back(std::move(l));
          }
          LineLayer l;
          l.x = c.x;
          l.y = c.y;
          l.style.color = gd.style.color;
          l.style.width = 1.5;
          l.label = gd.style.label;
          l.group = gd.group;
          p.layers.emplace_back(std::move(l));
          top = std::max(top, peak);
          peaks.emplace_back(&gd, peak);
          if (gd.mean && o.get_bool("show_mean_text")) {
            const std::string prefix = (gd.style.label.empty() ? "" : gd.style.label + ": ") +
                                       (weighted ? "wtd mean " : "mean ");
            lines.push_back(mean_text(prefix, gd.mean->value, gd.mean_error, &*gd.mean, gd.all.size(), mt, q->units()));
          }
        }
        // Mean indicators stacked above the curves.
        if (o.get_bool("show_mean_indicator")) {
          int k = 0;
          for (const auto& [gd, peak] : peaks) {
            if (!gd->mean) continue;
            PointLayer ind;
            ind.x = {gd->mean->value};
            ind.y = {top * (1.06 + 0.08 * k++)};
            ind.x_err = {gd->mean_error * nsigma};
            ind.refs = {PointRef{}};
            ind.excluded = {false};
            ind.tooltips = {mean_text(gd->style.label.empty() ? "mean " : gd->style.label + ": mean ", gd->mean->value,
                                      gd->mean_error, &*gd->mean, gd->all.size(), mt, q->units())};
            ind.marker.shape = MarkerShape::Diamond;
            ind.marker.size = 7;
            ind.marker.color = gd->style.color;
            ind.group = gd->group;
            p.layers.emplace_back(std::move(ind));
          }
          if (!p.y.max) p.y.max = top * (1.12 + 0.08 * std::max<int>(0, k - 1));
        }
        if (!p.y.min) p.y.min = 0.0;
        if (!lines.empty()) {
          TextLayer t;
          t.lines = std::move(lines);
          t.corner = stats_corner;
          t.font_size = scene.style.fonts.annotation;
          p.layers.emplace_back(std::move(t));
        }
      } else if (kind == "analysis_number" || kind == "analysis_number_nonsorted") {
        p.quantity = kind;
        p.y.title = "Analysis #";
        int index = 1;
        for (const auto& gd : groups) {
          std::vector<Val> vals = gd.all;
          if (kind == "analysis_number")
            std::stable_sort(vals.begin(), vals.end(), [](const Val& a, const Val& b) { return a.v < b.v; });
          else
            std::stable_sort(vals.begin(), vals.end(), [](const Val& a, const Val& b) {
              return a.item->analysis->analysis->timestamp < b.item->analysis->analysis->timestamp;
            });
          PointLayer pts;
          pts.group = gd.group;
          pts.marker.shape = gd.style.marker == MarkerShape::Circle ? shape : gd.style.marker;
          pts.marker.size = row.get_double("marker_size");
          pts.marker.color = gd.style.color;
          pts.excluded_marker = pts.marker;
          pts.excluded_marker.filled = false;
          pts.excluded_marker.color = Color{150, 150, 150, 255};
          pts.show_excluded = !hide_excluded;
          for (const auto& v : vals) {
            pts.x.push_back(v.v);
            pts.y.push_back(index++);
            if (errors) pts.x_err.push_back(v.e * nsigma);
            pts.refs.push_back({v.item->analysis->analysis->uuid});
            pts.excluded.push_back(v.item->exclusion.excluded());
            const auto& a = *v.item->analysis->analysis;
            pts.tooltips.push_back(a.runid + (a.tag != "ok" ? "  [" + a.tag + "]" : "") +
                                   (v.item->exclusion.excluded() ? "  excluded" : "") + "\n" + q->label() + " " +
                                   format_sig(v.v, mt.sig + 2) + " ± " + format_sig(v.e, 3));
          }
          p.layers.emplace_back(std::move(pts));
        }
        if (!p.y.min) p.y.min = 0.0;
        if (!p.y.max) p.y.max = index;
        if (!mean_text_placed && o.get_bool("show_mean_text")) {
          mean_text_placed = true;
          TextLayer t;
          for (const auto& gd : groups) {
            if (!gd.mean) continue;
            const std::string prefix = (gd.style.label.empty() ? "" : gd.style.label + ": ") +
                                       (weighted ? "wtd mean " : "mean ");
            t.lines.push_back(mean_text(prefix, gd.mean->value, gd.mean_error, &*gd.mean, gd.all.size(), mt, q->units()));
          }
          t.corner = stats_corner;
          t.font_size = scene.style.fonts.annotation;
          if (!t.lines.empty()) p.layers.emplace_back(std::move(t));
        }
      } else {  // value
        auto vq = Quantity::parse(row.get_string("quantity"));
        if (!vq) {
          scene.warnings.push_back("panel " + std::to_string(pi + 1) + ": " + vq.error().what);
          continue;
        }
        p.quantity = vq->text();
        p.y.title = vq->label();
        p.y.scale = row.get_string("scale") == "log" ? AxisScale::Log : AxisScale::Linear;
        for (const auto& gd : groups) {
          PointLayer pts;
          pts.group = gd.group;
          pts.marker.shape = gd.style.marker == MarkerShape::Circle ? shape : gd.style.marker;
          pts.marker.size = row.get_double("marker_size");
          pts.marker.color = gd.style.color;
          pts.excluded_marker = pts.marker;
          pts.excluded_marker.filled = false;
          pts.excluded_marker.color = Color{150, 150, 150, 255};
          pts.show_excluded = !hide_excluded;
          for (const auto& v : gd.all) {
            auto y = vq->eval(*v.item->analysis);
            if (!y) continue;
            pts.x.push_back(v.v);
            pts.y.push_back(y->value);
            if (errors) {
              pts.x_err.push_back(v.e * nsigma);
              pts.y_err.push_back(y->error * nsigma);
            }
            pts.refs.push_back({v.item->analysis->analysis->uuid});
            pts.excluded.push_back(v.item->exclusion.excluded());
            pts.tooltips.push_back(v.item->analysis->analysis->runid + "\n" + vq->label() + " " +
                                   format_sig(y->value, mt.sig + 2) + " ± " + format_sig(y->error, 3));
          }
          p.layers.emplace_back(std::move(pts));
        }
      }
      // Spans go under everything else in the panel.
      std::vector<Layer> under;
      for (const Options& sr : span_rows) {
        const std::string where = sr.get_string("panel");
        const std::size_t on = where == "all" ? 0 : static_cast<std::size_t>(where.front() - '0');  // 0: every panel
        if (on != 0 && on != pi + 1) continue;
        SpanLayer span;
        span.x0 = sr.get_optional_double("min");
        span.x1 = sr.get_optional_double("max");
        if (span.x0 && span.x1 && *span.x0 > *span.x1) std::swap(span.x0, span.x1);
        if (on != 0) {  // y bounds mean something on one panel only
          span.y0 = sr.get_optional_double("y_min");
          span.y1 = sr.get_optional_double("y_max");
          if (span.y0 && span.y1 && *span.y0 > *span.y1) std::swap(span.y0, span.y1);
        }
        const Color base = parse_color(sr.get_string("color")).value_or(Color{245, 165, 36, 255});
        span.fill = with_alpha(base, static_cast<std::uint8_t>(std::lround(base.a * sr.get_double("opacity") / 100.0)));
        // One label per span: on the panel it is on, or on the top one.
        if (on != 0 || g.panels.empty()) span.label = sr.get_string("label");
        under.emplace_back(std::move(span));
      }
      p.layers.insert(p.layers.begin(), std::make_move_iterator(under.begin()), std::make_move_iterator(under.end()));
      g.panels.push_back(std::move(p));
    }
    scene.graphs.push_back(std::move(g));
  }
  if (scene.graphs.empty()) scene.warnings.push_back("no analyses");
  return scene;
}

namespace {

class IdeogramUnit final : public Unit {
 public:
  std::string_view kind() const override { return "ideogram"; }
  std::string_view title() const override { return "Ideogram"; }
  const SchemaPtr& schema() const override { return ideogram_schema(); }
  std::vector<PortSpec> inputs() const override { return {{"analyses", PortType::Dataset}}; }
  std::vector<PortSpec> outputs() const override { return {{"figure", PortType::Scene}}; }
  Result<std::vector<PortValue>> execute(const std::vector<PortValue>& in, const Options& o,
                                         RunContext&) const override {
    auto scene = build_ideogram(*std::get<DatasetPtr>(in.at(0)), o);
    if (!scene) return fail(scene.error());
    return std::vector<PortValue>{PortValue(std::make_shared<const Scene>(std::move(*scene)))};
  }
};

}  // namespace

std::unique_ptr<Unit> make_ideogram_unit() { return std::make_unique<IdeogramUnit>(); }

}  // namespace pychron::processing
