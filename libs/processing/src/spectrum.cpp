// Age spectrum (legacy pipeline/plot/plotter/spectrum.py, options/spectrum.py).

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>

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

SchemaPtr panel_schema() {
  static const SchemaPtr s = make_schema(
      "figure.spectrum.panel", "Panel",
      {choice("kind", "Panel", "Panel", {"age_spectrum", "value"}),
       when(quantity("quantity", "Quantity (value spectra)", "Panel", "kca"), "kind == value"),
       number("height", "Height", "Panel", 1.0, 0.1, 10.0, 0.1),
       when(choice("scale", "Scale", "Panel", {"linear", "log"}), "kind == value"),
       optional_number("y_min", "Y min", "Panel"), optional_number("y_max", "Y max", "Panel")});
  return s;
}

SchemaPtr build_schema() {
  auto s = std::make_shared<Schema>();
  s->kind = "figure.spectrum";
  s->title = "Age spectrum";
  s->fields = {
      quantity("quantity", "Step value", "Calculations", "age"),
      quantity("gas", "Step width", "Calculations", "k39"),
      choice("plateau.method", "Plateau criterion", "Plateau", {"fleck", "mahon"}),
      integer("plateau.nsteps", "Minimum steps", "Plateau", 3, 2, 100),
      number("plateau.gas_fraction", "Minimum gas (%)", "Plateau", 50.0, 0.0, 100.0, 1.0),
      when(number("plateau.overlap_sigma", "Overlap at (sigma)", "Plateau", 2.0, 0.5, 5.0, 0.5),
           "plateau.method == fleck"),
      choice("plateau.weighting", "Plateau mean", "Plateau", {"inverse_variance", "volume_fraction"}),
      choice("plateau.error_kind", "Plateau error", "Plateau", {"msem", "sem", "sd"}),
      boolean("plateau.j_error", "Add J error to the plateau age", "Plateau", true),
      boolean("integrated.include_excluded", "Integrated age uses excluded steps", "Plateau", true),
      boolean("integrated.j_error", "Integrated age with J error", "Plateau", false),
      integer("step_nsigma", "Step boxes (sigma)", "Display", 2, 1, 3),
      boolean("show_plateau", "Plateau bar", "Display", true),
      boolean("show_plateau_text", "Plateau text", "Display", true),
      boolean("show_integrated_text", "Integrated age text", "Display", true),
      boolean("show_weighted_mean_text", "Weighted mean when no plateau", "Display", true),
      boolean("show_step_labels", "Step labels", "Display", false),
      boolean("dim_non_plateau", "Dim steps outside the plateau", "Display", false),
      boolean("show_percent_error", "Percent error", "Display", true),
      boolean("show_mswd", "MSWD", "Display", true),
      number("y.ignore_gas_percent", "Y range ignores steps below (% gas)", "Axes", 1.0, 0.0, 50.0, 0.5),
      text("x.title", "X title", "Axes", "", "Empty: Cumulative % of the step width quantity"),
  };
  for (auto& f : common_figure_fields()) s->fields.push_back(std::move(f));
  ListSpec panels;
  panels.key = "panels";
  panels.label = "Panels (top to bottom)";
  panels.section = "Panels";
  panels.row = panel_schema();
  panels.min_rows = 1;
  panels.max_rows = 6;
  panels.default_rows_toml = {"kind = \"age_spectrum\""};
  s->lists = {panels, groups_list(true)};
  s->factory_presets = {
      {"Default", ""},
      {"With K/Ca",
       "[[panels]]\nkind = \"value\"\nquantity = \"kca\"\nscale = \"log\"\n"
       "[[panels]]\nkind = \"age_spectrum\"\nheight = 2.5\n"},
      {"With %40Ar*",
       "[[panels]]\nkind = \"value\"\nquantity = \"radiogenic_yield\"\n"
       "[[panels]]\nkind = \"age_spectrum\"\nheight = 2.5\n"},
      {"Mahon", "plateau.method = \"mahon\"\n"},
  };
  return s;
}

struct Step {
  const DatasetItem* item;
  double v, e, gas;
  bool excluded;
};

bool is_age(const Quantity& q) { return q.text() == "age" || q.text() == "age_w_j"; }

}  // namespace

const SchemaPtr& spectrum_schema() {
  static const SchemaPtr s = build_schema();
  return s;
}

