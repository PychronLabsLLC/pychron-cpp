#pragma once

// Dialogs of the field table editor (field table editor spec, 3.1 and 3.4):
// Compare (the difference of two versions by isotope), Set Value (legacy
// "Define Peak Center…": FieldTable::update with Propagate), Add Row.

#include <optional>
#include <string>
#include <vector>

#include <QDialog>
#include <QString>

#include "pychron/systems/spectrometer/field_table_check.hpp"

class QCheckBox;
class QComboBox;
class QLineEdit;

namespace pychron::ui {

// Fit and override changes listed above; below, one row per difference:
// Isotope, Detector, Old, New, Δ. A row only one side has is marked
// "row added" / "row removed"; a changed mass has detector "mass".
class FieldTableCompareDialog : public QDialog {
  Q_OBJECT

 public:
  FieldTableCompareDialog(const spectrometer::TableDiff& diff, const QString& old_version,
                          const QString& new_version, QWidget* parent = nullptr);

  int row_count() const noexcept { return rows_; }

 private:
  int rows_ = 0;
};

// Two versions side by side as text, for a version that does not parse.
QDialog* make_text_compare_dialog(const QString& old_title, const QString& old_text, const QString& new_title,
                                  const QString& new_text, QWidget* parent);

class FieldTableSetValueDialog : public QDialog {
  Q_OBJECT

 public:
  // `isotopes` offered; free text is allowed too.
  FieldTableSetValueDialog(const std::vector<std::string>& detectors, const std::vector<std::string>& isotopes,
                           bool propagate, QWidget* parent = nullptr);

  std::string detector() const;
  std::string isotope() const;  // trimmed
  // nullopt: the text is not a finite number.
  std::optional<double> value() const;
  bool propagate() const;

  void set_detector(const std::string& detector);
  void set_isotope(const std::string& isotope);
  void set_value(double value);
  void set_propagate(bool on);

 private:
  QComboBox* detector_;
  QComboBox* isotope_;
  QLineEdit* value_;
  QCheckBox* propagate_;
};

class FieldTableAddRowDialog : public QDialog {
  Q_OBJECT

 public:
  // `names`: the molecular weights' names; free text is allowed too.
  explicit FieldTableAddRowDialog(const std::vector<std::string>& names, QWidget* parent = nullptr);

  QString isotope() const;  // trimmed
  void set_isotope(const QString& isotope);

 private:
  QComboBox* isotope_;
};

}  // namespace pychron::ui
