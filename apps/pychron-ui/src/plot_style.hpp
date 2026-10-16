#pragma once

// PlotStyle: series colours and background of the live plots (strip chart,
// evolutions, peak center, gauge history, pattern maker, holder), and of
// nothing else. A figure has its own colours in its options (pp::SceneStyle).
// By default both draw from the one palette, processing::default_palette(),
// on white, so series n of a live plot and group n of a figure look alike.
// A colour configured for one detector wins over the palette.

#include <cstddef>
#include <string>
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

struct Preferences;

// The style kept in `preferences`: the default where nothing (valid) is kept.
[[nodiscard]] PlotStyle plot_style_from(const Preferences& preferences);
// Writes into `preferences` only what differs from the default, so a later
// change of the default reaches a colour the user never changed.
void store_plot_style(const PlotStyle& style, Preferences& preferences);
// Makes `style` the current one. Series short of 12 (or none) are filled from
// the default; an invalid background reads as the default. GUI thread.
void set_plot_style(PlotStyle style);

}  // namespace pychron::ui
