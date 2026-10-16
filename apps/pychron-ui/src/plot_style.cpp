#include "plot_style.hpp"

#include <algorithm>
#include <utility>

#include <pychron/processing/scene.hpp>

#include "preferences.hpp"

namespace pychron::ui {

namespace {

PlotStyle& current_style() {
  static PlotStyle current = default_plot_style();
  return current;
}

// A kept colour: valid "#rrggbb" (any case) or nothing.
QColor parse_color(const std::string& text) {
  const QString s = QString::fromStdString(text).trimmed();
  if (s.size() != 7 || !s.startsWith(QLatin1Char('#'))) return {};
  return QColor::fromString(s);
}

std::string color_text(const QColor& c) { return c.name(QColor::HexRgb).toStdString(); }

}  // namespace

PlotStyle default_plot_style() {
  PlotStyle style;
  const auto palette = processing::default_palette();
  style.series.reserve(palette.size());
  for (const auto& c : palette) style.series.emplace_back(c.r, c.g, c.b, c.a);
  // White, as a figure's plot_background: the one plot colour named here.
  style.plot_background = QColor(255, 255, 255);
  return style;
}

const PlotStyle& plot_style() { return current_style(); }

QColor plot_series_color(std::size_t slot) {
  const auto& series = plot_style().series;
  return series[slot % series.size()];
}

}  // namespace pychron::ui

namespace pychron::ui {

PlotStyle plot_style_from(const Preferences& preferences) {
  PlotStyle style = default_plot_style();
  if (const QColor bg = parse_color(preferences.plot_background); bg.isValid()) style.plot_background = bg;
  for (const auto& [slot, text] : preferences.plot_series_colors) {
    if (slot < 1 || std::cmp_greater(slot, style.series.size())) continue;
    if (const QColor c = parse_color(text); c.isValid()) style.series[static_cast<std::size_t>(slot - 1)] = c;
  }
  return style;
}

void store_plot_style(const PlotStyle& style, Preferences& preferences) {
  const PlotStyle defaults = default_plot_style();
  preferences.plot_background.clear();
  preferences.plot_series_colors.clear();
  if (style.plot_background.isValid() && style.plot_background.rgb() != defaults.plot_background.rgb())
    preferences.plot_background = color_text(style.plot_background);
  const std::size_t n = std::min(style.series.size(), defaults.series.size());
  for (std::size_t i = 0; i < n; ++i) {
    const QColor& c = style.series[i];
    if (c.isValid() && c.rgb() != defaults.series[i].rgb())
      preferences.plot_series_colors[static_cast<int>(i) + 1] = color_text(c);
  }
}

void set_plot_style(PlotStyle style) {
  const PlotStyle defaults = default_plot_style();
  for (std::size_t i = 0; i < style.series.size() && i < defaults.series.size(); ++i)
    if (!style.series[i].isValid()) style.series[i] = defaults.series[i];
  for (std::size_t i = style.series.size(); i < defaults.series.size(); ++i) style.series.push_back(defaults.series[i]);
  if (!style.plot_background.isValid()) style.plot_background = defaults.plot_background;
  if (style == current_style()) return;
  current_style() = std::move(style);
  emit PlotStyleHub::instance().changed();
}

PlotStyleHub& PlotStyleHub::instance() {
  static auto* hub = new PlotStyleHub;  // lives for the whole process
  return *hub;
}

}  // namespace pychron::ui
