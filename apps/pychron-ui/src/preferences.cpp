#include "preferences.hpp"

#include <QApplication>
#include <QSettings>
#include <QWidget>

#include "code_editor.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

const QString kFont = QStringLiteral("preferences/font_pt");
const QString kCodeFont = QStringLiteral("preferences/code_font_pt");
const QString kPageSize = QStringLiteral("preferences/browser_page_size");

// The saved integer when it parses and lies in [lo, hi]; else `fallback`.
int read_int(const QSettings& settings, const QString& key, int lo, int hi, int fallback) {
  bool ok = false;
  const int value = settings.value(key).toString().trimmed().toInt(&ok);
  return ok && value >= lo && value <= hi ? value : fallback;
}

}  // namespace

Preferences load_preferences(const QSettings& settings) {
  Preferences p;
  p.font_pt = read_int(settings, kFont, Preferences::kMinFontPt, Preferences::kMaxFontPt, 0);
  p.code_font_pt = read_int(settings, kCodeFont, Preferences::kMinFontPt, Preferences::kMaxFontPt, 0);
  p.browser_page_size = read_int(settings, kPageSize, Preferences::kMinPageSize, Preferences::kMaxPageSize,
                                 Preferences::kDefaultPageSize);
  return p;
}

void save_preferences(QSettings& settings, const Preferences& preferences) {
  settings.setValue(kFont, preferences.font_pt);
  settings.setValue(kCodeFont, preferences.code_font_pt);
  settings.setValue(kPageSize, preferences.browser_page_size);
  settings.sync();
}

void apply_application_preferences(const Preferences& preferences) {
  style::set_font_sizes(preferences.font_pt, preferences.code_font_pt);
  for (QWidget* widget : QApplication::allWidgets()) {
    if (auto* editor = qobject_cast<CodeEditor*>(widget)) editor->setFont(style::mono_font());
  }
}

}  // namespace pychron::ui
