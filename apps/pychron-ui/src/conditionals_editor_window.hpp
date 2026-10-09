#pragma once

// ConditionalsEditorWindow (conditionals-editor design 5): edits the files of
// <lab>/conditionals.
//
//   left    the lab's files, system first (+ new, - delete)
//   right   the open file's conditionals by kind; the selected one's form;
//           the names the file disables; diagnostics (click to go to the row)
//
// One file is open at a time. Errors in a conditional (a check that does not
// compile, a rule of its kind, a duplicate name) are listed at once and keep
// the file from being saved; the lab's catalog (isotopes, detectors, gauges)
// is checked 600 ms after the last change and only warns. Saving writes the
// canonical TOML, so a hand-written file loses its comments (asked first).
// A file that does not parse is shown with the reason and cannot be edited.

#include <functional>
#include <optional>
#include <memory>
#include <set>

#include <QMainWindow>
#include <QSettings>
#include <QTimer>

#include "conditional_form.hpp"
#include "conditional_table_model.hpp"
#include "pychron/experiment/lab/lab.hpp"

class QAction;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QTableView;
class QToolButton;

namespace pychron::ui {

class ConditionalsEditorWindow : public QMainWindow {
  Q_OBJECT

 public:
  enum class Unsaved { Save, Discard, Cancel };
  static constexpr int kCheckDelayMs = 600;

  // `lab` must outlive the window.
  explicit ConditionalsEditorWindow(const experiment::lab::Lab& lab, std::unique_ptr<QSettings> settings = nullptr,
                                    QWidget* parent = nullptr);
  ~ConditionalsEditorWindow() override;

  QStringList file_names() const;  // as listed; a new, unsaved file included
  // Opens a file (asks about unsaved changes first). false on Cancel, a
  // failed save, or a file that cannot be read.
  bool open(const QString& name);
  // Starts an empty file; it is written by the first save. false (with
  // `error`) for a bad or existing name.
  bool new_file(const QString& name, QString* error = nullptr);
  // File > Open: asks which of the lab's files, then opens it as open()
  // does. false on cancel, with no files, or when open() says so.
  bool open_picked();
  // Which of `names` (file_names()) to open; nullopt: cancelled. A dialog by
  // default.
  using PickFile = std::function<std::optional<QString>(const QStringList& names)>;
  void set_pick_file(PickFile pick) { pick_file_ = std::move(pick); }
  bool delete_file(const QString& name, QString* error = nullptr);  // asks first

  QString current_name() const { return current_; }
  bool editable() const { return editable_; }
  QString load_error() const { return load_error_; }
  bool modified() const;
  bool save(QString* error = nullptr);

  ConditionalTableModel& model() noexcept { return model_; }
  ConditionalForm* form() const noexcept { return form_; }
  int add_conditional(experiment::ConditionalKind kind);  // selected; -1 when not editable
  void select_row(int row);
  int current_row() const;
  void add_disable(const QString& name);

  void check_now();
  QStringList diagnostic_lines() const;  // "error: big: ..." as listed
  QString status_text() const;

  // Dialog hooks; defaults are message boxes. Tests replace them.
  void set_ask_unsaved(std::function<Unsaved(const QString& name)> ask) { ask_unsaved_ = std::move(ask); }
  void set_confirm(std::function<bool(const QString& question)> confirm) { confirm_ = std::move(confirm); }
  // Whether the queue open in the experiment window uses a file (said when deleting it).
  void set_referenced(std::function<bool(const QString& name)> referenced) { referenced_ = std::move(referenced); }

 signals:
  void saved(const QString& name);
  // A file was created or deleted.
  void filesChanged();

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  bool resolve_unsaved();  // false: stay on the current file
  void load(const QString& name);
  void show_nothing();
  void fill_files();
  void fill_disable();
  void show_diagnostics(bool with_catalog);
  void update_title();
  void set_editable(bool on);
  void on_selection();

  const experiment::lab::Lab& lab_;
  std::unique_ptr<QSettings> settings_;
  ConditionalTableModel model_;
  QString current_;
  QString new_name_;       // a file that exists only here until saved
  bool editable_ = false;
  QString load_error_;
  bool has_comments_ = false;  // the file on disk has comments a save would drop
  bool selecting_ = false;

  QListWidget* files_;
  QTableView* table_;
  ConditionalForm* form_;
  QListWidget* disable_;
  QListWidget* diagnostics_;
  QLabel* status_;
  QList<QWidget*> edit_controls_;
  QTimer check_timer_;
  std::function<Unsaved(const QString&)> ask_unsaved_;
  std::function<bool(const QString&)> confirm_;
  std::function<bool(const QString&)> referenced_;
  QAction* open_ = nullptr;  // what File > Open does here; enabled while the lab has a file
  PickFile pick_file_;
};

}  // namespace pychron::ui
