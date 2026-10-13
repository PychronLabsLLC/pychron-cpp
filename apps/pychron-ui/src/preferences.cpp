#include "preferences.hpp"

#include <string_view>

#include <QApplication>
#include <QSettings>
#include <QWidget>

#include "code_editor.hpp"
#include "row_colors.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

const QString kFont = QStringLiteral("preferences/font_pt");
const QString kCodeFont = QStringLiteral("preferences/code_font_pt");
const QString kPageSize = QStringLiteral("preferences/browser_page_size");
const QString kGapHours = QStringLiteral("preferences/browser_gap_hours");
const QString kTypeColors = QStringLiteral("preferences/browser_type_colors");

QString type_color_key(std::string_view type_class) {
  return kTypeColors + QLatin1Char('/') + QString::fromUtf8(type_class.data(), static_cast<qsizetype>(type_class.size()));
}

// The saved integer when it parses and lies in [lo, hi]; else `fallback`.
int read_int(const QSettings& settings, const QString& key, int lo, int hi, int fallback) {
  bool ok = false;
  const int value = settings.value(key).toString().trimmed().toInt(&ok);
  return ok && value >= lo && value <= hi ? value : fallback;
}

// The saved number when it parses and lies in [lo, hi]; else `fallback`.
double read_double(const QSettings& settings, const QString& key, double lo, double hi, double fallback) {
  bool ok = false;
  const double value = settings.value(key).toString().trimmed().toDouble(&ok);
  return ok && value >= lo && value <= hi ? value : fallback;
}

}  // namespace

Preferences load_preferences(const QSettings& settings) {
  Preferences p;
  p.font_pt = read_int(settings, kFont, Preferences::kMinFontPt, Preferences::kMaxFontPt, 0);
  p.code_font_pt = read_int(settings, kCodeFont, Preferences::kMinFontPt, Preferences::kMaxFontPt, 0);
  p.browser_page_size = read_int(settings, kPageSize, Preferences::kMinPageSize, Preferences::kMaxPageSize,
                                 Preferences::kDefaultPageSize);
  p.browser_gap_hours = read_double(settings, kGapHours, 0.0, Preferences::kMaxGapHours, Preferences::kDefaultGapHours);
  for (const std::string_view type_class : kTypeClasses) {
    const QString key = type_color_key(type_class);
    if (!settings.contains(key)) continue;
    const QString text = settings.value(key).toString().trimmed().toLower();
    if (is_type_color_text(text)) p.browser_type_colors[std::string(type_class)] = text.toStdString();
  }
  return p;
}

void save_preferences(QSettings& settings, const Preferences& preferences) {
  settings.setValue(kFont, preferences.font_pt);
  settings.setValue(kCodeFont, preferences.code_font_pt);
  settings.setValue(kPageSize, preferences.browser_page_size);
  settings.setValue(kGapHours, preferences.browser_gap_hours);
  settings.remove(kTypeColors);  // a class no longer named is back to the theme's colour
  for (const auto& [type_class, text] : preferences.browser_type_colors)
    settings.setValue(type_color_key(type_class), QString::fromStdString(text));
  settings.sync();
}

void apply_application_preferences(const Preferences& preferences) {
  style::set_font_sizes(preferences.font_pt, preferences.code_font_pt);
  for (QWidget* widget : QApplication::allWidgets()) {
    if (auto* editor = qobject_cast<CodeEditor*>(widget)) editor->setFont(style::mono_font());
  }
}

}  // namespace pychron::ui
