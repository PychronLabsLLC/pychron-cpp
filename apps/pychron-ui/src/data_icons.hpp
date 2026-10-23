#pragma once

// Glyphs of the data browser's toolbar: one per figure and fit window, plus
// recall, export and publish. Line drawings in the manner of MainWindow::view_icon,
// in the theme's text colour.

#include <QIcon>
#include <QString>

namespace pychron::ui {

// `name`: a figure kind (time_series, ideogram, spectrum, inverse_isochron,
// spectrum_isochron, isotope_evolution_fit, blank_fit, icfactor_fit), "recall", "export" or "publish". Any
// other name (a figure of a domain without a glyph of its own) draws the
// time_series glyph, the generic figure.
QIcon data_icon(const QString& name);

}  // namespace pychron::ui
