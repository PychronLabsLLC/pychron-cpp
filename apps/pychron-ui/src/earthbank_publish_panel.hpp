#pragma once

// Publish to EarthBank, as a widget: the selected analyses, a row for each
// sample, and three things to do with them:
//
//   Pre-flight   signs in and asks EarthBank whether it knows every term the
//                upload would send; what it does not know is written in the
//                rows it belongs to.
//   Export xlsx  writes EarthBank's own import workbooks into a folder, for
//                the website. Needs no account.
//   Upload       shows who would upload what into which package, and on Yes
//                sends it, a record at a time. A record on EarthBank cannot
//                be changed or removed from here, so nothing is sent before
//                that Yes, and a record whose answer never came is not sent
//                again until the user has said whether it is there.
//
// A row says what the sample would report: each kind of age it can compute,
// with the one the rule picks first (mapping.hpp), and the flux monitor found
// on its analyses. The interpretation, the kind of age and the monitor can be
// changed, and are remembered by sample name in the UI's settings, not on
// EarthBank. A sample whose analyses disagree about what it is is shown with
// the reason and left out; the others go on.
//
// Everything that computes or talks runs on the worker's thread
// (earthbank_worker.hpp). Destroying the panel during an upload stops it after
// the record that is out, as Cancel does: the journal has what was written,
// and the next upload of the same analyses finishes it.
//
// The panel has no Run button: its host calls start(), and EarthbankPublishDialog
// (earthbank_publish_dialog.hpp) is the host that adds one. finished() says
// how each start ended; progress() how far it is.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QWidget>
#include <QPointer>
#include <QSettings>
#include <QString>

#include "earthbank_worker.hpp"
#include "pychron/earthbank/preflight.hpp"
#include "pychron/earthbank/profiles.hpp"
#include "pychron/earthbank/secret_store.hpp"
#include "pychron/earthbank/uploader.hpp"
#include "pychron/processing/model.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QWidget;

namespace pychron::ui {

class EarthbankPackageDialog;

class EarthbankPublishPanel : public QWidget {
  Q_OBJECT

 public:
  enum class Action { Upload, ExportXlsx, Preflight };
  enum class Column { Sample, Analyses, Interpretation, PreferredAge, FluxMonitor, Notes };
  using SettingsFactory = std::function<std::unique_ptr<QSettings>()>;

  // `settings`: where the choices are remembered (empty: the application's
  // QSettings), under earthbank/.
  EarthbankPublishPanel(std::vector<processing::AnalysisPtr> analyses, EarthbankSetup setup, SettingsFactory settings,
                         QWidget* parent = nullptr);
  ~EarthbankPublishPanel() override;

  // The analyses to publish, when they were not known at construction (a
  // release made while the panel is open): the rows are surveyed again. Ignored
  // while a run is under way.
  void set_analyses(std::vector<processing::AnalysisPtr> analyses);

  // Sets the action and runs it, as Run does. finished() follows every start,
  // once: when a run is under way ("Already running."), the rows are not in
  // ("Not ready.") or the action cannot run as things stand (no profile, no
  // folder, no sample to publish: the hint) nothing runs and it is
  // finished(false, the reason) at once.
  void start(Action action);
  // Stops an upload after the record that is out, as the Cancel button does.
  void cancel();
  // The per-sample cells and the options cannot be edited; the result pane
  // and the buttons that only read are unchanged.
  void set_read_only(bool read_only);
  bool read_only() const noexcept { return read_only_; }

  // Whether a start would run now: the rows are in, nothing is running or out,
  // and nothing in the fields keeps the action from running (hint() empty).
  bool can_run() const;
  Action current_action() const;

  // The rows are in, nothing is running and no request is out.
  bool ready() const;
  // A run (a check, an export, an upload) is under way.
  bool busy() const noexcept { return running_; }

  // Asked before an upload sends anything, with who, where and how much;
  // false sends nothing. Tests replace the message box.
  std::function<bool(const QString& text)> ask_confirm;
  // Shows a folder in the system's file browser; tests replace it.
  std::function<void(const QString& folder)> show_folder;

