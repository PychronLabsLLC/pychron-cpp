#pragma once

// File > Installations: the installs in the site config. Open one, make it
// the one Pychron opens by default, remove it from the list (its folder is
// left alone), or set up a new one with the setup wizard.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <QDialog>

#include "pychron/setup/profile.hpp"
#include "pychron/setup/site.hpp"
#include "setup_wizard.hpp"

class QLabel;
class QListWidget;
class QPushButton;

namespace pychron::ui {

class InstallationsDialog : public QDialog {
  Q_OBJECT

 public:
  // `library` (null: New… is disabled) must outlive the dialog. `current` is
  // the install this window has open.
  InstallationsDialog(std::filesystem::path site_path, const setup::ProfileLibrary* library,
                      SetupWizard::OpenDatabase open_database, std::string current, QWidget* parent = nullptr);

  // After accept(): the install to open.
  const std::optional<std::string>& to_open() const noexcept { return to_open_; }

  // For tests.
  QListWidget* list() const noexcept { return list_; }
  QPushButton* open_button() const noexcept { return open_; }
  QPushButton* default_button() const noexcept { return default_; }
  QPushButton* remove_button() const noexcept { return remove_; }
  QPushButton* new_button() const noexcept { return new_; }
  QLabel* message() const noexcept { return message_; }
  void reload();
  // Runs the wizard modally; tests replace it.
  std::function<std::optional<setup::SiteInstall>(InstallationsDialog&)> run_wizard;

 private:
  std::string selected() const;
  void make_default();
  void remove_selected();
  void update_buttons();

  std::filesystem::path site_path_;
  const setup::ProfileLibrary* library_;
  SetupWizard::OpenDatabase open_database_;
  std::string current_;
  std::optional<std::string> to_open_;
  QListWidget* list_;
  QLabel* message_;
  QPushButton* open_;
  QPushButton* default_;
  QPushButton* remove_;
  QPushButton* new_;
};

}  // namespace pychron::ui
