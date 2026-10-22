#pragma once

// The Publish dialog (publication workflow design, section 6.1): one release
// (new from a selection, or one of the store's) and the destinations it is
// sent to, each with its own tab, in the fixed order they run. The plan the
// dialog shows is checked against the store on every change (check_plan, on
// the bridge's worker, 200 ms after the last change); Run is off while
// anything refuses it.
//
// Run makes the release first when it is new (nothing else runs when that
// fails), then each ticked destination in run_order, one result row each. The
// steps are sequenced on the GUI thread, each started by the result of the
// one before: a store step (create, build, publish, the upload recheck,
// release_dataset) is one job on the bridge's worker, EarthBank is the panel's
// start() and its finished(). A step whose dependency failed is Skipped;
// Cancel lets the step that is out finish (a build cannot be interrupted, an
// upload stops after the record that is out) and marks the rest Cancelled.
// Nothing is undone.
//
// Every store call runs on the bridge's worker; its result comes back queued
// to the GUI thread and never reaches a dialog that is gone (EntryBridge::run
// checks a QPointer to it). A result that was overtaken by a later change is
// dropped. The EarthBank tab is made when it is first shown, not before: the
// dialog reads nothing of EarthBank (profiles, keychain) before it is shown.
//
// Message boxes and file dialogs are hooks tests replace.

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QDialog>
#include <QSettings>
#include <QString>
#include <QStringList>

#include "earthbank_worker.hpp"

// Qt's `signals` keyword macro would rewrite persistence::CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/persistence/ids.hpp"
#include "pychron/publish/publication.hpp"
#include "pychron/publish/release.hpp"
#pragma pop_macro("signals")

class QCheckBox;
class QCloseEvent;
class QGroupBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QTabWidget;
class QTableWidget;
class QTimer;
class QWidget;

namespace pychron::processing {
class StoreSource;
}

namespace pychron::ui {

class EarthbankPublishPanel;
class EntryBridge;

class PublishDialog : public QDialog {
  Q_OBJECT

 public:
  using SettingsFactory = std::function<std::unique_ptr<QSettings>()>;  // as EarthbankPublishPanel's

  // `settings`: where the destinations ticked last are remembered (empty: the
  // application's QSettings), under publish/; the EarthBank tab remembers its
  // own under earthbank/.
  PublishDialog(EntryBridge& bridge, processing::StoreSource& source, std::string store_url, EarthbankSetup earthbank,
                SettingsFactory settings, QWidget* parent = nullptr);
  ~PublishDialog() override;
  PublishDialog(const PublishDialog&) = delete;
  PublishDialog& operator=(const PublishDialog&) = delete;

  // "New release from selection" of these analyses (uuid text); what the
  // Details… of an earlier new release said is dropped.
  void open_new(const QStringList& analyses);
  // "Existing release", this one selected once the store's releases are read.
  void open_existing(persistence::Uuid release);

  // The plan as the dialog stands; nullopt while no existing release is
  // chosen. A new release's analyses, options and versions are filled.
  std::optional<publish::PublicationPlan> plan() const;

  // Runs the plan as the dialog stands; nothing when Run is off or a run is
  // under way. run_finished follows, once.
  void run();
  // Stops the run at the next step boundary (inside an EarthBank upload,
  // after the record that is out); the steps not run are Cancelled.
  void cancel();
  // A run is under way: from Run until its last row is settled. While it is,
  // Run, the release section and the destinations are off, and opening the
  // dialog again only raises it.
  bool busy() const noexcept { return running_ != nullptr; }

  // Details…: the selection and how many of it are under embargo now; the
  // spec the user filled in, or nullopt. Default: the New release dialog.
  using AskRelease = std::function<std::optional<publish::ReleaseSpec>(const QStringList& analyses, std::size_t embargoed)>;
  void set_ask_release(AskRelease ask) { ask_release_ = std::move(ask); }
  // Choose… of a directory field; nullopt cancels. Default: a directory chooser.
  using AskDirectory = std::function<std::optional<QString>()>;
  void set_ask_directory(AskDirectory ask) { ask_directory_ = std::move(ask); }
  // The time plans are checked at. Default: the system clock, as build_release.
  void set_now(std::function<persistence::UtcTime()> now) { now_ = std::move(now); }
  // Closing while a run is under way: true cancels it and closes once it has
  // ended, false keeps the dialog open. Default: a message box.
  void set_ask_close_while_running(std::function<bool()> ask) { ask_close_ = std::move(ask); }

  // For tests.
  QRadioButton* new_radio() const noexcept { return new_radio_; }
  QRadioButton* existing_radio() const noexcept { return existing_radio_; }
  QComboBox* release_combo() const noexcept { return release_combo_; }  // item data: release uuid
  QLabel* members() const noexcept { return members_; }                 // "3 analyses, 0 under embargo now"
  QPushButton* details_button() const noexcept { return details_; }
  QCheckBox* destination(publish::Destination d) const;
  QTabWidget* tabs() const noexcept { return tabs_; }
  QLineEdit* folder_dir() const noexcept { return folder_dir_; }
  QCheckBox* with_figures() const noexcept { return with_figures_; }
  QCheckBox* private_build() const noexcept { return private_build_; }
  QRadioButton* store_public() const noexcept { return store_public_; }
  QLineEdit* store_dir() const noexcept { return store_dir_; }  // hidden while Release folder is ticked
  EarthbankPublishPanel* earthbank_panel() const noexcept { return panel_; }  // null until its tab is first shown
  QLabel* earthbank_note() const noexcept { return earthbank_note_; }  // shown for a new release
  QLabel* refusals() const noexcept { return refusals_; }  // "<destination>: <reason>", one a line
  QPushButton* run_button() const noexcept { return run_; }
  QPushButton* cancel_button() const noexcept { return cancel_; }
  QTableWidget* results() const noexcept { return results_; }
  QProgressBar* progress() const noexcept { return progress_; }

