#pragma once

// What every figure kind shares: appearance, legend, layout and statistics
// fields with the same keys and defaults, scene styling from them, the group
// rows table and per-group styling, and number formatting.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "pychron/processing/dataset.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/scene.hpp"
#include "pychron/reduction/stats.hpp"

namespace pychron::processing::detail {

std::string format_sig(double v, int sig);
std::string format_utc(double t);
Color with_alpha(Color c, std::uint8_t a);

// title, graph_columns, panel_spacing, font.*, background, plot_background,
// show_grid, error_bar_nsigma, excluded_style, show_legend, legend_location,
// statistics_location, statistics_sig_figs.
std::vector<FieldSpec> common_figure_fields();
void apply_common_style(const Options& o, Scene& scene);

// Group rows: colour, marker, legend label, fill opacity; with
// `fixed_steps` also fixed_start / fixed_end step letters (spectra).
SchemaPtr group_row_schema(bool fixed_steps = false);
ListSpec groups_list(bool fixed_steps = false);

struct GroupStyle {
  Color color;
  MarkerShape marker = MarkerShape::Circle;
  std::string label;  // empty: no legend entry
  int fill_alpha = 70;  // percent
};
// Row `group` of the groups table overrides the palette; an ungrouped figure
// (one unnamed group in the graph) has no label.
GroupStyle group_style(const Dataset& d, int graph, int group, const std::vector<Options>& rows,
                       MarkerShape default_marker);

// "value ± error" with the error at nsigma, optional percent, MSWD (marked *
// when outside the Mahon limits), probability and n/total.
struct MeanTextOptions {
  int sig = 4;
  int nsigma = 1;
  bool percent = true;
  bool mswd = true;
  bool probability = false;
  bool n = true;
};
// The inverse isochron (arar_figures.hpp). With `plateau_steps` (analysis
// uuids), "exclude_non_plateau" leaves out every step not among them, in
// place of the plateau the isochron would look for itself.
Result<Scene> build_isochron_scene(const Dataset& dataset, const Options& options,
                                   const std::set<std::string>* plateau_steps);

std::string mean_text(const std::string& prefix, double value, double error, const reduction::Mean* stats,
                      std::size_t n_total, const MeanTextOptions& t, const std::string& units = {});

}  // namespace pychron::processing::detail
