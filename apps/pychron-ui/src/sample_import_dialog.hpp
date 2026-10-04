#pragma once

// Entry > Import Samples (entry spec, section 9.2): a CSV file or rows pasted
// from a spreadsheet, a column mapping, a preview of what each row would do,
// and one write for the lot. The same planning as `elctl entry samples import`.

#include <optional>

#include <QDialog>

#include "entry_bridge.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace pychron::ui {

class SampleImportDialog : public QDialog {
  Q_OBJECT

 public:
  SampleImportDialog(EntryBridge& bridge, QWidget* parent = nullptr);

  // The text to import (tests; the Open… button and paste fill it too).
  void set_text(const QString& text);
  // Parses the text, maps its columns and plans against the catalog.
  // Blocks briefly on the store for the catalog.
  bool preview();
  // Writes the plan (asynchronous); `finished(true)` when it applied.
  void import_rows();

  const std::optional<entry::SampleImportPlan>& plan() const noexcept { return plan_; }
  QString message() const;
  QTableWidget* preview_table() const noexcept { return preview_; }
  QComboBox* filter() const noexcept { return filter_; }

 Q_SIGNALS:
  void imported(bool ok);

 private:
  void fill_mapping();
  void fill_preview();
  void show_message(const QString& text, bool error = false);
  entry::ColumnMapping mapping() const;

  EntryBridge& bridge_;
  QPlainTextEdit* text_;
  QTableWidget* mapping_;
  QTableWidget* preview_;
  QComboBox* filter_;
  QCheckBox* update_existing_;
  QPushButton* import_;
  QLabel* message_;
  std::optional<entry::CsvTable> table_;
  std::optional<entry::SampleImportPlan> plan_;
  entry::CatalogSnapshot catalog_;
  entry::EntrySettings settings_;
};

}  // namespace pychron::ui
