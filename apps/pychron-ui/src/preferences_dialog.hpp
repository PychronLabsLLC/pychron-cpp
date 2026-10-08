#pragma once

// File > Preferences…: a page list on the left (Appearance, Data, with a
// spectrometer Spectrometer, and with a line Logging and Metrics), the page
// on the right. OK and Apply hand the values to the window's `apply`, which
// saves and applies them; Restore Defaults only resets the fields; Cancel
// drops what was not applied.
//
// The line's two pages are saved first, to the line's local override file,
// and that can fail (a folder that cannot be written). The dialog then says
// so, stays open and applies nothing.

#include <functional>
#include <memory>
#include <optional>

#include <QDialog>
#include <QPointer>
#include <QSettings>

#include "line_settings.hpp"
#include "preferences.hpp"

class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QSpinBox;
class QStackedWidget;

namespace pychron::ui {

class LoggingPage;
class MetricsPage;

// The grey explanatory line under a page's fields.
QLabel* preferences_note(const QString& text);

class PreferencesDialog : public QDialog {
  Q_OBJECT

 public:
  struct Values {
    Preferences preferences;
    // The large magnet move threshold, amu (0: never ask); nullopt without a
    // spectrometer, which also leaves its page out.
    std::optional<double> confirm_move_amu;
    // The line's logging and metrics; nullopt without a line, which leaves
    // their pages out.
    std::optional<LineSettings> line = std::nullopt;
  };
  using Apply = std::function<void(const Values&)>;
  // Saves the line's settings; empty when done, else what went wrong.
  using LineSave = std::function<QString(const LineSettings&)>;
  using SettingsFactory = std::function<std::unique_ptr<QSettings>()>;

  PreferencesDialog(const Values& current, Apply apply, QWidget* parent = nullptr);

  // Called by OK and Apply before `apply`, with the line's pages' values.
  void set_line_save(LineSave save) { line_save_ = std::move(save); }

  // What a main window's File > Preferences… does: opens the dialog, window
  // modal, on the preferences kept in `settings` (empty: the application's
  // QSettings); OK and Apply save them there, then call `apply`. While `open`
  // holds a dialog that is showing, that one is raised instead.
  static PreferencesDialog* show_for(QWidget* window, QPointer<PreferencesDialog>& open,
                                     const SettingsFactory& settings, std::optional<double> confirm_move_amu,
                                     Apply apply, std::optional<LineSettings> line = std::nullopt,
                                     LineSave line_save = {});

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
  LoggingPage* logging_page() const noexcept { return logging_; }          // null without a line
  MetricsPage* metrics_page() const noexcept { return metrics_; }          // null without a line
  QLabel* problem() const noexcept { return problem_; }                    // why OK or Apply did nothing
  QDialogButtonBox* buttons() const noexcept { return buttons_; }

 private:
  void add_page(const QString& name, QWidget* page);
  // Saves and applies what the fields hold; false (and a word in problem())
  // when a field is wrong or the line's settings could not be saved.
  bool commit();
  void complain(const QString& what, QWidget* page);

  Apply apply_;
  LineSave line_save_;
  std::optional<LineSettings> line_;  // as given: what is shared, and the endpoint's status
  QListWidget* pages_;
  QStackedWidget* stack_;
  QSpinBox* font_;
  QSpinBox* code_font_;
  QSpinBox* page_size_;
  QDoubleSpinBox* confirm_move_ = nullptr;
  LoggingPage* logging_ = nullptr;
  MetricsPage* metrics_ = nullptr;
  QLabel* problem_;
  QDialogButtonBox* buttons_;
};

}  // namespace pychron::ui
