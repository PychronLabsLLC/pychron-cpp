#pragma once

// The setup wizard (installation wizard spec section 3.5): a QWizard over the
// setup profiles, doing what `elctl init` does.
//
//   Welcome     what to set up: data reduction, or one of the instruments
//   Location    the install's name and folder (an existing install there of
//               the same profile is updated: its answers are filled in and
//               files the lab edited are kept)
//   one page per question group, skipping groups with nothing to ask; the
//               page asking for a database server has Test connection
//   Ready       what will be written (the commit page: Install)
//   Done        the doctor's checks, and "Open it now"
//
// Install writes the files, creates a local database, records the install in
// the site config (the default when there is none yet) and runs the doctor.

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QWizard>

#include "pychron/core/error.hpp"
#include "pychron/setup/doctor.hpp"
#include "pychron/setup/install.hpp"
#include "pychron/setup/profile.hpp"
#include "pychron/setup/site.hpp"

class QButtonGroup;
class QCheckBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
class QWizardPage;

namespace pychron::ui {

class SetupWizard : public QWizard {
  Q_OBJECT

 public:
  // Opens (create: and migrates) a database URL; the result describes it
  // ("schema version 3"). Empty: databases are neither created nor checked.
  using OpenDatabase = std::function<Result<std::string>(const std::string& url, bool create)>;

  struct Options {
    std::filesystem::path site_path;  // default: setup::default_site_path()
    OpenDatabase open_database;
    QString profile;  // preselected on the Welcome page
    // Connects to the instrument the answers describe (the Instrument
    // connection page's Test connection); default
    // setup::test_instrument_connection, which runs the real drivers.
    std::function<Result<std::string>(const setup::ProfileLibrary&, const setup::ResolvedProfile&,
                                      const setup::Answers&)>
        test_instrument;
  };

  enum Page { kWelcome = 0, kLocation = 1, kFirstGroup = 10, kReady = 900, kDone = 901 };

  // `library` must outlive the wizard.
  SetupWizard(const setup::ProfileLibrary& library, Options options, QWidget* parent = nullptr);

  int nextId() const override;
  bool validateCurrentPage() override;

 protected:
  void initializePage(int id) override;

 public:

  // After Install: what was installed, and whether to open it now.
  const std::optional<setup::SiteInstall>& installed() const noexcept { return installed_; }
  bool open_now() const;
  const std::vector<setup::Check>& checks() const noexcept { return checks_; }

  // For tests.
  void choose(const QString& profile);
  QString chosen() const;
  QLineEdit* name_edit() const noexcept { return name_; }
  QLineEdit* root_edit() const noexcept { return root_; }
  QLabel* location_note() const noexcept { return location_note_; }
  QWidget* editor(const QString& id) const;     // the widget answering question `id`
  bool is_shown(const QString& id) const;       // asked given the answers so far
  QString error_for(const QString& id) const;   // the field's validation message
  QPushButton* test_button() const noexcept { return test_button_; }
  QLabel* test_result() const noexcept { return test_result_; }
  QPushButton* instrument_test_button() const noexcept { return instrument_test_button_; }
  QLabel* instrument_test_result() const noexcept { return instrument_test_result_; }
  QTextBrowser* summary() const noexcept { return summary_; }
  QLabel* ready_error() const noexcept { return ready_error_; }
  QTextBrowser* done_report() const noexcept { return done_report_; }
  // The answers the widgets hold (asked questions only), typed.
  Result<setup::Answers> answers() const;

 private:
  struct Field {
    setup::Question question;
    QWidget* editor = nullptr;
    QLabel* error = nullptr;
    QFormLayout* form = nullptr;
    int group = 0;
    int label_row = -1;  // a table's prompt row above it
  };

  QWizardPage* make_welcome();
  QWizardPage* make_location();
  QWizardPage* make_ready();
  QWizardPage* make_done();
  void rebuild_groups();
  QWidget* make_editor(Field& field);
  void set_value(Field& field, const setup::Value& value);
  Result<setup::Value> value_of(const Field& field) const;
  setup::Answers so_far() const;  // builtins, values and every readable answer
  void refresh_visibility();
  bool group_has_questions(int group) const;
  bool validate_group(int group);
  bool validate_location();
  void prepare_ready();
  bool install();
  void test_connection();
  void test_instrument();
  QPushButton* add_test_row(QFormLayout* form, QLabel*& result, const char* name);
  std::filesystem::path root() const;

  const setup::ProfileLibrary& library_;
  Options options_;
  std::optional<setup::ResolvedProfile> profile_;
  std::vector<std::string> groups_;  // in order of first question
  std::vector<Field> fields_;
  std::map<std::string, std::size_t> field_index_;
  std::optional<setup::InstallRecord> existing_;  // the install already in the folder
  std::optional<setup::InstallPlan> plan_;
  std::optional<setup::SiteInstall> installed_;
  std::vector<setup::Check> checks_;

  QButtonGroup* choices_ = nullptr;
  std::vector<std::string> choice_names_;
  QLineEdit* name_ = nullptr;
  QLineEdit* root_ = nullptr;
  QLabel* location_note_ = nullptr;
  bool name_edited_ = false, root_edited_ = false;
  bool building_ = false;  // rebuild_groups(): editors not all made yet
  QPushButton* test_button_ = nullptr;
  QLabel* test_result_ = nullptr;
  QPushButton* instrument_test_button_ = nullptr;
  QLabel* instrument_test_result_ = nullptr;
  QTextBrowser* summary_ = nullptr;
  QLabel* ready_error_ = nullptr;
  QTextBrowser* done_report_ = nullptr;
  QCheckBox* open_now_ = nullptr;
};

}  // namespace pychron::ui
