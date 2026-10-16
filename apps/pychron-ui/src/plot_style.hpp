#pragma once

// PlotStyle: series colours and background of the live plots (strip chart,
// evolutions, peak center, gauge history, pattern maker, holder), and of
// nothing else. A figure has its own colours in its options (pp::SceneStyle).
// By default both draw from the one palette, processing::default_palette(),
// on white, so series n of a live plot and group n of a figure look alike.
// A colour configured for one detector wins over the palette.

#include <cstddef>
#include <vector>

#include <QColor>

namespace pychron::ui {

struct PlotStyle {               // live plots only
  std::vector<QColor> series;    // never empty; default: default_palette()
  QColor plot_background;        // default #ffffff
  bool operator==(const PlotStyle&) const = default;
};

[[nodiscard]] PlotStyle default_plot_style();
// The current style. GUI thread.
[[nodiscard]] const PlotStyle& plot_style();
// plot_style().series[slot % size]: the live palette, cycled.
[[nodiscard]] QColor plot_series_color(std::size_t slot);

}  // namespace pychron::ui
