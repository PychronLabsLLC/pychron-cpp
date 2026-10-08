#include "figure_common.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>

#include "schema_builder.hpp"

namespace pychron::processing::detail {

namespace {

const std::vector<std::string> kCorners{"top_left", "top_right", "bottom_left", "bottom_right"};

}  // namespace

std::string format_sig(double v, int sig) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*g", std::max(1, sig), v);
  return buf;
}

std::string format_utc(double t) {
  const auto tt = static_cast<std::time_t>(std::llround(t));
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
  return std::string(buf) + "Z";
}

Color with_alpha(Color c, std::uint8_t a) {
  c.a = a;
  return c;
}

std::vector<FieldSpec> common_figure_fields() {
  return {
      text("title", "Title", "Layout", "", "Placeholders: {graph}"),
      integer("graph_columns", "Graphs per row", "Layout", 1, 1, 6),
      integer("panel_spacing", "Panel spacing (px)", "Layout", 4, 0, 40),
      font("font.family", "Font", "Appearance", ""),
      number("font.title", "Title size", "Appearance", 12, 4, 72, 1),
      number("font.axis", "Axis title size", "Appearance", 10, 4, 72, 1),
      number("font.tick", "Tick label size", "Appearance", 9, 4, 72, 1),
      number("font.annotation", "Annotation size", "Appearance", 9, 4, 72, 1),
      color("background", "Background", "Appearance", "#ffffff"),
      color("plot_background", "Plot background", "Appearance", "#ffffff"),
      boolean("show_grid", "Grid", "Appearance", true),
      integer("error_bar_nsigma", "Error bars (sigma)", "Appearance", 1, 0, 3),
      choice("excluded_style", "Excluded analyses", "Appearance", {"ghost", "hidden"}),
      boolean("show_legend", "Legend", "Legend", true),
      choice("legend_location", "Legend location", "Legend", kCorners, "top_right"),
      choice("statistics_location", "Statistics location", "Statistics", kCorners, "top_left"),
      integer("statistics_sig_figs", "Significant figures", "Statistics", 4, 1, 10),
  };
}

void apply_common_style(const Options& o, Scene& scene) {
  scene.columns = static_cast<int>(o.get_int("graph_columns"));
  scene.style.background = parse_color(o.get_string("background")).value_or(Color{255, 255, 255, 255});
  scene.style.plot_background = parse_color(o.get_string("plot_background")).value_or(Color{255, 255, 255, 255});
  scene.style.grid = o.get_bool("show_grid");
  scene.style.fonts.family = o.get_string("font.family");
  scene.style.fonts.title = o.get_double("font.title");
  scene.style.fonts.axis_title = o.get_double("font.axis");
  scene.style.fonts.tick = o.get_double("font.tick");
  scene.style.fonts.annotation = o.get_double("font.annotation");
  scene.style.legend = o.get_bool("show_legend");
  scene.style.legend_corner = parse_corner(o.get_string("legend_location")).value_or(Corner::TopRight);
  scene.style.panel_spacing = static_cast<int>(o.get_int("panel_spacing"));
}

SchemaPtr group_row_schema(bool fixed_steps) {
  auto fields = [](bool fixed) {
    std::vector<FieldSpec> f{
        optional_color("color", "Colour", "Group"),
        choice("marker", "Marker", "Group", {"auto", "circle", "square", "diamond", "triangle", "cross", "plus", "star"}),
        text("label", "Legend label", "Group"), integer("fill_alpha", "Fill opacity (%)", "Group", 70, 0, 100)};
    if (fixed) {
      f.push_back(text("fixed_start", "Plateau from step", "Group", "", "Step letter; empty: search for a plateau"));
      f.push_back(text("fixed_end", "Plateau to step", "Group", "", "Step letter; empty: the last step"));
    }
    return f;
  };
  static const SchemaPtr plain = make_schema("figure.group", "Group", fields(false));
  static const SchemaPtr fixed = make_schema("figure.group.fixed_steps", "Group", fields(true));
  return fixed_steps ? fixed : plain;
}

ListSpec groups_list(bool fixed_steps) {
  ListSpec groups;
  groups.key = "groups";
  groups.label = "Groups";
  groups.section = "Groups";
  groups.row = group_row_schema(fixed_steps);
  groups.max_rows = 32;
  return groups;
}

GroupStyle group_style(const Dataset& d, int graph, int group, const std::vector<Options>& rows,
                       MarkerShape default_marker) {
  GroupStyle st;
  st.color = palette_color(group);
  st.marker = default_marker;
  const Options* row = group >= 0 && static_cast<std::size_t>(group) < rows.size() ? &rows[group] : nullptr;
  if (row) {
    if (auto c = parse_color(row->get_string("color"))) st.color = *c;
    if (row->get_string("marker") != "auto") st.marker = parse_marker(row->get_string("marker")).value_or(default_marker);
    if (row->schema() && row->schema()->field("fill_alpha")) st.fill_alpha = static_cast<int>(row->get_int("fill_alpha"));
  }
  const bool named = static_cast<std::size_t>(group) < d.group_names.size() && !d.group_names[group].empty();
  if (row && !row->get_string("label").empty())
    st.label = row->get_string("label");
  else if (named || d.groups_of_graph(graph).size() > 1)
    st.label = d.group_name(group);
  return st;
}

std::string mean_text(const std::string& prefix, double value, double error, const reduction::Mean* stats,
                      std::size_t n_total, const MeanTextOptions& t, const std::string& units) {
  const int ns = std::max(1, t.nsigma);
  std::string s = prefix + format_sig(value, t.sig) + " ± " + format_sig(error * ns, 2);
  if (!units.empty()) s += " " + units;
  if (ns > 1) s += " (" + std::to_string(ns) + "σ)";
  if (t.percent && value != 0) s += " (" + format_sig(std::abs(error * ns / value) * 100.0, 2) + "%)";
  if (stats && stats->n > 1) {
    if (t.mswd) s += "  MSWD " + format_sig(stats->mswd, 3) + (stats->mswd_acceptable ? "" : "*");
    if (t.probability)
      s += "  p " + format_sig(reduction::mswd_probability(stats->mswd, static_cast<int>(stats->n) - 1), 2);
  }
  if (t.n && stats) s += "  n " + std::to_string(stats->n) + "/" + std::to_string(n_total);
  return s;
}

}  // namespace pychron::processing::detail
