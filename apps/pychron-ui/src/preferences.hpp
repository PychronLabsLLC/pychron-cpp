#pragma once

// User preferences: the values File > Preferences… edits, kept in the
// application's QSettings under "preferences/". Saved values are untrusted:
// one that is missing, does not parse or is out of range reads as its default.
//
// Per-window state (geometry, the spectrometer's chart and target, the last
// queue) stays with its window. The spectrometer's large-move threshold is
// edited here too but kept with its spectrometer (SpectrometerWindow).

#include <map>
#include <string>

class QSettings;

namespace pychron::ui {

struct Preferences {
  static constexpr int kMinFontPt = 6;
  static constexpr int kMaxFontPt = 32;
  static constexpr int kDefaultPageSize = 200;
  static constexpr int kMinPageSize = 20;
  static constexpr int kMaxPageSize = 5000;

  int font_pt = 0;       // interface text; 0: the platform's size
  int code_font_pt = 0;  // script editors; 0: the interface size
  int browser_page_size = kDefaultPageSize;  // analyses per data browser page
  // The data browser draws a separator where a spectrometer ran nothing for
  // longer than this; 0: never.
  static constexpr double kDefaultGapHours = 6.0;
  static constexpr double kMaxGapHours = 720.0;
  double browser_gap_hours = kDefaultGapHours;
  // The analysis types' row colours where they differ from the theme's: class
  // ("unknown", "blank", "air", "cocktail", "detector_ic", "other") to
  // "#rrggbb", or to empty text for no colour (row_colors.hpp).
  std::map<std::string, std::string> browser_type_colors;

  bool operator==(const Preferences&) const = default;
};

Preferences load_preferences(const QSettings& settings);
void save_preferences(QSettings& settings, const Preferences& preferences);

// The application-wide part: interface and code font sizes, for every window
// open now and opened later. Windows apply the rest (MainWindow,
// DataMainWindow::apply_preferences).
void apply_application_preferences(const Preferences& preferences);

}  // namespace pychron::ui
