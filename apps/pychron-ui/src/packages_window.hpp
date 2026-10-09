#pragma once

// Entry > Packages (entry spec, section 9.3): packages (an irradiation is a
// package of kind irradiation) and their levels on the left, the positions of
// one level in the middle, and docks to pick samples, edit the level and the
// chronology, and see the holder. Edits stay in the grid until Save, which
// writes the level's catalog rows and its z and production in one changeset.

#include <functional>
#include <optional>
#include <set>
#include <utility>

#include <QMainWindow>
#include <QPointer>
#include <QStringList>

#include "entry_bridge.hpp"
#include "level_grid_model.hpp"

class QAction;
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
  // Selects the level of that name in the tree and opens it, as a click on it
  // does; once the tree is read, when it is being read. A level already open
  // is only selected.
  void show_level(const QString& irradiation, const QString& level);
  // "Fit flux…": enabled while a level is open; asks with flux_requested.
  QAction* fit_flux_action() const noexcept { return fit_flux_; }
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

 Q_SIGNALS:
  // "Fit flux…" was chosen on the open level.
  void flux_requested(const QString& irradiation, const QString& level);

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  struct LevelData;
  static QStringList dose_texts(const persistence::Dose& dose);  // a row of the dose table
  // The dose table differs from the stored chronology (it is written only by
  // "Save Chronology"), or a cell of it is being typed in.
  bool chronology_edited() const;
  bool level_fields_edited() const;              // z or the note is typed and not yet in the grid's edit
  // Something here would be lost by reading the level again.
  bool unsaved() const;
  void notify_changed();                         // bridge_.notify_changed(), as this window's own
  bool show_wanted();                            // false when the tree has no such level
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
  HolderView* holder_view_ = nullptr;
  QLabel* message_;
  // Level dock.
  QComboBox *kind_ = nullptr, *holder_ = nullptr, *production_ = nullptr;
  QLineEdit *z_ = nullptr, *level_note_ = nullptr;
  // Chronology dock.
  QTableWidget* doses_ = nullptr;
  QLabel* hours_ = nullptr;
  QWidget* chronology_page_ = nullptr;
  // Sample picker.
  QComboBox *pick_pi_ = nullptr, *pick_project_ = nullptr;
  QLineEdit* pick_search_ = nullptr;
  QListWidget* pick_list_ = nullptr;

  std::vector<persistence::IrradiationRow> packages_;
  std::optional<persistence::Uuid> package_, level_;
  std::vector<persistence::RefObjectRow> holders_;
  std::map<persistence::Uuid, persistence::HolderValue> holder_values_;
  std::vector<entry::NamedProduction> productions_;
  entry::PackageChronology chronology_;
  entry::CatalogSnapshot catalog_;
  entry::EntrySettings settings_;
  QAction* fit_flux_ = nullptr;
  QString level_name_;                           // of level_
  std::optional<std::pair<QString, QString>> wanted_;  // show_level()'s, until the tree is read
  std::optional<std::set<int>> reselect_;        // the selection to put back after the read a change elsewhere started
  int tree_jobs_ = 0;                            // tree reads outstanding
  int level_jobs_ = 0;                           // level reads outstanding
  int busy_ = 0;
  bool syncing_ = false;
  bool notifying_ = false;                       // the bridge's changed() is this window's own
};

}  // namespace pychron::ui
