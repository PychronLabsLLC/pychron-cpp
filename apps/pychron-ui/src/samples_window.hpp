#pragma once

// Entry > Samples (entry spec, section 9.1): browse, edit, add and delete
// samples. Edits stay in the table until Save, which writes them as one batch;
// rows another client changed meanwhile come back tinted and nothing is
// written.

#include <optional>

#include <QMainWindow>
#include <QPointer>

#include "entry_bridge.hpp"
#include "sample_table_model.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QTableView;

namespace pychron::ui {

class SampleImportDialog;

class SamplesWindow : public QMainWindow {
  Q_OBJECT

 public:
  // `bridge` must outlive the window.
  explicit SamplesWindow(EntryBridge& bridge, QWidget* parent = nullptr);

  SampleTableModel* model() const noexcept { return model_; }
  QTableView* table() const noexcept { return table_; }
  QString message() const;
  bool busy() const noexcept { return busy_ > 0; }

  // Reads the catalog and the samples the filters select (asynchronous).
  void reload();
  // Writes the edits (asynchronous); a no-op without edits.
  void save();
  // Stages the sample in the new-sample form; false (with a message) when it
  // is incomplete.
  bool add_from_form();
  void set_form(const NewSample& sample);  // tests
  SampleImportDialog* open_import();

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void apply_loaded(entry::CatalogSnapshot catalog, std::vector<persistence::SampleRow> rows,
                    entry::EntrySettings settings);
  void fill_filter(QComboBox* box, const std::vector<std::pair<QString, persistence::Uuid>>& items);
  void show_message(const QString& text, bool error = false);
  void check_duplicates();

  EntryBridge& bridge_;
  SampleTableModel* model_;
  QTableView* table_;
  QComboBox *pi_filter_, *project_filter_, *material_filter_;
  QLineEdit* search_;
  // The new-sample form.
  QLineEdit *new_name_, *new_grainsize_, *new_lat_, *new_lon_, *new_note_;
  QComboBox *new_pi_, *new_project_, *new_material_;
  QLabel* duplicates_;
  QLabel* message_;
  entry::CatalogSnapshot catalog_;
  entry::EntrySettings settings_;
  int busy_ = 0;
  QPointer<SampleImportDialog> import_;
};

}  // namespace pychron::ui
