#pragma once

// FieldTableModel (field table editor spec, section 3.1): the editor's grid
// over an edit copy of one version of a field table.
//
//   Isotope | Mass | one column per detector
//
// Detector columns are the config's detectors in config order, then any
// detector only the table has (header marked "not in config"); the set is
// fixed when a table is loaded. Rows are the table's points, kept sorted by
// mass (lightest first) after every edit. An empty cell is no value for that
// detector.
//
// Every edit is a QUndoCommand holding the whole table before and after:
// tables are tens of rows, and a copy keeps undo exact. modified() is the
// undo stack's clean state.

#include <optional>
#include <string>
#include <vector>

#include <QAbstractTableModel>
#include <QUndoStack>

#include "pychron/core/error.hpp"
#include "pychron/systems/spectrometer/field_table.hpp"
#include "pychron/systems/spectrometer/molecular_weights.hpp"

namespace pychron::ui {

class FieldTableModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  static constexpr int kIsotopeColumn = 0;
  static constexpr int kMassColumn = 1;
  static constexpr int kFirstDetectorColumn = 2;

  FieldTableModel(std::vector<std::string> config_detectors, spectrometer::MolecularWeights weights,
                  QObject* parent = nullptr);

  // Replaces the table (sorted by mass), clears the undo stack and marks it
  // clean.
  void set_table(spectrometer::FieldTable table);
  const spectrometer::FieldTable& table() const noexcept { return table_; }
  const spectrometer::MolecularWeights& weights() const noexcept { return weights_; }

  QUndoStack* undo_stack() noexcept { return &undo_; }
  bool modified() const { return !undo_.isClean(); }
  void mark_saved() { undo_.setClean(); }

  // A read-only model refuses every edit (a version that is shown but
  // cannot be saved).
  void set_read_only(bool on);
  bool read_only() const noexcept { return read_only_; }

  const std::vector<std::string>& detector_columns() const noexcept { return columns_; }
  bool in_config(const std::string& detector) const;
  int column_of(const std::string& detector) const;  // -1 if absent
  // The detector of a column; empty for Isotope, Mass or out of range.
  std::string detector_at(int column) const;

  // Mass from the weights, else 0 (diagnostics then flag it); no values.
  void add_row(const QString& isotope);
  void remove_rows(std::vector<int> rows);
  // FieldTable::update on the edit copy: peak center's semantics. Refused
  // (nothing pushed) for a non-finite value, an unknown isotope or a
  // detector the table has no value for.
  Result<void> set_value(const std::string& det, const std::string& isotope, double value, bool propagate);
  void set_default_fit(spectrometer::FitKind kind);
  // nullopt: the detector uses the table default.
  void set_detector_fit(const std::string& det, std::optional<spectrometer::FitKind> kind);

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

 signals:
  // After any change of table() (an edit, an undo, a redo, set_table).
  void tableEdited();

 private:
  class Edit;

  // Pushes an undoable change from the current table to `after` (no-op
  // when they are equal).
  void push(spectrometer::FieldTable after, const QString& text);
  // Installs `table` (from an undo command), telling the views.
  void apply(spectrometer::FieldTable table);
  bool set_isotope(std::size_t row, const QString& text);

  std::vector<std::string> config_detectors_;
  spectrometer::MolecularWeights weights_;
  spectrometer::FieldTable table_;
  std::vector<std::string> columns_;
  QUndoStack undo_;
  bool read_only_ = false;
};

// Points sorted by mass, lightest first; equal masses keep their order.
void sort_by_mass(spectrometer::FieldTable& table);

}  // namespace pychron::ui
