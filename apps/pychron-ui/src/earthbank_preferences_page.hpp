#pragma once

// File > Preferences, EarthBank: the user's connections to EarthBank, a row
// each. They are kept in the profiles file `elctl earthbank` reads too
// (profiles.hpp), not in the UI's own settings, so that what is set up here
// is what the command line uses. OK and Apply write the file; nothing else
// does.
//
// The password is typed into a field of its own, for the selected row. It is
// never put in the table or the file: it goes to the system's password store,
// when Test signs in with it or on Apply, and the field is then emptied.
// Leaving it empty uses the one already kept.
//
// Nothing is read until the page is first shown: opening Preferences does not
// touch the profiles file or the password store.

#include <cstdint>
#include <memory>
#include <optional>

#include <QPointer>
#include <QString>
#include <QWidget>

#include "earthbank_worker.hpp"
#include "pychron/earthbank/profiles.hpp"
#include "pychron/earthbank/secret_store.hpp"

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace pychron::ui {

class EarthbankPackageDialog;

class EarthbankPreferencesPage : public QWidget {
  Q_OBJECT

 public:
  enum class Column { Active, Name, Server, Username, Institution, Package };

  explicit EarthbankPreferencesPage(EarthbankSetup setup, QWidget* parent = nullptr);
  ~EarthbankPreferencesPage() override;

  // Another service, store and file; only before the page has loaded.
  void set_setup(EarthbankSetup setup);
  // Reads the profiles file and opens the password store, once. Showing the
  // page does it.
  void load();
  bool loaded() const noexcept { return loaded_; }

  // What OK and Apply do: writes the file when the table differs from it,
  // forgets the passwords of profiles that are gone and keeps the one typed.
  // Empty when done (or when there is nothing to do: a page never loaded, or
  // a file that could not be read, which is never written over); else what
  // is wrong, and nothing was written.
  QString apply();

  // For tests.
  QTableWidget* table() const noexcept { return table_; }
  QPushButton* add_button() const noexcept { return add_; }
  QPushButton* remove_button() const noexcept { return remove_; }
  QPushButton* test_button() const noexcept { return test_; }
  QPushButton* package_button() const noexcept { return package_; }
  QLineEdit* password() const noexcept { return password_; }
  QLabel* status() const noexcept { return status_; }
  QLabel* store_line() const noexcept { return store_line_; }
  EarthbankPackageDialog* package_dialog() const;  // null unless open

 protected:
  void showEvent(QShowEvent* event) override;

 private:
  // `saved`: the profile is in the file, so its password, if it has one, is
  // kept under its account.
  int add_row(const earthbank::Profile& profile, bool active, bool saved = false);
  int selected_row() const;
  // The table as profiles; `problem` is set, and the result not to be used,
  // when a cell is not what it should be.
  earthbank::Profiles profiles(QString& problem) const;
  // The selected row as a profile to ask the service with; nothing, and a
  // word in status(), when there is none or it is not one.
  std::optional<earthbank::Profile> selected_profile();
  void say(const QString& what, bool wrong);
  void update_buttons();

  EarthbankSetup setup_;
  bool loaded_ = false;
  bool unreadable_ = false;  // the file is there and could not be read
  earthbank::Profiles saved_;  // what the file holds
  std::shared_ptr<earthbank::ISecretStore> secrets_;
  EarthbankWorker* worker_ = nullptr;
  bool changing_ = false;  // the page itself is ticking Active boxes
  QTableWidget* table_;
  QPushButton* add_;
  QPushButton* remove_;
  QPushButton* test_;
  QPushButton* package_;
  QLineEdit* password_;
  QLabel* store_line_;
  QLabel* status_;
  QPointer<EarthbankPackageDialog> package_dialog_;
};

}  // namespace pychron::ui
