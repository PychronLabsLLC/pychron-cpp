#include "theme.hpp"

#include <initializer_list>
#include <utility>

#include <QApplication>
#include <QFontDatabase>
#include <QFontInfo>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QStyle>
#include <QTemporaryDir>
#include <QVariant>
#include <QWidget>

namespace pychron::ui {

namespace {

QColor rgb(QRgb value) { return QColor(value); }

Theme light() {
  Theme t;
  t.ok = rgb(0x1fb46a);
  t.warning = rgb(0xf5a524);
  t.error = rgb(0xe5484d);
  t.inactive = rgb(0xa7b0bc);

  t.text = rgb(0x141b24);
  t.muted_text = rgb(0x536070);
  t.faint_text = rgb(0x8b95a3);
  t.error_text = rgb(0xb4232a);
  t.warning_text = rgb(0x8a5a00);
  t.accent = rgb(0x0b7a75);
  t.lock = rgb(0x1f6feb);

  t.window = rgb(0xeef1f4);
  t.base = rgb(0xffffff);
  t.alt_base = rgb(0xf6f8fa);
  t.gutter = rgb(0xf1f4f7);
  t.plot_bg = rgb(0xfafad2);
  t.overlay = QColor(255, 255, 255, 215);
  t.grid = rgb(0xc9d1da);
  t.outline = rgb(0x4a5563);
  t.neutral_fill = rgb(0xdde3ea);
  t.flash = rgb(0xffe14d);

  t.border = rgb(0xd5dce4);
  t.strong_border = rgb(0xb7c1cc);
  t.header_bg = rgb(0xe6ebf0);
  t.chrome = rgb(0x111a24);
  t.on_chrome = rgb(0xe6edf3);
  t.accent_strong = rgb(0x065c58);
  t.accent_soft = rgb(0xcde9e6);
  t.accent_wash = rgb(0xeaf5f4);
  t.scroll_handle = rgb(0xc2cbd5);
  t.signal = rgb(0x3fd6c6);

  t.error_bg = rgb(0xfde7e7);
  t.on_error_bg = rgb(0x7a1419);
  t.warning_bg = rgb(0xfff3d4);
  t.success_bg = rgb(0xd8f4e5);
  t.stopped_bg = rgb(0xffe7cf);
  t.progress_bg = rgb(0xd3eeec);

  t.row_blank = rgb(0xe7effd);
  t.row_air = rgb(0xe5f6ec);
  t.row_cocktail = rgb(0xfff2dc);
  t.row_detector_ic = rgb(0xf0e9fc);

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
  t.sources.pump = rgb(0x8fd3ff);
  t.sources.pipette = rgb(0xe3b6e0);
  t.sources.laser = rgb(0xffb3a7);
  t.sources.tanks = {rgb(0xc9c3ff), rgb(0x8fe0d0), rgb(0xf2e08a), rgb(0xf5b8d0), rgb(0xa9c4f5), rgb(0xd9c7a3)};
  t.sources.spectrometer = rgb(0xb6e3a8);
  t.sources.getter = rgb(0xffd27f);
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

// A style sheet that restyles a combo or spin box frame must also supply its
// arrows as images. They are drawn here, at twice the size they are shown, into
// a directory that lasts as long as the application.
QString arrow_dir(const Theme& t) {
  static QTemporaryDir dir;
  static bool drawn = false;
  if (drawn || !dir.isValid()) return dir.path();
  drawn = true;
  const auto stroke = [&](const QString& name, const QPolygonF& line, const QColor& color) {
    QImage image(20, 20, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(line);
    p.end();
    image.save(dir.filePath(name));
  };
  const auto draw = [&](const QString& name, bool up, const QColor& color) {
    const double a = up ? 13 : 7;
    const double b = up ? 7 : 13;
    stroke(name, QPolygonF({QPointF(4, a), QPointF(10, b), QPointF(16, a)}), color);
  };
  // What a ticked box and a partly ticked one show, in the page's white on the accent.
  stroke(QStringLiteral("check.png"), QPolygonF({QPointF(4.5, 10.5), QPointF(8.5, 14.5), QPointF(15.5, 6)}), t.base);
  stroke(QStringLiteral("dash.png"), QPolygonF({QPointF(5, 10), QPointF(15, 10)}), t.base);
  draw(QStringLiteral("down.png"), false, t.muted_text);
  draw(QStringLiteral("up.png"), true, t.muted_text);
  draw(QStringLiteral("down-off.png"), false, t.strong_border);
  draw(QStringLiteral("up-off.png"), true, t.strong_border);
  return dir.path();
}

// The widget chrome. Colours are @name tokens, filled from the theme.
// Item views keep their default item drawing so a model's BackgroundRole
// (run states, analysis types) still shows; only frames and headers change.
// A tooltip is a menu's surface with square corners: a tooltip window is
// opaque on macOS, so a radius leaves its corners filled.
QString chrome_sheet(const Theme& t) {
  QString s = QStringLiteral(R"(
QMainWindow::separator { background: @window; width: 5px; height: 5px; }
QMainWindow::separator:hover { background: @accent; }

QMenuBar { background: @chrome; color: @on_chrome; padding: 3px 6px; border: none; }
QMenuBar::item { background: transparent; padding: 5px 12px; border-radius: 5px; }
QMenuBar::item:selected { background: @accent_strong; }
QMenuBar::item:pressed { background: @accent; color: @base; }
QMenu { background: @base; border: 1px solid @strong_border; border-radius: 8px; padding: 5px; }
QMenu::item { padding: 6px 26px 6px 12px; border-radius: 5px; }
QMenu::item:selected { background: @accent_soft; color: @accent_strong; }
QMenu::item:disabled { color: @faint_text; }
QMenu::separator { height: 1px; background: @border; margin: 4px 8px; }
QToolTip { background: @base; color: @text; border: 1px solid @strong_border; padding: 4px 7px; }

QToolBar { background: @base; border: none; border-bottom: 1px solid @border; padding: 4px 6px; spacing: 4px; }
QToolBar::separator { background: @border; width: 1px; margin: 4px 6px; }
QStatusBar { background: @base; border-top: 1px solid @border; color: @muted_text; }
QStatusBar::item { border: none; }

QDockWidget::title { background: @header_bg; padding: 6px 10px; border-left: 3px solid @accent; text-align: left; }

QPushButton { background: @base; color: @text; border: 1px solid @strong_border; border-radius: 6px; padding: 5px 14px; min-height: 18px; }
QPushButton:hover { background: @accent_wash; border-color: @accent; }
QPushButton:pressed, QPushButton:checked { background: @accent_soft; border-color: @accent_strong; color: @accent_strong; }
QPushButton:default { background: @accent; border-color: @accent_strong; color: @base; }
QPushButton:default:hover { background: @accent_strong; }
QPushButton:disabled { background: @alt_base; border-color: @border; color: @faint_text; }
QPushButton:focus { outline: none; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 4px 8px; }
QToolButton:hover { background: @accent_wash; border-color: @border; }
QToolButton:pressed, QToolButton:checked { background: @accent_soft; border-color: @accent; color: @accent_strong; }
QToolButton:disabled { color: @faint_text; }

QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
  background: @base; color: @text; border: 1px solid @border; border-radius: 6px;
  padding: 4px 6px; selection-background-color: @accent; selection-color: @base; }
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover { border-color: @strong_border; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
  border: 2px solid @accent; padding: 3px 5px; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {
  background: @alt_base; border-color: @border; color: @faint_text; }
QLineEdit:read-only { background: @alt_base; }
QComboBox { padding-right: 22px; }
QComboBox:focus { padding-right: 21px; }
QComboBox::drop-down { subcontrol-origin: border; subcontrol-position: center right; width: 20px; margin-right: 1px;
  border: none; }
QComboBox::down-arrow { image: url(@arrows/down.png); width: 10px; height: 10px; }
QComboBox::down-arrow:disabled { image: url(@arrows/down-off.png); }
QComboBox::down-arrow:on { image: url(@arrows/up.png); }
QComboBox QAbstractItemView { background: @base; border: 1px solid @strong_border; outline: 0;
  selection-background-color: @accent_soft; selection-color: @accent_strong; }
QSpinBox, QDoubleSpinBox { padding-right: 18px; }
QSpinBox:focus, QDoubleSpinBox:focus { padding-right: 17px; }
QSpinBox::up-button, QDoubleSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::down-button {
  subcontrol-origin: border; width: 16px; border: none; background: transparent; }
QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-position: top right; margin: 2px 2px 0 0; }
QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-position: bottom right; margin: 0 2px 2px 0; }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(@arrows/up.png); width: 8px; height: 8px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(@arrows/down.png); width: 8px; height: 8px; }
QSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:disabled, QSpinBox::up-arrow:off, QDoubleSpinBox::up-arrow:off {
  image: url(@arrows/up-off.png); }
QSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:disabled, QSpinBox::down-arrow:off,
QDoubleSpinBox::down-arrow:off { image: url(@arrows/down-off.png); }
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover { background: @accent_wash; border-radius: 3px; }

QCheckBox::indicator, QGroupBox::indicator, QAbstractItemView::indicator {
  width: 14px; height: 14px; background: @base; border: 1px solid @strong_border; border-radius: 4px; }
QCheckBox::indicator:hover, QGroupBox::indicator:hover { border-color: @accent; }
QCheckBox::indicator:checked, QGroupBox::indicator:checked, QAbstractItemView::indicator:checked {
  background: @accent; border-color: @accent; image: url(@arrows/check.png); }
QCheckBox::indicator:indeterminate, QGroupBox::indicator:indeterminate, QAbstractItemView::indicator:indeterminate {
  background: @accent; border-color: @accent; image: url(@arrows/dash.png); }
QCheckBox::indicator:disabled, QGroupBox::indicator:disabled, QAbstractItemView::indicator:disabled {
  background: @alt_base; border-color: @border; }
QCheckBox::indicator:checked:disabled, QGroupBox::indicator:checked:disabled,
QAbstractItemView::indicator:checked:disabled, QCheckBox::indicator:indeterminate:disabled,
QGroupBox::indicator:indeterminate:disabled, QAbstractItemView::indicator:indeterminate:disabled {
  background: @accent_soft; border-color: @accent_soft; }

QListWidget#PreferencesPages { outline: 0; }
QListWidget#PreferencesPages::item { padding: 0 10px; }
QListWidget#PreferencesPages::item:hover { background: @accent_wash; }
QListWidget#PreferencesPages::item:selected { background: @accent; color: @base; }

QPlainTextEdit, QTextEdit, QAbstractItemView {
  background: @base; border: 1px solid @border; selection-background-color: @accent; selection-color: @base; }
QPlainTextEdit:focus, QTextEdit:focus, QAbstractItemView:focus { border-color: @accent; }
QTableView, QTreeView, QListView { gridline-color: @border; alternate-background-color: @alt_base; }
QHeaderView { background: @header_bg; border: none; }
QHeaderView::section { background: @header_bg; color: @muted_text; font-weight: 600; padding: 5px 8px;
  border: none; border-right: 1px solid @border; border-bottom: 1px solid @strong_border; }
QHeaderView::section:hover { color: @text; }
QHeaderView::section:checked { color: @accent_strong; }
QTableCornerButton::section { background: @header_bg; border: none; border-bottom: 1px solid @strong_border; }

QTabWidget::pane { background: @base; border: 1px solid @border; border-radius: 6px; top: -1px; }
QTabBar::tab { background: transparent; color: @muted_text; padding: 7px 16px; border: none;
  border-bottom: 2px solid transparent; margin-right: 2px; }
QTabBar::tab:hover { color: @text; background: @accent_wash; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }

QGroupBox { background: @base; border: 1px solid @border; border-radius: 8px; margin-top: 12px; padding: 12px 8px 8px 8px; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 10px; padding: 0 5px;
  color: @accent_strong; font-weight: 600; }

QProgressBar { background: @border; border: none; border-radius: 5px; text-align: center; color: @text; min-height: 14px; }
QProgressBar::chunk { background: @accent; border-radius: 5px; }

QSplitter::handle { background: transparent; }
QSplitter::handle:hover { background: @accent_soft; }

QScrollBar:vertical { background: transparent; width: 12px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 0; }
QScrollBar::handle { background: @scroll_handle; border: 3px solid transparent; border-radius: 6px; }
QScrollBar::handle:vertical { min-height: 32px; }
QScrollBar::handle:horizontal { min-width: 32px; }
QScrollBar::handle:hover { background: @outline; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
QScrollArea { background: transparent; }

QFrame#CommandPalette { background: @base; border: 1px solid @strong_border; }
QFrame#CommandPalette QLineEdit { border: none; border-bottom: 2px solid @accent; border-radius: 0; padding: 10px 14px; }
QFrame#CommandPalette QTreeView { border: none; padding: 2px 0; }
QFrame#CommandPalette QTreeView::item { padding: 0 10px; border: none; background: transparent; }
QFrame#CommandPalette QTreeView::item:hover { background: @accent_wash; }
QFrame#CommandPalette QTreeView::item:selected { background: @accent_soft; color: @accent_strong; }
)");
  const std::pair<const char*, QColor> colors[] = {
      {"accent_strong", t.accent_strong}, {"accent_soft", t.accent_soft}, {"accent_wash", t.accent_wash},
      {"strong_border", t.strong_border}, {"scroll_handle", t.scroll_handle}, {"muted_text", t.muted_text},
      {"faint_text", t.faint_text},       {"header_bg", t.header_bg},     {"on_chrome", t.on_chrome},
      {"alt_base", t.alt_base},           {"outline", t.outline},         {"border", t.border},
      {"chrome", t.chrome},               {"accent", t.accent},           {"window", t.window},
      {"base", t.base},                   {"text", t.text},
  };
  s.replace(QStringLiteral("@arrows"), arrow_dir(t));
  // Longer names first, so @accent never eats the start of @accent_soft.
  for (const auto& [name, color] : colors) s.replace(QLatin1Char('@') + QLatin1String(name), color.name());
  return s;
}

// The platform's interface font size, remembered before a preference changes it.
double platform_point_size() {
  static const double pt = QApplication::font().pointSizeF();
  return pt;
}

int code_point_size = 0;

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
  QString s = chrome_sheet(t);
  const auto tone = [&s](Tone which, const QColor& color) {
    s += QStringLiteral("*[tone=\"%1\"] { color: %2; }\n").arg(QLatin1String(tone_name(which)), color.name());
  };
  tone(Tone::Muted, t.muted_text);
  tone(Tone::Accent, t.accent);
  tone(Tone::Warning, t.warning_text);
  tone(Tone::Error, t.error_text);

