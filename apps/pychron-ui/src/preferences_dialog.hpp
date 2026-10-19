#pragma once

// File > Preferences…: a page list on the left (Appearance, Data, Plots, with
// a spectrometer Spectrometer, with a line Logging and Metrics, and EarthBank),
// the page on the right. OK and Apply hand the values to the window's `apply`, which
// saves and applies them; Restore Defaults only resets the fields; Cancel
// drops what was not applied.
//
// The line's two pages are saved first, to the line's local override file,
// and that can fail (a folder that cannot be written). The dialog then says
// so, stays open and applies nothing.
//
// The EarthBank page keeps a file of its own too, the profiles `elctl` also
// reads (earthbank_preferences_page.hpp). It is written before anything else,
// and a row that is wrong likewise leaves the dialog open with nothing applied.
//
// A window that runs on an install adds Data source (data_source_page.hpp,
// add_data_source_page), last. Changed, OK and Apply check it and write the
// install's credentials, record and site entry, after the EarthBank page and
// before the line's; a failed check leaves it unwritten, says why and offers
// "Save without checking". Written, the store is fixed for the life of the
// process, so the dialog asks to reopen.

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include <QDialog>
#include <QPointer>
#include <QSettings>
#include <QWidget>

#include "data_source_page.hpp"
#include "line_settings.hpp"
#include "plot_style.hpp"
#include "preferences.hpp"
#include "row_colors.hpp"

class QCheckBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;

namespace pychron::ui {

class EarthbankPreferencesPage;
class LoggingPage;
class MetricsPage;

// One colour of the Data or Plots page: a button showing it that opens the
// colour dialog, and a box for "no colour".
class ColorField : public QWidget {
  Q_OBJECT

 public:
  explicit ColorField(QWidget* parent = nullptr);
  // Invalid: no colour.
  QColor color() const;
  // An invalid colour is "no colour", or ignored when there can be none.
  void set_color(const QColor& color);
  // False: the "no colour" box is hidden and color() is a colour once one
  // was set. A plot colour is never none.
  void set_can_be_none(bool can);
  QPushButton* button() const noexcept { return button_; }
  QCheckBox* none() const noexcept { return none_; }
  // The colour dialog; tests replace it. Invalid: cancelled.
  std::function<QColor(const QColor& current)> ask;

 private:
  void show_color();

  QColor color_;  // the last one chosen, kept while "no colour" is ticked
  bool can_be_none_ = true;
  QPushButton* button_;
  QCheckBox* none_;
};

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

  // Closes the window and starts pychron-ui on the install again.
  using Reopen = std::function<void()>;
  // Whether to reopen now; default: "Pychron must reopen to use the new data
  // source." with Reopen now / Later.
  using AskReopen = std::function<bool()>;
  // Adds the Data source page, last; once (a second call does nothing).
  void add_data_source_page(DataSourcePageDeps deps, Reopen reopen, AskReopen ask = {});

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
  QDoubleSpinBox* gap_hours() const noexcept { return gap_hours_; }
  // The colour of an analysis type class (row_colors.hpp); null for no class.
  ColorField* type_color(const std::string& type_class) const;
  QPushButton* reset_colors() const noexcept { return reset_colors_; }
  // The Plots page: the live plots' background and series 0..11 (null past
  // the last), and its own Reset colors.
  ColorField* plot_background() const noexcept { return plot_background_; }
  ColorField* plot_series(std::size_t slot) const;
  QPushButton* reset_plot_colors() const noexcept { return reset_plot_colors_; }
  QDoubleSpinBox* confirm_move() const noexcept { return confirm_move_; }  // null without a spectrometer
  LoggingPage* logging_page() const noexcept { return logging_; }          // null without a line
  MetricsPage* metrics_page() const noexcept { return metrics_; }          // null without a line
  EarthbankPreferencesPage* earthbank_page() const noexcept { return earthbank_; }
  DataSourcePage* data_source_page() const noexcept { return data_source_; }  // null when not added
  QPushButton* save_without_checking() const noexcept { return save_without_checking_; }  // shown after a failed check
  QLabel* problem() const noexcept { return problem_; }                    // why OK or Apply did nothing
  QDialogButtonBox* buttons() const noexcept { return buttons_; }

 private:
  static constexpr std::size_t kPlotSeries = 12;  // as default_plot_style()

  void add_page(const QString& name, QWidget* page);
  QWidget* make_plots_page();
  void show_plot_style(const PlotStyle& style);
  // Saves and applies what the fields hold; false (and a word in problem())
  // when a field is wrong or the line's settings could not be saved.
  // `check_data_source` false: the Data source page is written without its
  // checks (Save without checking).
  bool commit(bool check_data_source = true);
  void complain(const QString& what, QWidget* page);

  Apply apply_;
  LineSave line_save_;
  std::optional<LineSettings> line_;  // as given: what is shared, and the endpoint's status
  QListWidget* pages_;
  QStackedWidget* stack_;
  QSpinBox* font_;
  QSpinBox* code_font_;
  QSpinBox* page_size_;
  QDoubleSpinBox* gap_hours_;
  std::map<std::string, ColorField*> type_colors_;
  QPushButton* reset_colors_;
  ColorField* plot_background_;
  std::array<ColorField*, kPlotSeries> plot_series_{};
  QPushButton* reset_plot_colors_;
  QDoubleSpinBox* confirm_move_ = nullptr;
  LoggingPage* logging_ = nullptr;
  MetricsPage* metrics_ = nullptr;
  EarthbankPreferencesPage* earthbank_ = nullptr;
  DataSourcePage* data_source_ = nullptr;
  Reopen reopen_;
  bool source_written_ = false;  // written by an OK/Apply that went on to fail: still to ask about
  AskReopen ask_reopen_;
  QLabel* problem_;
  QPushButton* save_without_checking_;
  QDialogButtonBox* buttons_;
};

}  // namespace pychron::ui
