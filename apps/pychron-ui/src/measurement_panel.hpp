#pragma once

// MeasurementPanel (experiment-window design 5.7): the measurement of the
// selected queue row.
//
//   Template   instrument family filter and the plans that fit the row's
//              analysis type; choosing one moves the row to it, keeping the
//              overrides that still apply
//   Parameters one editor per parameter (a check box for booleans, a line
//              edit otherwise) showing the effective value; an overridden
//              value is marked and can be reset to the template's
//   Advanced   lists every value of the template and lets the row override
//              any of them (MeasurementRef::advanced)
//   Status     the plan as loaded with the overrides: its duration, or why it
//              does not load
//
// Edits go straight into the row through QueueTableModel (revalidated; refused
// while a queue runs). The panel follows the table's selection: one selected
// row is edited, anything else shows nothing.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QWidget>

#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/plan/parameters.hpp"
#include "queue_table_model.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;

namespace pychron::ui {

class MeasurementPanel : public QWidget {
  Q_OBJECT

 public:
  using Selection = std::function<std::vector<std::size_t>()>;

  // `lab` and `model` must outlive the panel.
  MeasurementPanel(const experiment::lab::Lab& lab, QueueTableModel& model, Selection selection,
                   QWidget* parent = nullptr);

  // The table's selection changed (or the queue was replaced).
  void refresh();
  std::optional<std::size_t> row() const noexcept { return row_; }

  QStringList families() const;  // "" (all) first
  QStringList plans() const;     // offered for the row, its own plan first when it does not match
  bool set_family(const QString& family);
  bool choose_plan(const QString& plan);
  bool set_advanced(bool on);
  bool advanced() const;

  QStringList parameter_paths() const;
  QString parameter_text(const QString& path) const;  // the effective value
  bool overridden(const QString& path) const;
  // As typing into the editor and pressing Enter: false (nothing changes)
  // when the text does not fit the parameter or the row cannot change.
  bool set_parameter(const QString& path, const QString& text);
  bool reset_parameter(const QString& path);
  bool reset_all();

  QString header_text() const;
  QString status_text() const;
  bool status_ok() const noexcept { return status_ok_; }
  void set_locked(bool locked);

 private:
  struct Editor {
    experiment::plan::PlanParameter param;
    QLabel* label = nullptr;
    QLineEdit* line = nullptr;
    QCheckBox* check = nullptr;
    QLabel* badge = nullptr;
    QPushButton* reset = nullptr;
    bool extra = false;  // an override with no template parameter: no template value to return to
  };

  const experiment::RunSpec* run() const;
  // The row with its measurement replaced; false when the model refuses.
  bool store(experiment::MeasurementRef measurement);
  void rebuild();        // everything, from the row
  void fill_plans();
  void build_parameters();
  void update_marks();   // badges, reset buttons and status, after an edit
  void commit(Editor& e);
  void sync();           // the model changed: rebuild unless it was this panel's edit

  const experiment::lab::Lab& lab_;
  QueueTableModel& model_;
  Selection selection_;
  std::optional<std::size_t> row_;
  experiment::MeasurementRef shown_;  // what the editors show
  bool locked_ = false;
  bool building_ = false;
  bool status_ok_ = false;

  QLabel* header_;
  QComboBox* family_;
  QComboBox* plan_;
  QLabel* description_;
  QCheckBox* advanced_;
  QScrollArea* scroll_;  // holds the current parameter form
  QPushButton* reset_all_;
  QLabel* status_;
  std::map<std::string, Editor> editors_;  // by path
  std::vector<std::string> order_;         // editor paths in display order
};

}  // namespace pychron::ui
