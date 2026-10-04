#pragma once

// ConditionalForm (conditionals-editor design 5.2): the fields of one
// conditional. Which fields show depends on the kind (experiment::fields_of):
// start and frequency for kinds evaluated per reading, the count ratio, the
// action and its parameters, resume, and a modification's truncate/terminate.
//
// The form keeps the conditional and changes one field per edit, so values a
// widget cannot show exactly survive. edited() follows every change by the
// user, never set_conditional().

#include <QStringList>
#include <QWidget>

#include "pychron/experiment/conditionals/conditional.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;

namespace pychron::ui {

class ConditionalForm : public QWidget {
  Q_OBJECT

 public:
  // `analysis_types`: the names offered (lowercase); a conditional naming
  // others gets them added to its list.
  explicit ConditionalForm(QStringList analysis_types, QWidget* parent = nullptr);

  void set_conditional(const experiment::Conditional& c);
  const experiment::Conditional& conditional() const noexcept { return c_; }
  QString check_error() const;   // why the check does not compile; empty when it does
  QString action_error() const;  // an action parameter that does not parse

  bool shows_gating() const;
  bool shows_ratio() const;
  bool shows_action() const;
  bool shows_resume() const;
  bool shows_run_flags() const;

 signals:
  void edited(const pychron::experiment::Conditional& c);

 private:
  void show_fields();         // visibility and the action list for c_.kind
  void show_action_params();  // which parameter widgets the action type takes
  void fill_types();
  void update_check_error();
  void change_kind(experiment::ConditionalKind kind);
  void change_action_type(experiment::ActionSpec::Type type);
  void read_steps();
  void notify();

  experiment::Conditional c_;
  QStringList offered_types_;
  bool loading_ = false;

  QLineEdit* name_;
  QComboBox* kind_;
  QLineEdit* check_;
  QLabel* check_error_;
  QWidget* gating_;  // start + frequency
  QSpinBox* start_;
  QSpinBox* frequency_;
  QSpinBox* ntrips_;
  QSpinBox* window_;
  QLineEdit* mapper_;
  QListWidget* types_;
  QWidget* ratio_row_;
  QDoubleSpinBox* ratio_;
  QWidget* action_row_;
  QComboBox* action_;
  QCheckBox* action_quick_;
  QLineEdit* action_name_;
  QLineEdit* action_value_;
  QSpinBox* action_count_;
  QLineEdit* action_steps_;
  QCheckBox* action_percent_;
  QLabel* action_error_;
  QCheckBox* resume_;
  QWidget* run_flag_row_;
  QComboBox* run_flag_;
};

}  // namespace pychron::ui
