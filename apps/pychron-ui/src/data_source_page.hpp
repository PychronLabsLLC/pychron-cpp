#pragma once

// Preferences > Data source (data source editing spec,
// docs/superpowers/specs/2026-10-18-data-source-editing-design.md, 3.5): the
// database the install reads and writes (a file on this computer, or the
// lab's server) and its data folder, in the setup wizard's own editors
// (question_editor.hpp). Test connection checks without writing; apply()
// checks, then writes the credentials, the install record and the site entry
// (setup::apply_data_source). The store is fixed for the life of the
// process, so the dialog offers to reopen afterwards.
//
// Neither QSettings nor the line's local override file: the source is the
// install's, which elctl reads too, and lives where startup reads it.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <QString>
#include <QWidget>

#include "pychron/core/error.hpp"
#include "pychron/setup/data_source.hpp"
#include "pychron/setup/profile.hpp"
#include "pychron/setup/site.hpp"

class QFormLayout;
class QLabel;
class QPushButton;

namespace pychron::ui {

struct DataSourcePageDeps {
  setup::SiteInstall install;
  std::filesystem::path site_path;              // default_site_path() in the app
  std::vector<setup::Question> questions;       // store-common's, from the profile library
  setup::DataSourceOpeners openers;             // empty members: not checked, not seeded
  std::function<bool()> queue_running;          // empty: never
  std::optional<std::string> session_database;  // --db given
};

// What a window that runs on an install is given to add the page: its
// dependencies, read each time Preferences opens (the install as the site
// file has it then); nullopt: no page.
using DataSourceDepsFactory = std::function<std::optional<DataSourcePageDeps>()>;

class DataSourcePage : public QWidget {
  Q_OBJECT

 public:
  explicit DataSourcePage(DataSourcePageDeps deps, QWidget* parent = nullptr);

  // What the fields hold; the password nullopt when blank (keep the kept one).
  [[nodiscard]] Result<setup::DataSource> source() const;
  // The fields differ from what the install uses, or a password was typed.
  [[nodiscard]] bool changed() const;
  // setup::check_data_source(..., create = false) into result(): nothing written.
  void test_connection();
  // OK/Apply: empty when written or unchanged, else the reason (nothing
  // written then). `check` false skips the checks ("Save without checking").
  QString apply(bool check = true);
  // queue_running() true: every field read-only, and the note says why.
  void refresh_read_only();
  // The install as it is after the last apply().
  [[nodiscard]] const setup::SiteInstall& install() const noexcept { return deps_.install; }

  // For tests.
  [[nodiscard]] QWidget* editor(const QString& id) const;  // "data_source", "db_host", ..., "db_file", "data_folder"
  [[nodiscard]] QPushButton* test_button() const noexcept { return test_button_; }
  [[nodiscard]] QLabel* result() const noexcept { return result_; }
  [[nodiscard]] QLabel* note() const noexcept { return note_; }

 protected:
  void showEvent(QShowEvent* event) override;  // read-only again if a queue started meanwhile

 private:
  struct Field {
    setup::Question question;
    QWidget* editor = nullptr;
    QWidget* cell = nullptr;  // the form row's field: the editor and its buttons
  };

  [[nodiscard]] const Field* field(const std::string& id) const;
  [[nodiscard]] setup::Answers so_far() const;
  void show_source(const setup::DataSource& source);
  void show_kept_password();
  void refresh_visibility();
  void say(const std::vector<std::string>& lines, bool ok);

  DataSourcePageDeps deps_;
  std::optional<setup::DataSource> current_;  // what the install uses; nullopt when it could not be read
  std::vector<Field> fields_;
  QFormLayout* form_ = nullptr;
  QPushButton* test_button_ = nullptr;
  QLabel* result_ = nullptr;
  QLabel* note_ = nullptr;
  bool building_ = true;
};

}  // namespace pychron::ui
