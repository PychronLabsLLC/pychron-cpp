#include "theme.hpp"

#include <QApplication>
#include <QLabel>
#include <QStyle>
#include <QVariant>
#include <QWidget>

namespace pychron::ui {

namespace {

QColor rgb(QRgb value) { return QColor(value); }

Theme light() {
  Theme t;
  t.ok = rgb(0x2ecc40);
  t.warning = rgb(0xffb300);
  t.error = rgb(0xe03c31);
  t.inactive = rgb(0x999999);

  t.text = rgb(0x222222);
  t.muted_text = rgb(0x555555);
  t.faint_text = rgb(0x888888);
  t.error_text = rgb(0xa01818);
  t.warning_text = rgb(0x9a6700);
  t.accent = rgb(0x1e6fe8);

  t.window = rgb(0xf0f0f0);
  t.base = rgb(0xffffff);
  t.alt_base = rgb(0xf7f7f7);
  t.gutter = rgb(0xf2f2f2);
  t.plot_bg = rgb(0xfafad2);
  t.overlay = QColor(255, 255, 255, 200);
  t.grid = rgb(0xbbbbbb);
  t.outline = rgb(0x555555);
  t.neutral_fill = rgb(0xdddddd);
  t.flash = rgb(0xffff00);

  t.error_bg = rgb(0xf8d7da);
  t.on_error_bg = rgb(0x721c24);
  t.warning_bg = rgb(0xfff3cd);
  t.success_bg = rgb(0xc8e6c9);
  t.stopped_bg = rgb(0xffe0b2);
  t.progress_bg = rgb(0xbbdefb);

  t.row_blank = rgb(0xe6f0ff);
  t.row_air = rgb(0xebfaeb);
  t.row_cocktail = rgb(0xfff5e1);
  t.row_detector_ic = rgb(0xf5ebff);

  t.diff_changed = rgb(0xffeca0);
  t.diff_added = rgb(0xcef0ce);
  t.diff_removed = rgb(0xf5cdcd);

  t.syntax.keyword = rgb(0x003399);
  t.syntax.builtin = rgb(0x800080);
  t.syntax.command = rgb(0x007a6e);
  t.syntax.context = rgb(0x8a4b08);
  t.syntax.number = rgb(0xc45200);
  t.syntax.string = rgb(0x067d17);
  t.syntax.comment = rgb(0x808080);
  t.syntax.header = rgb(0x9c27b0);

  t.series = {rgb(0x1f77b4), rgb(0xd62728), rgb(0x2ca02c), rgb(0xff7f0e),
              rgb(0x9467bd), rgb(0x8c564b), rgb(0x17becf), rgb(0x000000)};
  t.regions = {rgb(0x8fd3ff), rgb(0xffd27f), rgb(0xb6e3a8), rgb(0xe3b6e0), rgb(0xffb3a7), rgb(0xc9c3ff)};
  return t;
}

// A style sheet matches on the property's value when the widget is polished;
// a later change needs a re-polish to show.
void set_property(QWidget* widget, const char* name, const QVariant& value) {
  if (widget->property(name) == value) return;
  widget->setProperty(name, value);
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}

const char* tone_name(style::Tone tone) {
  switch (tone) {
    case style::Tone::Muted:
      return "muted";
    case style::Tone::Accent:
      return "accent";
    case style::Tone::Warning:
      return "warning";
    case style::Tone::Error:
      return "error";
    case style::Tone::Normal:
      break;
  }
  return "";
}

const char* level_name(style::Level level) {
  switch (level) {
    case style::Level::Ok:
      return "ok";
    case style::Level::Warning:
      return "warning";
    case style::Level::Error:
      return "error";
    case style::Level::Unknown:
      break;
  }
  return "unknown";
}

}  // namespace

const Theme& theme() {
  static const Theme t = light();
  return t;
}

namespace style {

QPalette palette() {
  const Theme& t = theme();
  QPalette p(t.window, t.window);  // derives the bevel shades
  p.setColor(QPalette::Window, t.window);
  p.setColor(QPalette::WindowText, t.text);
  p.setColor(QPalette::Base, t.base);
  p.setColor(QPalette::AlternateBase, t.alt_base);
  p.setColor(QPalette::Text, t.text);
  p.setColor(QPalette::Button, t.window);
  p.setColor(QPalette::ButtonText, t.text);
  p.setColor(QPalette::ToolTipBase, t.base);
  p.setColor(QPalette::ToolTipText, t.text);
  p.setColor(QPalette::PlaceholderText, t.faint_text);
  p.setColor(QPalette::Highlight, t.accent);
  p.setColor(QPalette::HighlightedText, t.base);
  p.setColor(QPalette::Link, t.accent);
  for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
    p.setColor(QPalette::Disabled, role, t.faint_text);
  return p;
}

QString style_sheet() {
  const Theme& t = theme();
  QString s;
  const auto tone = [&s](Tone which, const QColor& color) {
    s += QStringLiteral("*[tone=\"%1\"] { color: %2; }\n").arg(QLatin1String(tone_name(which)), color.name());
  };
  tone(Tone::Muted, t.muted_text);
  tone(Tone::Accent, t.accent);
  tone(Tone::Warning, t.warning_text);
  tone(Tone::Error, t.error_text);

  s += QStringLiteral("*[invalid=\"true\"] { border: 1px solid %1; }\n").arg(t.error.name());

  s += QStringLiteral("*[banner=\"true\"] { background: %1; color: %2; }\n"
                      "*[banner=\"true\"] QLabel { color: %2; }\n"
                      "QLabel[banner=\"true\"] { padding: 4px; }\n")
           .arg(t.error_bg.name(), t.on_error_bg.name());

  s += QStringLiteral("QLabel[chip] { border-radius: 4px; padding: 1px 6px; }\n");
  for (const auto level : {Level::Unknown, Level::Ok, Level::Warning, Level::Error})
    s += QStringLiteral("QLabel[chip=\"%1\"] { background: %2; }\n")
             .arg(QLatin1String(level_name(level)), level_color(level).name());
  return s;
}

void apply(QApplication& app) {
  QApplication::setStyle(QStringLiteral("Fusion"));
  QApplication::setPalette(palette());
  app.setStyleSheet(style_sheet());
}

void set_tone(QWidget* widget, Tone tone) { set_property(widget, "tone", QLatin1String(tone_name(tone))); }

void set_invalid(QWidget* widget, bool invalid) { set_property(widget, "invalid", invalid); }

void make_banner(QWidget* widget) { set_property(widget, "banner", true); }

QColor level_color(Level level) {
  const Theme& t = theme();
  switch (level) {
    case Level::Ok:
      return t.ok;
    case Level::Warning:
      return t.warning;
    case Level::Error:
      return t.error;
    case Level::Unknown:
      break;
  }
  return t.inactive;
}

void set_chip(QLabel* label, Level level) { set_property(label, "chip", QLatin1String(level_name(level))); }

QFont title_font(const QFont& base) {
  QFont f = base;
  f.setPointSizeF(f.pointSizeF() * 1.3);
  f.setBold(true);
  return f;
}

QFont mono_font() {
  QFont f(QStringLiteral("monospace"));
  f.setStyleHint(QFont::Monospace);
  return f;
}

}  // namespace style

}  // namespace pychron::ui
