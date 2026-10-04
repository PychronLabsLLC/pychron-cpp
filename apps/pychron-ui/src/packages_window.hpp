#pragma once

// Entry > Packages (entry spec, section 9.3): packages (an irradiation is a
// package of kind irradiation) and their levels on the left, the positions of
// one level in the middle, and docks to pick samples, edit the level and the
// chronology, and see the holder. Edits stay in the grid until Save, which
// writes the level's catalog rows and its z and production in one changeset.

#include <functional>
#include <optional>
#include <set>

#include <QMainWindow>
#include <QPointer>

#include "entry_bridge.hpp"
#include "level_grid_model.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QTableView;
class QTableWidget;
class QTreeWidget;

namespace pychron::ui {

class HolderView;

class PackagesWindow : public QMainWindow {
  Q_OBJECT

 public:
  // `bridge` must outlive the window.
  explicit PackagesWindow(EntryBridge& bridge, QWidget* parent = nullptr);

  // How the window asks a yes/no question (tests answer without a dialog).
  void set_confirm(std::function<bool(const QString&)> confirm) { confirm_ = std::move(confirm); }

  LevelGridModel* grid() const noexcept { return grid_; }
  QTableView* table() const noexcept { return table_; }
  QTreeWidget* tree() const noexcept { return tree_; }
  HolderView* holder_view() const noexcept { return holder_view_; }
  QString message() const;
  bool busy() const noexcept { return busy_ > 0; }
  const std::vector<persistence::IrradiationRow>& packages() const noexcept { return packages_; }
  std::optional<persistence::IrradiationRow> current_package() const;

  void reload();                                 // the package tree (asynchronous)
  void open_level(persistence::Uuid level);      // asynchronous; asks to save unsaved edits first
  void save();                                   // asynchronous
  void revert();
  void select_positions(const std::set<int>& positions);
  std::vector<int> selected_positions() const;
  // Puts the sample on the selected positions; asks first when any is analyzed.
  void assign(const persistence::SampleRow& sample);
  void clear_fields(const std::set<entry::SheetField>& fields);
  void fill_packets(const QString& first);
  Result<int> import_positions(const QString& csv);
  Result<void> export_csv(const QString& path);
  Result<int> save_pdf(const QString& path);
  void set_kind(const QString& kind);
  void open_identifiers();
  void new_package();
  void new_level();

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  struct LevelData;
  void build_docks();
  void apply_level(LevelData data);
  void fill_level_dock();
  void fill_chronology();
  void refresh_picker();
  void show_message(const QString& text, bool error = false);
  bool ask(const QString& question);
  // False when the user cancelled; saves or drops unsaved edits otherwise.
  bool settle_edits();
  std::optional<persistence::HolderValue> holder_value(std::optional<persistence::Uuid> holder) const;

  EntryBridge& bridge_;
  std::function<bool(const QString&)> confirm_;
  LevelGridModel* grid_;
  QTableView* table_;
  QTreeWidget* tree_;
  HolderView* holder_view_;
  QLabel* message_;
  // Level dock.
  QComboBox *kind_, *holder_, *production_;
  QLineEdit *z_, *level_note_;
  // Chronology dock.
  QTableWidget* doses_;
  QLabel* hours_;
  QWidget* chronology_page_;
  // Sample picker.
  QComboBox *pick_pi_, *pick_project_;
  QLineEdit* pick_search_;
  QListWidget* pick_list_;

  std::vector<persistence::IrradiationRow> packages_;
  std::optional<persistence::Uuid> package_, level_;
  std::vector<persistence::RefObjectRow> holders_;
  std::map<persistence::Uuid, persistence::HolderValue> holder_values_;
  std::vector<entry::NamedProduction> productions_;
  entry::PackageChronology chronology_;
  entry::CatalogSnapshot catalog_;
  entry::EntrySettings settings_;
  int busy_ = 0;
  bool syncing_ = false;
};

}  // namespace pychron::ui
