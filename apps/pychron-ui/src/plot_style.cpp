#include "plot_style.hpp"

#include <pychron/processing/scene.hpp>

namespace pychron::ui {

PlotStyle default_plot_style() {
  PlotStyle style;
  const auto palette = processing::default_palette();
  style.series.reserve(palette.size());
  for (const auto& c : palette) style.series.emplace_back(c.r, c.g, c.b, c.a);
  // White, as a figure's plot_background: the one plot colour named here.
  style.plot_background = QColor(255, 255, 255);
  return style;
}

const PlotStyle& plot_style() {
  static const PlotStyle current = default_plot_style();
  return current;
}

QColor plot_series_color(std::size_t slot) {
  const auto& series = plot_style().series;
  return series[slot % series.size()];
}

}  // namespace pychron::ui
