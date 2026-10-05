#pragma once

// Theme: the one place pychron-ui gets its colours, fonts and widget styling.
// Widgets ask for a colour by meaning (theme().error, theme().plot_bg), never
// by value, and mark state with the helpers below (set_tone, set_invalid,
// make_banner, set_chip), which the application style sheet turns into looks.
// apply() installs the style, palette, font and style sheet; without it the
// helpers only set properties. Light only.
//
// The look is "ink and signal": cool lab-white surfaces, ink text, an ink
// menu bar as the one dark band, and a single teal signal colour for focus,
// selection and the primary action. Status colours are kept for status.
//
// Colours that are data stay where the data is: detector and canvas colours
// from config, figure colours from pp::SceneStyle.

#include <array>
#include <cstddef>

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QString>

class QApplication;
class QLabel;
class QWidget;

namespace pychron::ui {

struct Theme {
  // Status fills: valves, health chips, diagnostic marks.
  QColor ok;
  QColor warning;
  QColor error;
  QColor inactive;  // unknown, no report yet, excluded

  // Text on a plain background.
  QColor text;
  QColor muted_text;  // secondary: descriptions, axis and row labels
  QColor faint_text;  // skipped rows, history, line numbers
  QColor error_text;
  QColor warning_text;
  QColor accent;  // overridden, selection
  QColor lock;    // a software-locked valve's border

  // Surfaces.
  QColor window;
  QColor base;
  QColor alt_base;      // alternate rows
  QColor gutter;        // code editor line numbers
  QColor plot_bg;       // pychron's light yellow
  QColor overlay;       // translucent backing for legends and annotations
  QColor grid;
  QColor outline;       // input hover border, scroll handle hover
  QColor neutral_fill;  // isolated volumes, pending steps
  QColor flash;

  // Chrome: the style sheet's frames, headers and the menu bar.
  QColor border;         // hairlines between surfaces
  QColor strong_border;  // input and button outlines
  QColor header_bg;      // table headers, dock titles
  QColor chrome;         // the menu bar
  QColor on_chrome;      // text on chrome
  QColor accent_strong;  // pressed, text on accent_soft
  QColor accent_soft;    // selected menu item, pressed button
  QColor accent_wash;    // hover
  QColor scroll_handle;
  QColor signal;  // the accent on chrome: the brand mark, splash and about banner

  // Tinted backgrounds for a row, cell or banner.
  QColor error_bg;
  QColor on_error_bg;  // text on error_bg
  QColor warning_bg;
  QColor success_bg;
  QColor stopped_bg;   // cancelled, aborted
  QColor progress_bg;  // in progress

  // Analysis table rows by analysis type.
  QColor row_blank;
  QColor row_air;
  QColor row_cocktail;
  QColor row_detector_ic;

  // Revision diff rows.
  QColor diff_changed;
  QColor diff_added;
  QColor diff_removed;

  struct Syntax {
    QColor keyword;
    QColor builtin;
    QColor command;
    QColor context;
    QColor number;
    QColor string;
    QColor comment;
    QColor header;
  } syntax;

  std::array<QColor, 8> series;   // detector traces without a configured colour
  // Canvas regions take the colour of the source they are connected to.
  struct Sources {
    QColor pump, pipette, laser, spectrometer, getter;
    std::array<QColor, 6> tanks;  // each tank its own: its gas is not another's
  } sources;
};

const Theme& theme();

namespace style {

// Fusion style, the theme's palette and style sheet. Call once, before any
// window is built.
void apply(QApplication& app);
QPalette palette();
QString style_sheet();

enum class Tone { Normal, Muted, Accent, Warning, Error };
// Text colour of a label by meaning.
void set_tone(QWidget* widget, Tone tone);

// Red border on an input whose value does not parse.
void set_invalid(QWidget* widget, bool invalid);

// An error banner: a frame holding labels, or a label on its own.
void make_banner(QWidget* widget);

enum class Level { Unknown, Ok, Warning, Error };
QColor level_color(Level level);
// A rounded status chip.
void set_chip(QLabel* label, Level level);

// Interface and code font sizes in points; 0 is the default (the platform's
// size; code follows the interface). The interface size reaches every widget
// that does not set its own; code editors refresh themselves
// (apply_application_preferences).
void set_font_sizes(int ui_pt, int code_pt);

// Tooltip text from plain lines: the first line is the subject and is set
// heavier, the rest are its details. A single line comes back as it is.
QString tip_text(const QString& plain);

// Window headline: larger and bold.
QFont title_font(const QFont& base);
// At the code font size when one is set.
QFont mono_font();

}  // namespace style

}  // namespace pychron::ui
