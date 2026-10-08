#pragma once

// Glyphs of the data browser's toolbar: one per figure and fit window, plus
// recall and export. Line drawings in the manner of MainWindow::view_icon,
// in the theme's text colour.

#include <QIcon>
#include <QString>

namespace pychron::ui {

// `name`: a figure kind (time_series, ideogram, spectrum, inverse_isochron,
// isotope_evolution_fit, blank_fit, icfactor_fit), "recall" or "export". A
// null icon for any other name.
QIcon data_icon(const QString& name);

}  // namespace pychron::ui
