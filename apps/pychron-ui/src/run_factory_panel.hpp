#pragma once

// RunFactoryPanel (experiment-window design 5.6): adds runs to the queue.
//
// A form over experiment::FactoryForm: the run (type, identifier, aliquot,
// step), its extraction (device, position, value and units, durations,
// script, step heat) and measurement (plan, post scripts, comment). Fields
// the analysis type does not use are disabled; changing the type applies the
// lab's defaults.toml entry when there is one. A preview line says what Add
// will insert or why it cannot.
//
// Add inserts after the selected rows (or appends), then advances the form by
// the auto-increment settings. Frequency inserts a special run every N
// unknowns (and/or before and after); Block inserts one of the lab's blocks.
// Everything goes through QueueTableModel, so it is revalidated and refused
// while a queue runs.

#include <functional>
#include <optional>
#include <vector>

#include <QWidget>

#include "pychron/experiment/factory/form.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "queue_table_model.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace pychron::ui {

class RunFactoryPanel : public QWidget {
  Q_OBJECT

 public:
  using Selection = std::function<std::vector<std::size_t>()>;

  // `lab` and `model` must outlive the panel. `selection` returns the queue
  // rows selected in the table (sorted).
  RunFactoryPanel(const experiment::lab::Lab& lab, QueueTableModel& model, Selection selection,
                  QWidget* parent = nullptr);

  experiment::FactoryForm form() const;
  void set_form(const experiment::FactoryForm& form);

  // The buttons; each returns whether it changed something.
  bool add();
  bool apply_defaults();
  bool load_from_selection();
  bool insert_frequency();
  bool insert_block();

  void set_increment(int identifier, int position);
  void set_insert_after_selection(bool on);
  // Frequency and block settings, as the widgets hold them.
  void set_frequency(experiment::AnalysisType type, int every, bool before, bool after);
  void set_block(const QString& name, int times);
  void set_locked(bool locked);
  // The lab's conditionals files changed (the editor made or deleted one).
  void refresh_conditionals();
  void set_conditional_checked(const QString& name, bool on);

  // For tests.
  QString preview_text() const;
  bool add_enabled() const;
  bool field_enabled(const char* name) const;  // "value", "position", "script", "plan", ...
  QStringList plan_choices() const;
  QStringList script_choices() const;
  QStringList block_choices() const;
  QStringList conditional_choices() const;
  QString conditionals_text() const;  // the button: the ticked files or "(none)"

 signals:
  // Rows the panel just inserted (sorted), for the window to select.
  void inserted(const std::vector<std::size_t>& rows);

 private:
  QWidget* build_run();
  QWidget* build_extraction();
  QWidget* build_measurement();
  QWidget* build_add();
  QWidget* build_frequency();
  QWidget* build_block();

  std::size_t insert_at() const;
  void refresh();  // field enabling and preview, after any edit
  void on_identifier_changed();
  void report_inserted(std::size_t at, std::size_t count);

  const experiment::lab::Lab& lab_;
  QueueTableModel& model_;
  Selection selection_;
  bool locked_ = false;
  bool updating_ = false;  // set_form in progress: no per-field reactions
  experiment::AnalysisType last_type_ = experiment::AnalysisType::Unknown;
  experiment::FactoryForm overrides_carrier_;  // keeps overrides from defaults or a row
  std::vector<std::string> conditionals_;      // the ticked conditionals files, in the order ticked

  QComboBox* type_ = nullptr;
  QLineEdit* identifier_ = nullptr;
  QLineEdit* aliquot_ = nullptr;
  QLineEdit* step_ = nullptr;
  QLineEdit* device_ = nullptr;
  QLineEdit* position_ = nullptr;
  QCheckBox* per_hole_ = nullptr;
  QSpinBox* identifier_step_ = nullptr;
  QDoubleSpinBox* value_ = nullptr;
  QComboBox* units_ = nullptr;
  QDoubleSpinBox* duration_ = nullptr;
  QDoubleSpinBox* cleanup_ = nullptr;
  QComboBox* script_ = nullptr;
  QLineEdit* step_heat_ = nullptr;
  QComboBox* plan_ = nullptr;
  QComboBox* post_equilibration_ = nullptr;
  QComboBox* post_measurement_ = nullptr;
  QLineEdit* comment_ = nullptr;
  QToolButton* conditionals_button_ = nullptr;
  QLabel* preview_ = nullptr;
  QPushButton* add_ = nullptr;
  QCheckBox* after_selection_ = nullptr;
  QSpinBox* inc_identifier_ = nullptr;
  QSpinBox* inc_position_ = nullptr;
  QComboBox* freq_type_ = nullptr;
  QSpinBox* freq_every_ = nullptr;
  QCheckBox* freq_before_ = nullptr;
  QCheckBox* freq_after_ = nullptr;
  QComboBox* block_ = nullptr;
  QSpinBox* block_times_ = nullptr;
  std::vector<QWidget*> groups_;  // disabled while locked
  QWidget* block_box_ = nullptr;  // stays disabled when the lab has no blocks
};

}  // namespace pychron::ui