  // After the chrome, and with :focus spelled out, so it beats the focus ring.
  // With the focus it is a ring of the same two pixels: the field keeps its size.
  s += QStringLiteral("*[invalid=\"true\"], *[invalid=\"true\"]:hover { border: 1px solid %1; background: %2; }\n"
                      "*[invalid=\"true\"]:focus { border: 2px solid %1; background: %2; }\n")
           .arg(t.error.name(), t.error_bg.name());

  s += QStringLiteral("*[banner=\"true\"] { background: %1; color: %2; }\n"
                      "*[banner=\"true\"] QLabel { color: %2; }\n"
                      "*[banner=\"true\"] { border-bottom: 2px solid %3; }\n"
                      "QLabel[banner=\"true\"] { padding: 6px 10px; font-weight: 600; }\n")
           .arg(t.error_bg.name(), t.on_error_bg.name(), t.error.name());

  // Pills; the text is the darkest shade of the fill that still reads.
  s += QStringLiteral("QLabel[chip] { border-radius: 9px; padding: 2px 10px; font-weight: 600; }\n");
  for (const auto level : {Level::Unknown, Level::Ok, Level::Warning, Level::Error}) {
    const QColor fill = level_color(level);
    const QColor ink = level == Level::Error ? t.base : fill.darker(level == Level::Unknown ? 260 : 330);
    s += QStringLiteral("QLabel[chip=\"%1\"] { background: %2; color: %3; }\n")
             .arg(QLatin1String(level_name(level)), fill.name(), ink.name());
  }
  return s;
}

namespace {

// A family that is not installed is never handed to Qt: on macOS that makes
// it build its whole alias table ("Populating font family aliases took ...
// ms").
bool installed(const QString& family) {
  static const QStringList families = QFontDatabase::families();
  return families.contains(family, Qt::CaseInsensitive);
}

// The first of `wanted` installed here, or empty.
QString installed_family(std::initializer_list<QLatin1String> wanted) {
  for (const QLatin1String name : wanted) {
    if (installed(name)) return name;
  }
  return {};
}

}  // namespace

void apply(QApplication& app) {
  platform_point_size();
  QApplication::setStyle(QStringLiteral("Fusion"));
  QApplication::setPalette(palette());
  // IBM Plex Sans (or Windows 11's variable Segoe) where installed, else the
  // platform's own UI font. The size stays the platform's until set_font_sizes.
  QFont font = QApplication::font();
  if (const QString family = installed_family({QLatin1String("IBM Plex Sans"), QLatin1String("Segoe UI Variable Text")});
      !family.isEmpty()) {
    font.setFamily(family);
  } else if (!installed(font.family())) {
    // A platform theme may name a family that is not installed (the offscreen
    // platform's is "Sans Serif"); the one Qt resolves it to is.
    if (const QString resolved = QFontInfo(font).family(); installed(resolved)) font.setFamily(resolved);
  }
  font.setHintingPreference(QFont::PreferNoHinting);
  QApplication::setFont(font);
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

void set_font_sizes(int ui_pt, int code_pt) {
  QFont font = QApplication::font();
  const double pt = ui_pt > 0 ? ui_pt : platform_point_size();
  if (pt > 0) font.setPointSizeF(pt);
  QApplication::setFont(font);
  code_point_size = code_pt > 0 ? code_pt : 0;
}

QString tip_text(const QString& plain) {
  const qsizetype cut = plain.indexOf(QLatin1Char('\n'));
  if (cut < 0) return plain;
  QString rest = plain.mid(cut + 1).toHtmlEscaped();
  rest.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
  return QStringLiteral("<p style=\"white-space:pre; margin:0\"><span style=\"font-weight:600\">%1</span><br>%2</p>")
      .arg(plain.left(cut).toHtmlEscaped(), rest);
}

QFont title_font(const QFont& base) {
  QFont f = base;
  f.setPointSizeF(f.pointSizeF() * 1.3);
  f.setBold(true);
  return f;
}

QFont mono_font() {
  // The platform's fixed font (a real family everywhere, unlike "monospace"),
  // or the first of these that is installed.
  QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  if (const QString family =
          installed_family({QLatin1String("JetBrains Mono"), QLatin1String("IBM Plex Mono"), QLatin1String("SF Mono"),
                            QLatin1String("Cascadia Mono"), QLatin1String("Consolas"), QLatin1String("Menlo")});
      !family.isEmpty())
    f.setFamily(family);
  f.setStyleHint(QFont::Monospace);
  // The system font carries its own size; code follows the interface's unless set.
  if (code_point_size > 0) {
    f.setPointSize(code_point_size);
  } else {
    f.setPointSizeF(QApplication::font().pointSizeF());
  }
  return f;
}

}  // namespace style

}  // namespace pychron::ui
