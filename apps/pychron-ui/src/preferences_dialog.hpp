#pragma once

// File > Preferences…: a page list on the left (Appearance, Data and, with a
// spectrometer, Spectrometer), the page on the right. OK and Apply hand the
// values to the window's `apply`, which saves and applies them; Restore
// Defaults only resets the fields; Cancel drops what was not applied.

#include <functional>
#include <memory>
#include <optional>

#include <QDialog>
#include <QPointer>
#include <QSettings>

#include "preferences.hpp"

class QDialogButtonBox;
class QDoubleSpinBox;
class QListWidget;
class QSpinBox;
class QStackedWidget;

namespace pychron::ui {

class PreferencesDialog : public QDialog {
  Q_OBJECT

 public:
  struct Values {
    Preferences preferences;
    // The large magnet move threshold, amu (0: never ask); nullopt without a
    // spectrometer, which also leaves its page out.
    std::optional<double> confirm_move_amu;
  };
  using Apply = std::function<void(const Values&)>;
  using SettingsFactory = std::function<std::unique_ptr<QSettings>()>;

  PreferencesDialog(const Values& current, Apply apply, QWidget* parent = nullptr);

  // What a main window's File > Preferences… does: opens the dialog, window
  // modal, on the preferences kept in `settings` (empty: the application's
  // QSettings); OK and Apply save them there, then call `apply`. While `open`
  // holds a dialog that is showing, that one is raised instead.
  static PreferencesDialog* show_for(QWidget* window, QPointer<PreferencesDialog>& open,
                                     const SettingsFactory& settings, std::optional<double> confirm_move_amu,
                                     Apply apply);

  // What the fields hold now.
  Values values() const;
  void set_values(const Values& values);
  void restore_defaults();

  // For tests.
  QListWidget* pages() const noexcept { return pages_; }
  QStackedWidget* stack() const noexcept { return stack_; }
  QSpinBox* font_size() const noexcept { return font_; }
  QSpinBox* code_font_size() const noexcept { return code_font_; }
  QSpinBox* page_size() const noexcept { return page_size_; }
  QDoubleSpinBox* confirm_move() const noexcept { return confirm_move_; }  // null without a spectrometer
  QDialogButtonBox* buttons() const noexcept { return buttons_; }

 private:
  void add_page(const QString& name, QWidget* page);

  Apply apply_;
  QListWidget* pages_;
  QStackedWidget* stack_;
  QSpinBox* font_;
  QSpinBox* code_font_;
  QSpinBox* page_size_;
  QDoubleSpinBox* confirm_move_ = nullptr;
  QDialogButtonBox* buttons_;
};

}  // namespace pychron::ui