  // For tests.
  EarthbankWorker* worker() const noexcept { return worker_; }
  QComboBox* action() const noexcept { return action_; }  // item data: Action
  QComboBox* profile() const noexcept { return profile_; }
  QLineEdit* package() const noexcept { return package_; }
  QPushButton* package_button() const noexcept { return package_button_; }
  EarthbankPackageDialog* package_dialog() const;  // null unless open
  QLineEdit* password() const noexcept { return password_; }
  QLabel* hint() const noexcept { return hint_; }  // why Run is off, when it is not plain
  QTableWidget* table() const noexcept { return table_; }
  QComboBox* preferred_age(int row) const;  // item 0: the rule; 1 to 4: AgeKind in order
  QLineEdit* setup_name() const noexcept { return setup_name_; }
  QLineEdit* decay_constant() const noexcept { return decay_constant_; }
  QLineEdit* air_ratio() const noexcept { return air_ratio_; }
  QLineEdit* folder() const noexcept { return folder_; }
  QCheckBox* per_sample() const noexcept { return per_sample_; }
  QCheckBox* include_samples() const noexcept { return include_samples_; }
  QCheckBox* allow_unresolved() const noexcept { return allow_unresolved_; }
  QCheckBox* new_datapoint() const noexcept { return new_datapoint_; }
  QPushButton* cancel_button() const noexcept { return cancel_; }
  QPushButton* show_folder_button() const noexcept { return show_folder_; }
  QLineEdit* in_doubt_id() const noexcept { return in_doubt_id_; }
  QPushButton* there_button() const noexcept { return there_; }
  QPushButton* not_there_button() const noexcept { return not_there_; }
  QProgressBar* progress_bar() const noexcept { return progress_; }
  QPlainTextEdit* result_pane() const noexcept { return result_; }

 signals:
  // How far the run is; the survey is not one.
  void progress(int done, int total);
  // A start is over: ok is false when it failed, was stopped, was refused or
  // was declined; message is the last line the result pane shows.
  void finished(bool ok, const QString& message);
  // What can_run(), the hint or the buttons say may have changed.
  void controls_changed();

 private:
  struct Row {
    EarthbankSampleRow found;
    QString base_note;  // why it is left out, or empty
  };

  std::optional<earthbank::Profile> current_profile() const;
  // Whether row `row` takes part, and when it does not, why.
  bool included(int row, QString* why = nullptr) const;
  // The analyses of the rows taking part and what the fields say of them.
  EarthbankPublication publication() const;
  // What row `row` says replaces the monitor found on its analyses; empty
  // when its cell holds that monitor or nothing.
  QString flux_monitor_override(int row) const;
  earthbank::PublishOptions base_options() const;
  void fill(const std::vector<EarthbankSampleRow>& rows);
  void remember() const;
  void recall_fields();
  void begin(std::optional<std::string> resolve_in_doubt = std::nullopt);
  void end_run(bool ok);
  void survey_all();
  // Hides the in-doubt answer: what it would finish is no longer what is set.
  void withdraw_in_doubt();
  void say(const QString& line);
  void show_preflight(const earthbank::PreflightReport& report);
  void show_upload(const earthbank::UploadReport& report);
  void update_controls();
  QString hint_text() const;
  std::unique_ptr<QSettings> settings() const;

  std::vector<processing::AnalysisPtr> analyses_;
  EarthbankSetup setup_;
  SettingsFactory settings_;
  earthbank::Profiles profiles_;
  std::shared_ptr<earthbank::ISecretStore> secrets_;
  EarthbankWorker* worker_;
  std::vector<Row> rows_;
  bool surveyed_ = false;
  bool running_ = false;
  bool read_only_ = false;
  bool agreed_ = false;       // this run finishes an upload already agreed to
  QString last_folder_;       // of the last export that wrote something
  QString last_line_;         // the last line the result pane was given

  QComboBox* action_;
  QComboBox* profile_;
  QLineEdit* package_;
  QPushButton* package_button_;
  QLineEdit* password_;
  QLabel* hint_;
  QTableWidget* table_;
  QLineEdit* setup_name_;
  QLineEdit* decay_constant_;
  QLineEdit* air_ratio_;
  QWidget* export_box_;
  QLineEdit* folder_;
  QPushButton* browse_;
  QCheckBox* per_sample_;
  QCheckBox* include_samples_;
  QWidget* upload_box_;
  QCheckBox* allow_unresolved_;
  QCheckBox* new_datapoint_;
  QPushButton* cancel_;
  QPushButton* show_folder_;
  QWidget* in_doubt_box_;
  QLabel* in_doubt_label_;
  QLineEdit* in_doubt_id_;
  QPushButton* there_;
  QPushButton* not_there_;
  QProgressBar* progress_;
  QPlainTextEdit* result_;
  QPointer<EarthbankPackageDialog> package_dialog_;
};

}  // namespace pychron::ui