Result<Scene> build_spectrum(const Dataset& d, const Options& o) {
  auto q = Quantity::parse(o.get_string("quantity"));
  if (!q) return fail(q.error());
  auto gq = Quantity::parse(o.get_string("gas"));
  if (!gq) return fail(gq.error());
  Scene scene;
  scene.kind = "spectrum";
  apply_common_style(o, scene);
  const int step_ns = static_cast<int>(o.get_int("step_nsigma"));
  const int nsigma = std::max<int>(1, static_cast<int>(o.get_int("error_bar_nsigma")));
  const Corner stats_corner = parse_corner(o.get_string("statistics_location")).value_or(Corner::TopLeft);
  MeanTextOptions mt;
  mt.sig = static_cast<int>(o.get_int("statistics_sig_figs"));
  mt.nsigma = nsigma;
  mt.percent = o.get_bool("show_percent_error");
  mt.mswd = o.get_bool("show_mswd");
  r::PlateauCriteria crit;
  crit.method = o.get_string("plateau.method") == "mahon" ? r::PlateauMethod::Mahon : r::PlateauMethod::Fleck;
  crit.nsteps = static_cast<int>(o.get_int("plateau.nsteps"));
  crit.gas_fraction = o.get_double("plateau.gas_fraction");
  crit.overlap_sigma = o.get_double("plateau.overlap_sigma");
  const auto weighting = o.get_string("plateau.weighting") == "volume_fraction" ? r::PlateauWeighting::VolumeFraction
                                                                              : r::PlateauWeighting::InverseVariance;
  const auto pek = r::parse_mean_error_kind(o.get_string("plateau.error_kind")).value_or(r::MeanErrorKind::Msem);
  const auto group_rows = o.rows("groups");
  const auto panel_rows = o.rows("panels");
  const double ignore_pct = o.get_double("y.ignore_gas_percent");

  for (int graph : d.graphs()) {
    Graph g;
    std::string title = o.get_string("title");
    const std::string gname = d.graph_name(graph);
    if (auto p = title.find("{graph}"); p != std::string::npos) title.replace(p, 7, gname);
    g.title = title.empty() ? gname : title;
    g.x.title = o.get_string("x.title").empty() ? "Cumulative % " + gq->label(false) : o.get_string("x.title");
    g.x.min = 0.0;
    g.x.max = 100.0;

    struct GroupData {
      int group;
      GroupStyle style;
      GroupItems items;
      std::vector<Step> steps;   // step order
      std::vector<double> x0, x1;
      std::optional<r::StepRange> plateau;
      std::optional<r::PlateauMean> plateau_mean;
      double plateau_error = 0;
      std::vector<std::string> lines;
    };
    std::vector<GroupData> groups;
    std::size_t missing = 0;
    for (const auto& [group, items] : d.groups_of_graph(graph)) {
      GroupData gd;
      gd.group = group;
      gd.style = group_style(d, graph, group, group_rows, MarkerShape::Circle);
      gd.items = items;
      std::vector<const DatasetItem*> ordered = items;
      // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order): ordered by increment and time, not by address
      std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        const auto& x = *a->analysis->analysis;
        const auto& y = *b->analysis->analysis;
        if (x.increment != y.increment) return x.increment < y.increment;
        return x.timestamp < y.timestamp;
      });
      for (const auto* it : ordered) {
        auto v = q->eval(*it->analysis);
        auto w = gq->eval(*it->analysis);
        if (!v || !w || !(w->value > 0)) {
          ++missing;
          continue;
        }
        gd.steps.push_back({it, v->value, v->error, w->value, it->exclusion.excluded()});
      }
      double total = 0;
      for (const auto& s : gd.steps) total += s.gas;
      double cum = 0;
      for (const auto& s : gd.steps) {
        gd.x0.push_back(total > 0 ? cum / total * 100.0 : 0.0);
        cum += s.gas;
        gd.x1.push_back(total > 0 ? cum / total * 100.0 : 0.0);
      }

      // Plateau: fixed steps from the group row, else the search.
      // std::vector<bool> cannot back a span<const bool>.
      const std::size_t n = gd.steps.size();
      std::vector<double> vs, es, ws;
      std::unique_ptr<bool[]> ex(new bool[std::max<std::size_t>(1, n)]);
      for (std::size_t i = 0; i < n; ++i) {
        vs.push_back(gd.steps[i].v);
        es.push_back(gd.steps[i].e);
        ws.push_back(gd.steps[i].gas);
        ex[i] = gd.steps[i].excluded;
      }
      const std::span<const bool> excluded(ex.get(), n);
      const Options* grow = group >= 0 && static_cast<std::size_t>(group) < group_rows.size() ? &group_rows[group] : nullptr;
      const std::string fs = grow ? grow->get_string("fixed_start") : std::string();
      const std::string fe = grow ? grow->get_string("fixed_end") : std::string();
      if (!gd.steps.empty() && (!fs.empty() || !fe.empty())) {
        auto index_of = [&](const std::string& letters) -> std::optional<std::size_t> {
          for (std::size_t i = 0; i < gd.steps.size(); ++i) {
            std::string st = gd.steps[i].item->analysis->analysis->step();
            if (st == letters) return i;
          }
          return std::nullopt;
        };
        const auto a = fs.empty() ? std::optional<std::size_t>(0) : index_of(fs);
        const auto b = fe.empty() ? std::optional<std::size_t>(gd.steps.size() - 1) : index_of(fe);
        if (a && b)
          gd.plateau = r::StepRange{std::min(*a, *b), std::max(*a, *b)};
        else
          scene.warnings.push_back(gd.style.label + ": fixed plateau steps not found");
      } else if (!gd.steps.empty()) {
        gd.plateau = r::find_plateau(vs, es, ws, excluded, crit);
      }
      if (gd.plateau) {
        auto pm = r::plateau_mean(vs, es, ws, excluded, *gd.plateau, weighting, pek);
        if (pm) {
          gd.plateau_mean = *pm;
          gd.plateau_error = pm->mean.error;
          if (q->text() == "age" && o.get_bool("plateau.j_error"))
            gd.plateau_error = with_external_error(pm->mean.value, pm->mean.error, j_relative_error(items));
        } else {
          gd.plateau.reset();
        }
      }
      const std::string label = gd.style.label.empty() ? "" : gd.style.label + ": ";
      if (gd.plateau_mean && o.get_bool("show_plateau_text")) {
        const auto& first = *gd.steps[gd.plateau->first].item->analysis->analysis;
        const auto& last = *gd.steps[gd.plateau->last].item->analysis->analysis;
        std::string line = mean_text(label + "plateau " + first.step() + "-" + last.step() + " ", gd.plateau_mean->mean.value,
                                     gd.plateau_error, &gd.plateau_mean->mean, gd.steps.size(), mt, q->units());
        line += "  " + format_sig(gd.plateau_mean->gas_fraction, 3) + "% gas";
        gd.lines.push_back(line);
      } else if (!gd.plateau_mean && o.get_bool("show_weighted_mean_text") && !gd.steps.empty()) {
        std::vector<double> iv, ie;
        for (const auto& s : gd.steps)
          if (!s.excluded) {
            iv.push_back(s.v);
            ie.push_back(s.e);
          }
        if (auto m = r::weighted_mean(iv, ie, pek))
          gd.lines.push_back(mean_text(label + "no plateau; wtd mean ", m->value, m->error, &*m, gd.steps.size(), mt,
                                       q->units()));
      }
      if (is_age(*q) && o.get_bool("show_integrated_text") && !gd.steps.empty()) {
        GroupItems integrated;
        for (const auto& s : gd.steps)
          if (o.get_bool("integrated.include_excluded") || !s.excluded) integrated.push_back(s.item);
        if (auto ia = integrated_age(integrated, o.get_bool("integrated.j_error"))) {
          MeanTextOptions it = mt;
          it.mswd = false;
          it.n = false;
          gd.lines.push_back(mean_text(label + "integrated ", ia->nominal(), ia->std_dev(), nullptr, 0, it, q->units()));
        }
      }
      groups.push_back(std::move(gd));
    }
    if (missing)
      scene.warnings.push_back(std::to_string(missing) + " analyses have no " + q->text() + " or no positive " + gq->text() +
                               "; not plotted");

    for (std::size_t pi = 0; pi < panel_rows.size(); ++pi) {
      const Options& row = panel_rows[pi];
      const bool age_panel = row.get_string("kind") == "age_spectrum";
      std::optional<Quantity> vq;
      if (!age_panel) {
        auto parsed = Quantity::parse(row.get_string("quantity"));
        if (!parsed) {
          scene.warnings.push_back("panel " + std::to_string(pi + 1) + ": " + parsed.error().what);
          continue;
        }
        vq = *parsed;
      }
      Panel p;
      p.id = "p" + std::to_string(pi);
      p.quantity = age_panel ? q->text() : vq->text();
      p.height = row.get_double("height");
      p.y.title = age_panel ? q->label() : vq->label();
      p.y.scale = !age_panel && row.get_string("scale") == "log" ? AxisScale::Log : AxisScale::Linear;
      double ylo = 0, yhi = 0;
      bool yany = false;
      std::vector<std::string> lines;
      for (const auto& gd : groups) {
        StepLayer steps;
        steps.group = gd.group;
        steps.fill = with_alpha(gd.style.color, static_cast<std::uint8_t>(gd.style.fill_alpha * 255 / 200));
        steps.line = gd.style.color;
        steps.dim_others = age_panel && o.get_bool("dim_non_plateau") && gd.plateau.has_value();
        steps.label = gd.style.label;
        LineLayer center;
        center.style.color = gd.style.color;
        center.style.width = 1.0;
        center.group = gd.group;
        for (std::size_t i = 0; i < gd.steps.size(); ++i) {
          const auto& s = gd.steps[i];
          double v = s.v, e = s.e;
          if (!age_panel) {
            auto y = vq->eval(*s.item->analysis);
            if (!y) continue;
            v = y->value;
            e = y->error;
          }
          const auto& a = *s.item->analysis->analysis;
          steps.x0.push_back(gd.x0[i]);
          steps.x1.push_back(gd.x1[i]);
          steps.y.push_back(v);
          steps.y_err.push_back(e * step_ns);
          steps.refs.push_back({a.uuid});
          steps.excluded.push_back(s.excluded);
          steps.highlighted.push_back(age_panel && gd.plateau && i >= gd.plateau->first && i <= gd.plateau->last &&
                                      !s.excluded);
          steps.labels.push_back(o.get_bool("show_step_labels") ? a.step() : std::string());
          steps.tooltips.push_back(a.runid + (s.excluded ? "  excluded" : "") + "\n" + p.y.title + " " +
                                   format_sig(v, mt.sig + 2) + " ± " + format_sig(e, 3) + "\n" +
                                   format_sig(gd.x1[i] - gd.x0[i], 3) + "% gas");
          center.x.insert(center.x.end(), {gd.x0[i], gd.x1[i]});
          center.y.insert(center.y.end(), {v, v});
          if (gd.x1[i] - gd.x0[i] >= ignore_pct) {
            const double a0 = v - e * step_ns, b0 = v + e * step_ns;
            if (!yany || a0 < ylo) ylo = a0;
            if (!yany || b0 > yhi) yhi = b0;
            yany = true;
          }
        }
        p.layers.emplace_back(std::move(steps));
        p.layers.emplace_back(std::move(center));
        if (age_panel && gd.plateau_mean && o.get_bool("show_plateau")) {
          LineLayer bar;
          bar.x = {gd.x0[gd.plateau->first], gd.x1[gd.plateau->last]};
          bar.y = {gd.plateau_mean->mean.value, gd.plateau_mean->mean.value};
          bar.style.color = gd.style.color;
          bar.style.width = 3.0;
          bar.group = gd.group;
          p.layers.emplace_back(std::move(bar));
        }
        if (age_panel) lines.insert(lines.end(), gd.lines.begin(), gd.lines.end());
      }
      if (yany) {
        double pad = (yhi - ylo) * 0.1;
        if (pad <= 0) pad = std::max(std::abs(yhi) * 0.05, 1e-9);
        if (p.y.scale == AxisScale::Linear) {
          p.y.min = ylo - pad;
          p.y.max = yhi + pad * (lines.empty() ? 1.0 : 2.5);
        }
      }
      if (row.get_optional_double("y_min")) p.y.min = row.get_optional_double("y_min");
      if (row.get_optional_double("y_max")) p.y.max = row.get_optional_double("y_max");
      if (!lines.empty()) {
        TextLayer t;
        t.lines = std::move(lines);
        t.corner = stats_corner;
        t.font_size = scene.style.fonts.annotation;
        p.layers.emplace_back(std::move(t));
      }
      g.panels.push_back(std::move(p));
    }
    scene.graphs.push_back(std::move(g));
  }
  if (scene.graphs.empty()) scene.warnings.push_back("no analyses");
  return scene;
}

namespace {

class SpectrumUnit final : public Unit {
 public:
  std::string_view kind() const override { return "spectrum"; }
  std::string_view title() const override { return "Age spectrum"; }
  const SchemaPtr& schema() const override { return spectrum_schema(); }
  std::vector<PortSpec> inputs() const override { return {{"analyses", PortType::Dataset}}; }
  std::vector<PortSpec> outputs() const override { return {{"figure", PortType::Scene}}; }
  Result<std::vector<PortValue>> execute(const std::vector<PortValue>& in, const Options& o,
                                         RunContext&) const override {
    auto scene = build_spectrum(*std::get<DatasetPtr>(in.at(0)), o);
    if (!scene) return fail(scene.error());
    return std::vector<PortValue>{PortValue(std::make_shared<const Scene>(std::move(*scene)))};
  }
};

}  // namespace

std::unique_ptr<Unit> make_spectrum_unit() { return std::make_unique<SpectrumUnit>(); }

}  // namespace pychron::processing