 Q_SIGNALS:
  // A run is over: `steps`, the result of each ticked destination in run
  // order (empty when none was ticked); `release_ok`, false only when the run
  // was to make a new release and could not (every step is then Skipped).
  void run_finished(const std::vector<publish::StepResult>& steps, bool release_ok);

 protected:
  void showEvent(QShowEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
  void reject() override;

 private:
  class StageReporter;  // a build's stages, queued to the GUI thread

  // One run: the plan as it was when Run was pressed and what came of it.
  struct Run {
    enum class EarthBank { Idle, Loading, Waiting, Checking, Running };
    int id = 0;
    publish::PublicationPlan plan;
    std::vector<publish::Destination> order;
    std::vector<publish::StepResult> results;  // one per entry of order
    std::size_t next = 0;                      // the step running, or the next to run
    QString release;                           // uuid text, once the release exists
    bool created = false;                      // made by this run
    std::optional<std::filesystem::path> built;  // Folder's directory, when it ran Ok
    bool made_public = false;                  // Store ran Ok with make_public
    bool cancelled = false;
    EarthBank earthbank = EarthBank::Idle;
    bool panel_locked = false;     // the EarthBank tab is locked by this run
    bool panel_read_only = false;  // what it was before
  };

  bool current(int run) const noexcept { return running_ != nullptr && running_->id == run; }
  void set_running(bool on);
  // While a run is under way the EarthBank tab cannot change what runs:
  // read-only and locked; afterwards as it was.
  void lock_panel(bool on);
  void set_row(int row, const QString& name, publish::StepState state, const QString& message);
  void settle(publish::StepState state, const std::string& message);  // the step at next; next moves on
  void next_step();
  void run_folder();
  void run_store();
  void run_earthbank();
  void earthbank_poll();   // the EarthBank step waits for the panel's rows
  void start_panel();
  void panel_finished(bool ok, const QString& message);
  void build_stage(int run, const QString& stage);
  void finish();

  void load_releases();       // the combo, from the store
  void count_embargoed();     // of the selection, from the store
  void ask_details();
  void release_changed();     // the radio or the combo: tabs, dataset, check
  void update_release_section();
  void update_tabs();
  void update_store_dir();
  void ensure_panel();
  void load_dataset();        // the EarthBank tab's analyses, for an existing release
  void schedule_check();      // check_plan, 200 ms after the last change
  void check();
  void show_refusals();
  void update_run();
  void remember_destinations() const;
  void recall_destinations();
  std::unique_ptr<QSettings> settings() const;
  std::optional<QString> choose_directory();
  // Refusals the dialog judges itself (a new release without a name, a
  // release folder without a directory); check_plan's come on top.
  std::vector<publish::PlanRefusal> local_refusals() const;
  QString chosen_release() const;  // uuid text of the combo's current item; empty when none

  EntryBridge& bridge_;
  processing::StoreSource& source_;
  std::string url_;
  EarthbankSetup earthbank_;
  SettingsFactory settings_;
  AskRelease ask_release_;
  AskDirectory ask_directory_;
  std::function<persistence::UtcTime()> now_;
  std::function<bool()> ask_close_;
  // Read on the GUI thread by a build's queued stages; null once the dialog is gone.
  std::shared_ptr<PublishDialog*> alive_;
  std::unique_ptr<Run> running_;
  int runs_ = 0;
  bool close_when_done_ = false;
  bool dataset_loading_ = false;              // load_dataset's release_dataset is out

  QStringList selection_;
  std::optional<std::size_t> embargoed_;      // of the selection, once read
  std::optional<publish::ReleaseSpec> spec_;  // from Details…
  QStringList release_names_;                 // what a new release may supersede
  QString pending_select_;                    // selected by the next load_releases
  QString dataset_for_;                       // the release the EarthBank tab holds, or empty
  std::vector<publish::PlanRefusal> refusals_list_;
  QString check_error_;                       // the store could not be read
  bool checked_ = false;                      // refusals_list_ is of the plan as it stands
  int generation_ = 0;                        // bumped on every change: older results are dropped
  int selection_generation_ = 0;
  int dataset_generation_ = 0;
  bool restoring_ = false;

  QGroupBox* release_box_;
  QGroupBox* destination_box_;
  QRadioButton* new_radio_;
  QRadioButton* existing_radio_;
  QComboBox* release_combo_;
  QLabel* members_;
  QPushButton* details_;
  std::array<QCheckBox*, 5> destinations_{};
  QLabel* refusals_;
  QTabWidget* tabs_;
  QWidget* folder_page_;
  QLineEdit* folder_dir_;
  QCheckBox* with_figures_;
  QCheckBox* private_build_;
  QWidget* store_page_;
  QRadioButton* store_private_;
  QRadioButton* store_public_;
  QLabel* store_dir_label_;
  QLineEdit* store_dir_;
  QPushButton* store_browse_;
  QWidget* earthbank_page_;
  QLabel* earthbank_note_;
  EarthbankPublishPanel* panel_ = nullptr;
  QTableWidget* results_;
  QProgressBar* progress_;
  QPushButton* run_;
  QPushButton* cancel_;
  QTimer* check_timer_;
};

}  // namespace pychron::ui
