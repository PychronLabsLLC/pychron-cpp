#pragma once

// PresetBar (data browsing and visualization design, section 6.3): the named
// presets of one options schema, factory < lab < user, with Save, Save as,
// Delete and Factory. Figure windows put it above their options editor.

#include <functional>

#include <QString>
#include <QWidget>

#include "pychron/processing/options.hpp"

class QComboBox;

namespace pychron::ui {

class PresetBar : public QWidget {
  Q_OBJECT

 public:
  // `store` must outlive the bar.
  PresetBar(processing::PresetStore& store, processing::SchemaPtr schema, QWidget* parent = nullptr);

  // The options Save writes; required.
  std::function<processing::Options()> current;
  // The name for "Save as" (empty: cancelled); a dialog by default.
  std::function<QString()> ask_name;

  QComboBox* combo() const noexcept { return combo_; }
  QString current_name() const;

  // Loads `name` and emits loaded(); false (and message()) when it cannot.
  bool select(const QString& name);
  void reload(const QString& select);
  bool save(bool as);
  bool remove();
  bool factory_reset();

 signals:
  void loaded(const processing::Options& options, const QString& name);
  // A status line, and a tooltip with details (preset load warnings).
  void message(const QString& text, const QString& details);

 private:
  processing::PresetStore& store_;
  processing::SchemaPtr schema_;
  QComboBox* combo_;
};

}  // namespace pychron::ui
