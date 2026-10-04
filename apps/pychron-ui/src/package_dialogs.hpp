#pragma once

// The dialogs of the Packages window (entry spec, section 9.3) and the
// holders and entry settings dialogs of the Entry menu (9.4, section 7).

#include <optional>
#include <set>
#include <vector>

#include <QDialog>

#include "entry_bridge.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;

namespace pychron::ui {

class HolderView;

// Which fields Clear Fields blanks on the selected positions.
class ClearFieldsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit ClearFieldsDialog(QWidget* parent = nullptr);
  std::set<entry::SheetField> fields() const;

 private:
  QCheckBox *sample_, *weight_, *packet_, *note_;
};

// New Package: kind, name (the next with the lab's prefix), levels, and for
// an irradiation the chronology and reactor. Written in one transaction.
class NewPackageDialog : public QDialog {
  Q_OBJECT
 public:
  NewPackageDialog(EntryBridge& bridge, QWidget* parent = nullptr);
  void accept() override;  // creates the package; stays open with a message on failure

  // The values the dialog would create (tests fill the form through these).
  QLineEdit* name() const noexcept { return name_; }
  QComboBox* kind() const noexcept { return kind_; }
  QLineEdit* levels() const noexcept { return levels_; }
  QComboBox* reactor() const noexcept { return reactor_; }
  QTableWidget* doses() const noexcept { return doses_; }
  QString message() const;
  std::optional<persistence::Uuid> first_level() const noexcept { return first_level_; }

 private:
  void update_kind();

  EntryBridge& bridge_;
  QLineEdit *name_, *levels_, *z_;
  QComboBox *kind_, *holder_, *reactor_;
  QTableWidget* doses_;
  QWidget* irradiation_part_;
  QLabel* message_;
  std::vector<persistence::RefObjectRow> holders_;
  std::map<std::string, persistence::ProductionValue> reactors_;
  std::optional<persistence::Uuid> first_level_;
};

// New Level of the current package: name, holder, z and production
// pre-filled from the level in view (package_level_editor.py:161-178).
class NewLevelDialog : public QDialog {
  Q_OBJECT
 public:
  NewLevelDialog(EntryBridge& bridge, persistence::IrradiationRow package, const entry::NewLevel& defaults,
                 std::optional<persistence::Uuid> production, const std::vector<persistence::RefObjectRow>& holders,
                 const std::vector<entry::NamedProduction>& productions, QWidget* parent = nullptr);
  void accept() override;
  std::optional<persistence::Uuid> level() const noexcept { return level_; }

 private:
  EntryBridge& bridge_;
  persistence::IrradiationRow package_;
  QLineEdit *name_, *z_, *note_;
  QComboBox *holder_, *production_;
  QLabel* message_;
  std::optional<persistence::Uuid> level_;
};

// The interference ratios of one production of a package, saved as its next
// revision; a new production by name; copy from a reactor default.
class ProductionDialog : public QDialog {
  Q_OBJECT
 public:
  ProductionDialog(EntryBridge& bridge, persistence::IrradiationRow package,
                   std::vector<entry::NamedProduction> productions, const QString& current, QWidget* parent = nullptr);
  void accept() override;

 private:
  void show_production(int index);

  EntryBridge& bridge_;
  persistence::IrradiationRow package_;
  std::vector<entry::NamedProduction> productions_;
  std::map<std::string, persistence::ProductionValue> reactors_;
  QComboBox *which_, *reactor_;
  QLineEdit* new_name_;
  QTableWidget* ratios_;
  QLabel* message_;
};

// Generate Identifiers: the plan for every level of the package, then one
// allocation. A counter that moved meanwhile re-plans and shows the new plan.
class IdentifierDialog : public QDialog {
  Q_OBJECT
 public:
  IdentifierDialog(EntryBridge& bridge, persistence::IrradiationRow package, QWidget* parent = nullptr);

  // Plans again (blocks briefly on the store).
  bool replan();
  void commit();  // asynchronous; `allocated` when written
  const entry::IdentifierPlan& plan() const noexcept { return plan_; }
  QCheckBox* overwrite() const noexcept { return overwrite_; }
  QTableWidget* preview() const noexcept { return preview_; }
  QString message() const;

 Q_SIGNALS:
  void allocated();

 private:
  EntryBridge& bridge_;
  persistence::IrradiationRow package_;
  QCheckBox* overwrite_;
  QListWidget* warnings_;
  QTableWidget* preview_;
  QLabel* message_;
  entry::IdentifierPlan plan_;
};

// Entry > Holders: the irradiation holders with a drawing; import a legacy file.
class HoldersDialog : public QDialog {
  Q_OBJECT
 public:
  explicit HoldersDialog(EntryBridge& bridge, QWidget* parent = nullptr);
  void reload();
  Result<void> import_text(const QString& name, const QString& text);
  QListWidget* list() const noexcept { return list_; }

 private:
  EntryBridge& bridge_;
  QListWidget* list_;
  HolderView* view_;
  QLabel* message_;
  std::vector<persistence::RefObjectRow> holders_;
  std::map<persistence::Uuid, persistence::HolderValue> values_;
};

// Entry > Entry Settings: the lab's settings document (section 7).
class EntrySettingsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit EntrySettingsDialog(EntryBridge& bridge, QWidget* parent = nullptr);
  void accept() override;

 private:
  EntryBridge& bridge_;
  entry::LoadedSettings loaded_;
  QLineEdit *prefix_, *pi_names_, *monitor_sample_, *monitor_material_, *project_prefix_, *j_multiplier_;
  QComboBox *default_kind_, *null_rows_;
  QCheckBox* create_project_;
  QLabel* message_;
};

}  // namespace pychron::ui
