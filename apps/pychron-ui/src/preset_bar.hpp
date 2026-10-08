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

  // An entry before the presets that is not one of them (a flux level's
  // "(saved fit)"), shown selected; only an empty text takes it away. It stays
  // in the list when a preset is chosen and over reload(), so it can be chosen
  // again. While it is selected Save asks for a name, and Delete and Factory
  // have nothing to act on. Choosing it emits pinned_chosen().
  void set_pinned_item(const QString& text);
  bool pinned_selected() const;
  // Shows the preset `name` as the one in use without loading it; nothing when there is none.
  void show_name(const QString& name);

  // Loads `name` and emits loaded(); false (and message()) when it cannot.
  bool select(const QString& name);
  // The list read again, `select` shown when it is one of the presets.
  void reload(const QString& select);
  bool save(bool as);
  bool remove();
  bool factory_reset();

 signals:
  void loaded(const processing::Options& options, const QString& name);
  // A status line, and a tooltip with details (preset load warnings).
  void message(const QString& text, const QString& details);
  void pinned_chosen();

 private:
  processing::PresetStore& store_;
  processing::SchemaPtr schema_;
  QComboBox* combo_;
  QString pinned_;  // not empty: the combo's first item
};

}  // namespace pychron::ui
