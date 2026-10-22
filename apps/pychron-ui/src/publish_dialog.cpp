#include "publish_dialog.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "earthbank_publish_panel.hpp"
#include "entry_bridge.hpp"
#include "release_dialog.hpp"
#include "theme.hpp"

// Qt's `signals` keyword macro would rewrite persistence::CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/processing/store_source.hpp"
#include "pychron/publish/earthbank_source.hpp"
#include "pychron/publish/workflow.hpp"
#include "pychron/reduction/arar_types.hpp"
#pragma pop_macro("signals")

namespace pychron::ui {

namespace ps = persistence;
namespace pb = publish;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

constexpr std::array<pb::Destination, 5> kDestinations = {pb::Destination::Folder, pb::Destination::Store,
                                                          pb::Destination::EarthBank, pb::Destination::Doi,
                                                          pb::Destination::Hosted};
constexpr int kCheckDelayMs = 200;

QString name_of(pb::Destination d) {
  const std::string_view name = pb::destination_name(d);
  return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}

std::size_t index_of(pb::Destination d) { return static_cast<std::size_t>(d); }

// The EarthBank tab's action combo is the plan's EarthBank mode.
pb::EarthBankMode mode_of(EarthbankPublishPanel::Action action) {
  switch (action) {
    case EarthbankPublishPanel::Action::Preflight: return pb::EarthBankMode::Preflight;
    case EarthbankPublishPanel::Action::ExportXlsx: return pb::EarthBankMode::ExportXlsx;
    case EarthbankPublishPanel::Action::Upload: return pb::EarthBankMode::Upload;
  }
  return pb::EarthBankMode::Preflight;
}

EarthbankPublishPanel::Action action_of(pb::EarthBankMode mode) {
  switch (mode) {
    case pb::EarthBankMode::Preflight: return EarthbankPublishPanel::Action::Preflight;
    case pb::EarthBankMode::ExportXlsx: return EarthbankPublishPanel::Action::ExportXlsx;
    case pb::EarthBankMode::Upload: return EarthbankPublishPanel::Action::Upload;
  }
  return EarthbankPublishPanel::Action::Preflight;
}

QString state_name(pb::StepState state) {
  switch (state) {
    case pb::StepState::Pending: return QObject::tr("Pending");
    case pb::StepState::Running: return QObject::tr("Running");
    case pb::StepState::Ok: return QObject::tr("Ok");
    case pb::StepState::Failed: return QObject::tr("Failed");
    case pb::StepState::Skipped: return QObject::tr("Skipped");
    case pb::StepState::Cancelled: return QObject::tr("Cancelled");
  }
  return {};
}

// The stages build_release reports, in order (build.hpp).
constexpr std::array<std::string_view, 9> kBuildStages = {"select", "load",    "reduce",  "tables",  "files",
                                                          "figures", "parquet", "records", "manifest"};

// What the panel says when the upload's confirmation was answered No.
const QString kNothingSent = QStringLiteral("Nothing was sent.");

QString counted_analyses(qsizetype n) {
  return n == 1 ? QObject::tr("1 analysis") : QObject::tr("%1 analyses").arg(n);
}

}  // namespace

// A build's stages, queued through the bridge (which lives on the GUI thread
// and outlives the dialog) and dropped there once the dialog is gone. The
// worker only copies the shared token; the dialog is read on the GUI thread.
class PublishDialog::StageReporter final : public pb::IProgress {
 public:
  StageReporter(QObject* bridge, std::shared_ptr<PublishDialog*> alive, int run)
      : bridge_(bridge), alive_(std::move(alive)), run_(run) {}
  void stage(std::string_view name) override {
    QMetaObject::invokeMethod(
        bridge_,
        [alive = alive_, run = run_, n = QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()))] {
          if (*alive != nullptr) (*alive)->build_stage(run, n);
        },
        Qt::QueuedConnection);
  }

 private:
  QObject* bridge_;
  std::shared_ptr<PublishDialog*> alive_;
  int run_;
};

PublishDialog::PublishDialog(EntryBridge& bridge, processing::StoreSource& source, std::string store_url,
                             EarthbankSetup earthbank, SettingsFactory settings, QWidget* parent)
    : QDialog(parent),
      bridge_(bridge),
      source_(source),
      url_(std::move(store_url)),
      earthbank_(std::move(earthbank)),
      settings_(std::move(settings)),
      now_([] { return ps::UtcTime::now(); }),
      alive_(std::make_shared<PublishDialog*>(this)),
      release_box_(new QGroupBox(tr("Release"))),
      destination_box_(new QGroupBox(tr("Destinations"))),
      new_radio_(new QRadioButton(tr("New release from selection"))),
      existing_radio_(new QRadioButton(tr("Existing release"))),
      release_combo_(new QComboBox),
      members_(new QLabel),
      details_(new QPushButton(tr("Details…"))),
      refusals_(new QLabel),
      tabs_(new QTabWidget),
      folder_page_(new QWidget(this)),
      folder_dir_(new QLineEdit(QDir::homePath())),
      with_figures_(new QCheckBox(tr("With figures (ideogram, spectra, isochron as SVG and PDF)"))),
      private_build_(new QCheckBox(tr("Private build (keeps embargoed rows; never hosted as public)"))),
      store_page_(new QWidget(this)),
      store_private_(new QRadioButton(tr("Private"))),
      store_public_(new QRadioButton(tr("Public"))),
      store_dir_label_(new QLabel(tr("Built directory:"))),
      store_dir_(new QLineEdit),
      store_browse_(new QPushButton(tr("Choose…"))),
      earthbank_page_(new QWidget(this)),
      earthbank_note_(new QLabel),
      results_(new QTableWidget(0, 3)),
      progress_(new QProgressBar),
      run_(new QPushButton(tr("Run"))),
      cancel_(new QPushButton(tr("Close"))),
      check_timer_(new QTimer(this)) {
  setWindowTitle(tr("Publish"));
  setObjectName(QStringLiteral("publish_dialog"));
  setModal(false);

  ask_release_ = [this](const QStringList& analyses, std::size_t embargoed) {
    return ask_release(this, static_cast<std::size_t>(analyses.size()), embargoed, release_names_);
  };
  ask_directory_ = [this]() -> std::optional<QString> {
    QString chosen = QFileDialog::getExistingDirectory(this, tr("Choose a directory"), QDir::homePath());
    if (chosen.isEmpty()) return std::nullopt;
    return chosen;
  };
  ask_close_ = [this] {
    return QMessageBox::question(this, tr("Publish"),
                                 tr("A run is under way. Cancel it, and close once the step that is out is done?")) ==
           QMessageBox::Yes;
  };

  // Release section.
  auto* release_layout = new QVBoxLayout(release_box_);
  auto* new_row = new QHBoxLayout;
  new_row->addWidget(new_radio_);
  new_row->addWidget(members_, 1);
  new_row->addWidget(details_);
  auto* existing_row = new QHBoxLayout;
  existing_row->addWidget(existing_radio_);
  existing_row->addWidget(release_combo_, 1);
  release_layout->addLayout(new_row);
  release_layout->addLayout(existing_row);
  new_radio_->setChecked(true);

  // Destinations.
  auto* destination_layout = new QHBoxLayout(destination_box_);
  for (pb::Destination d : kDestinations) {
    auto* box = new QCheckBox(name_of(d));
    destinations_[index_of(d)] = box;
    destination_layout->addWidget(box);
    if (d == pb::Destination::Doi || d == pb::Destination::Hosted) {
      box->setEnabled(false);
      box->setToolTip(tr("Not built yet"));
    }
  }
  destination_layout->addStretch();
  refusals_->setWordWrap(true);
  style::set_tone(refusals_, style::Tone::Warning);

  // Release folder tab: the fields of the Publish window's Build dialog.
  auto* folder_form = new QFormLayout(folder_page_);
  auto* folder_browse = new QPushButton(tr("Choose…"));
  auto* folder_row = new QHBoxLayout;
  folder_row->addWidget(folder_dir_, 1);
  folder_row->addWidget(folder_browse);
  folder_form->addRow(tr("Directory"), folder_row);
  with_figures_->setChecked(true);
  folder_form->addRow(with_figures_);
  folder_form->addRow(private_build_);

  // Publish in store tab.
  auto* store_layout = new QVBoxLayout(store_page_);
  store_private_->setChecked(true);
  store_layout->addWidget(store_private_);
  store_layout->addWidget(store_public_);
  auto* store_dir_row = new QHBoxLayout;
  store_dir_row->addWidget(store_dir_label_);
  store_dir_row->addWidget(store_dir_, 1);
  store_dir_row->addWidget(store_browse_);
  store_dir_->setPlaceholderText(tr("The directory this release was built into"));
  store_layout->addLayout(store_dir_row);
  store_layout->addStretch();

  // EarthBank tab: the panel is made when the tab is first shown.
  auto* earthbank_layout = new QVBoxLayout(earthbank_page_);
  earthbank_note_->setWordWrap(true);
  earthbank_note_->setText(
      tr("The release is made when the plan runs; each sample's choices are then taken from the values remembered for "
         "it, or the rule's. To review them first, make the release and open it here as an existing release."));
  earthbank_layout->addWidget(earthbank_note_);
  earthbank_layout->setContentsMargins(0, 0, 0, 0);
  folder_page_->hide();
  store_page_->hide();
  earthbank_page_->hide();

  // Results of the last run.
  results_->setHorizontalHeaderLabels({tr("Destination"), tr("State"), tr("Message")});
  results_->horizontalHeader()->setStretchLastSection(true);
  results_->verticalHeader()->hide();
  results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  results_->hide();
  progress_->setRange(0, 1);
  progress_->setValue(0);
  progress_->hide();

  auto* button_row = new QHBoxLayout;
  button_row->addStretch();
  button_row->addWidget(cancel_);
  button_row->addWidget(run_);
  run_->setDefault(true);
  run_->setEnabled(false);

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(release_box_);
  layout->addWidget(destination_box_);
  layout->addWidget(refusals_);
  layout->addWidget(tabs_, 1);
  layout->addWidget(results_);
  layout->addWidget(progress_);
  layout->addLayout(button_row);

  check_timer_->setSingleShot(true);
  check_timer_->setInterval(kCheckDelayMs);
  connect(check_timer_, &QTimer::timeout, this, [this] { check(); });

  connect(new_radio_, &QRadioButton::toggled, this, [this] { release_changed(); });
  connect(release_combo_, &QComboBox::currentIndexChanged, this, [this] { release_changed(); });
  connect(details_, &QPushButton::clicked, this, [this] { ask_details(); });
  for (pb::Destination d : kDestinations) {
    connect(destinations_[index_of(d)], &QCheckBox::toggled, this, [this] {
      if (!restoring_) remember_destinations();
      update_tabs();
      update_store_dir();
      schedule_check();
    });
  }
  connect(folder_dir_, &QLineEdit::textChanged, this, [this] { schedule_check(); });
  connect(store_dir_, &QLineEdit::textChanged, this, [this] { schedule_check(); });
  connect(store_public_, &QRadioButton::toggled, this, [this] { schedule_check(); });
  connect(folder_browse, &QPushButton::clicked, this, [this] {
    if (auto chosen = choose_directory()) folder_dir_->setText(*chosen);
  });
  connect(store_browse_, &QPushButton::clicked, this, [this] {
    if (auto chosen = choose_directory()) store_dir_->setText(*chosen);
  });
  connect(run_, &QPushButton::clicked, this, [this] { run(); });
  connect(cancel_, &QPushButton::clicked, this, [this] {
    if (busy()) {
      cancel();
    } else {
      close();
    }
  });

  recall_destinations();
  update_release_section();
  update_tabs();
  update_store_dir();
  update_run();
}

PublishDialog::~PublishDialog() {
  *alive_ = nullptr;  // a build's stages still queued reach nothing
  // The panel goes with the dialog's children, after this dialog's members:
  // nothing it says on the way may reach them.
  if (panel_ != nullptr) panel_->disconnect(this);
}

QCheckBox* PublishDialog::destination(pb::Destination d) const { return destinations_[index_of(d)]; }

void PublishDialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  update_tabs();  // the EarthBank tab, when it was ticked before the dialog was shown
}

void PublishDialog::open_new(const QStringList& analyses) {
  selection_ = analyses;
  spec_.reset();
  embargoed_.reset();
  {
    const QSignalBlocker block(new_radio_);
    new_radio_->setChecked(true);
  }
  count_embargoed();
  load_releases();
  release_changed();
}

void PublishDialog::open_existing(ps::Uuid release) {
  pending_select_ = qs(release.str());
  {
    const QSignalBlocker block(new_radio_);
    existing_radio_->setChecked(true);
  }
  if (const int at = release_combo_->findData(pending_select_); at >= 0) {
    const QSignalBlocker block(release_combo_);
    release_combo_->setCurrentIndex(at);
  }
  load_releases();
  release_changed();
}

void PublishDialog::load_releases() {
  bridge_.run<std::vector<ps::ReleaseRow>>(
      this,
      [](ps::IStore& store, const ps::Actor&) -> Result<std::vector<ps::ReleaseRow>> {
        auto rows = store.releases({});
        if (!rows) return fail(rows.error());
        std::vector<ps::ReleaseRow> out;
        for (auto& row : *rows)
          if (row.state != ps::ReleaseState::Withdrawn) out.push_back(std::move(row));
        return out;
      },
      [this](Result<std::vector<ps::ReleaseRow>> rows) {
        if (!rows) {
          check_error_ = tr("The releases could not be read: %1").arg(qs(rows.error().what));
          show_refusals();
          return;
        }
        const QString keep = pending_select_.isEmpty() ? chosen_release() : pending_select_;
        {
          const QSignalBlocker block(release_combo_);
          release_combo_->clear();
          release_names_.clear();
          for (const ps::ReleaseRow& r : *rows) {
            release_combo_->addItem(QStringLiteral("%1@v%2, %3, %4")
                                        .arg(qs(r.name))
                                        .arg(r.version)
                                        .arg(qs(std::string(ps::to_string(r.state))),
                                             qs(std::string(ps::to_string(r.visibility)))),
                                    qs(r.uuid.str()));
            if (!release_names_.contains(qs(r.name))) release_names_ << qs(r.name);
          }
          if (const int at = release_combo_->findData(keep); at >= 0) release_combo_->setCurrentIndex(at);
        }
        if (release_combo_->findData(pending_select_) >= 0) pending_select_.clear();
        release_changed();
      });
}

void PublishDialog::count_embargoed() {
  const int generation = ++selection_generation_;
  std::vector<ps::Uuid> uuids;
  for (const QString& a : selection_)
    if (auto u = ps::Uuid::parse(a.toStdString())) uuids.push_back(*u);
  if (uuids.empty()) {
    embargoed_ = 0;
    update_release_section();
    return;
  }
  bridge_.run<std::size_t>(
      this,
      [uuids, now = now_()](ps::IStore& store, const ps::Actor&) -> Result<std::size_t> {
        auto embargoed = store.embargoed_analyses(uuids, now);
        if (!embargoed) return fail(embargoed.error());
        return embargoed->size();
      },
      [this, generation](Result<std::size_t> embargoed) {
        if (generation != selection_generation_) return;
        if (!embargoed) {
          members_->setText(tr("%1; the embargoes could not be read: %2")
                                .arg(counted_analyses(selection_.size()), qs(embargoed.error().what)));
          return;
        }
        embargoed_ = *embargoed;
        update_release_section();
      });
}

void PublishDialog::ask_details() {
  if (!ask_release_ || selection_.isEmpty() || !embargoed_) return;
  auto spec = ask_release_(selection_, *embargoed_);
  if (!spec) return;
  spec_ = std::move(*spec);
  schedule_check();
}

void PublishDialog::release_changed() {
  update_release_section();
  update_tabs();
  load_dataset();
  schedule_check();
}

void PublishDialog::update_release_section() {
  const bool fresh = new_radio_->isChecked();
  release_combo_->setEnabled(!fresh);
  details_->setEnabled(fresh && !selection_.isEmpty() && embargoed_.has_value());
  QString text = counted_analyses(selection_.size());
  if (embargoed_) text += tr(", %1 under embargo now").arg(*embargoed_);
  members_->setText(text);
}

void PublishDialog::update_tabs() {
  QWidget* current = tabs_->currentWidget();
  const std::array<std::pair<pb::Destination, QWidget*>, 3> pages = {
      {{pb::Destination::Folder, folder_page_}, {pb::Destination::Store, store_page_},
       {pb::Destination::EarthBank, earthbank_page_}}};
  int at = 0;
  for (const auto& [d, page] : pages) {
    const int found = tabs_->indexOf(page);
    if (destination(d)->isChecked()) {
      if (found < 0) tabs_->insertTab(at, page, name_of(d));
      ++at;
    } else if (found >= 0) {
      tabs_->removeTab(found);
      page->hide();
    }
  }
  if (current != nullptr && tabs_->indexOf(current) >= 0) tabs_->setCurrentWidget(current);

  const bool fresh = new_radio_->isChecked();
  earthbank_note_->setVisible(fresh);
  if (destination(pb::Destination::EarthBank)->isChecked() && isVisible()) ensure_panel();
  if (panel_ != nullptr) {
    if (!busy()) panel_->set_read_only(fresh);  // a run holds it read-only (lock_panel)
    if (fresh && !dataset_for_.isEmpty() && !busy()) {
      // The panel is filled with the new release when the run makes it.
      dataset_for_.clear();
      dataset_loading_ = false;
      ++dataset_generation_;
      panel_->set_analyses({});
    }
  }
}

void PublishDialog::update_store_dir() {
  const bool shown = !destination(pb::Destination::Folder)->isChecked();
  store_dir_label_->setVisible(shown);
  store_dir_->setVisible(shown);
  store_browse_->setVisible(shown);
}

void PublishDialog::ensure_panel() {
  if (panel_ != nullptr) return;
  panel_ = new EarthbankPublishPanel({}, earthbank_, settings_, earthbank_page_);
  earthbank_page_->layout()->addWidget(panel_);
  panel_->set_read_only(new_radio_->isChecked());
  connect(panel_, &EarthbankPublishPanel::controls_changed, this, [this] {
    update_run();
    earthbank_poll();
  });
  connect(panel_, &EarthbankPublishPanel::finished, this,
          [this](bool ok, const QString& message) { panel_finished(ok, message); });
  connect(panel_, &EarthbankPublishPanel::progress, this, [this](int done, int total) {
    if (running_ == nullptr || running_->earthbank != Run::EarthBank::Running) return;
    progress_->setRange(0, std::max(total, 1));
    progress_->setValue(std::min(done, std::max(total, 1)));
  });
  connect(panel_->action(), &QComboBox::currentIndexChanged, this, [this] { schedule_check(); });
  lock_panel(true);  // made by a run's EarthBank step
  load_dataset();
  schedule_check();  // the plan's mode is the panel's now
}

void PublishDialog::load_dataset() {
  if (panel_ == nullptr || new_radio_->isChecked() || !destination(pb::Destination::EarthBank)->isChecked()) return;
  const QString chosen = chosen_release();
  if (chosen.isEmpty() || chosen == dataset_for_) return;
  const auto release = ps::Uuid::parse(chosen.toStdString());
  if (!release) return;
  dataset_for_ = chosen;
  dataset_loading_ = true;
  const int generation = ++dataset_generation_;
  bridge_.run<std::vector<processing::AnalysisPtr>>(
      this,
      [&source = source_, release = *release](ps::IStore& store, const ps::Actor&) {
        return pb::release_dataset(store, source, release);
      },
      [this, generation](Result<std::vector<processing::AnalysisPtr>> analyses) {
        if (generation != dataset_generation_ || panel_ == nullptr) return;
        dataset_loading_ = false;
        if (!analyses) {
          dataset_for_.clear();
          earthbank_note_->setText(tr("The release's analyses could not be read: %1").arg(qs(analyses.error().what)));
          earthbank_note_->show();
          if (running_ != nullptr && running_->earthbank == Run::EarthBank::Waiting) {
            running_->earthbank = Run::EarthBank::Idle;
            settle(pb::StepState::Failed, "The release's analyses could not be read: " + analyses.error().what);
            next_step();
          }
          return;
        }
        panel_->set_analyses(std::move(*analyses));
        earthbank_poll();
      });
}

void PublishDialog::schedule_check() {
  if (busy()) return;  // the run is of the plan as checked; finish() checks again
  ++generation_;
  checked_ = false;
  update_run();
  check_timer_->start();
}

void PublishDialog::check() {
  if (busy()) return;
  const int generation = generation_;
  auto p = plan();
  if (!p) {
    refusals_list_.clear();
    check_error_.clear();
    checked_ = true;
    show_refusals();
    update_run();
    return;
  }
  bridge_.run<std::vector<pb::PlanRefusal>>(
      this,
      [plan = std::move(*p), now = now_()](ps::IStore& store, const ps::Actor&) {
        return pb::check_plan(store, plan, now);
      },
      [this, generation](Result<std::vector<pb::PlanRefusal>> refused) {
        if (generation != generation_) return;  // overtaken by a later change
        checked_ = true;
        if (!refused) {
          refusals_list_.clear();
          check_error_ = tr("The plan could not be checked: %1").arg(qs(refused.error().what));
        } else {
          check_error_.clear();
          refusals_list_ = std::move(*refused);
        }
        show_refusals();
        update_run();
      });
}

std::vector<pb::PlanRefusal> PublishDialog::local_refusals() const {
  std::vector<pb::PlanRefusal> out;
  pb::Destination first = pb::Destination::Folder;
  for (pb::Destination d : kDestinations) {
    if (destination(d)->isChecked()) {
      first = d;
      break;
    }
  }
  if (new_radio_->isChecked()) {
    if (!selection_.isEmpty() && (!spec_ || spec_->name.empty()))
      out.push_back({first, "The new release needs a name: fill in its Details…"});
  } else if (chosen_release().isEmpty()) {
    out.push_back({first, "Choose a release."});
  }
  if (destination(pb::Destination::Folder)->isChecked() && folder_dir_->text().trimmed().isEmpty())
    out.push_back({pb::Destination::Folder, "Name the directory the release is built in."});
  return out;
}

void PublishDialog::show_refusals() {
  QStringList lines;
  auto refusals = local_refusals();
  refusals.insert(refusals.end(), refusals_list_.begin(), refusals_list_.end());
  for (const pb::PlanRefusal& r : refusals) lines << tr("%1: %2").arg(name_of(r.destination), qs(r.reason));
  if (!check_error_.isEmpty()) lines << check_error_;
  refusals_->setText(lines.join(QLatin1Char('\n')));
  refusals_->setVisible(!lines.isEmpty());
}

void PublishDialog::update_run() {
  bool any = false;
  for (pb::Destination d : kDestinations) any = any || destination(d)->isChecked();
  // A new release with nothing ticked is a run that only makes the release
  // (spec 6.2); an existing one with nothing ticked would do nothing.
  const bool something = any || new_radio_->isChecked();
  const bool earthbank_ready = !destination(pb::Destination::EarthBank)->isChecked() || new_radio_->isChecked() ||
                               (panel_ != nullptr && panel_->can_run());
  run_->setEnabled(!busy() && something && checked_ && check_error_.isEmpty() && refusals_list_.empty() &&
                   local_refusals().empty() && plan().has_value() && earthbank_ready);
}

// ---- The run ----

void PublishDialog::run() {
  if (busy() || !run_->isEnabled()) return;
  auto p = plan();
  if (!p) return;
  auto started = std::make_unique<Run>();
  started->id = ++runs_;
  started->plan = std::move(*p);
  started->order = pb::run_order(started->plan);
  for (pb::Destination d : started->order) started->results.push_back({d, pb::StepState::Pending, {}});
  running_ = std::move(started);
  close_when_done_ = false;
  check_timer_->stop();

  results_->setRowCount(0);
  const auto* fresh = std::get_if<pb::NewRelease>(&running_->plan.release);
  if (running_->order.empty()) {
    results_->setRowCount(1);
    set_row(0, tr("Release"), pb::StepState::Pending, {});
  } else {
    results_->setRowCount(static_cast<int>(running_->order.size()));
    for (std::size_t i = 0; i < running_->order.size(); ++i)
      set_row(static_cast<int>(i), name_of(running_->order[i]), pb::StepState::Pending, {});
  }
  set_running(true);

  if (fresh == nullptr) {
    running_->release = qs(std::get<pb::ExistingRelease>(running_->plan.release).name_or_uuid);
    next_step();
    return;
  }
  // The new release first; when it cannot be made nothing else runs.
  if (running_->order.empty()) set_row(0, tr("Release"), pb::StepState::Running, {});
  progress_->setRange(0, 0);
  progress_->setFormat(tr("Making the release…"));
  const int id = running_->id;
  bridge_.run<pb::ReleaseCreated>(
      this,
      [spec = fresh->spec](ps::IStore& store, const ps::Actor& actor) { return pb::create_release(store, actor, spec); },
      [this, id](Result<pb::ReleaseCreated> created) {
        if (!current(id)) return;
        Run& r = *running_;
        if (!created) {
          const std::string why = created.error().what;
          if (r.order.empty()) {
            set_row(0, tr("Release"), pb::StepState::Failed, qs(why));
          } else {
            while (r.next < r.order.size()) settle(pb::StepState::Skipped, "skipped: release could not be made: " + why);
          }
          finish();
          return;
        }
        r.release = qs(created->release.str());
        r.created = true;
        if (r.order.empty()) {
          const auto& spec = std::get<pb::NewRelease>(r.plan.release).spec;
          set_row(0, tr("Release"), pb::StepState::Ok,
                  tr("made %1 v%2: %3 members").arg(qs(spec.name)).arg(created->version).arg(created->members));
        }
        next_step();
      });
}

void PublishDialog::cancel() {
  if (running_ == nullptr || running_->cancelled) return;
  running_->cancelled = true;
  cancel_->setEnabled(false);
  progress_->setFormat(tr("Cancelling after the step that is out…"));
  if (running_->earthbank == Run::EarthBank::Running && panel_ != nullptr) {
    panel_->cancel();
  } else if (running_->earthbank == Run::EarthBank::Waiting) {
    earthbank_poll();
  }
}

void PublishDialog::set_running(bool on) {
  release_box_->setEnabled(!on);
  destination_box_->setEnabled(!on);
  folder_page_->setEnabled(!on);
  store_page_->setEnabled(!on);
  lock_panel(on);
  cancel_->setText(on ? tr("Cancel") : tr("Close"));
  cancel_->setEnabled(true);
  results_->show();
  progress_->setVisible(on);
  update_run();
}

void PublishDialog::lock_panel(bool on) {
  if (panel_ == nullptr || running_ == nullptr || running_->panel_locked == on) return;
  if (on) {
    running_->panel_read_only = panel_->read_only();
    panel_->set_read_only(true);  // for a new release, the remembered choices (spec 6.2)
    panel_->set_locked(true);
  } else {
    panel_->set_locked(false);
    panel_->set_read_only(running_->panel_read_only);
  }
  running_->panel_locked = on;
}

void PublishDialog::set_row(int row, const QString& name, pb::StepState state, const QString& message) {
  const std::array<QString, 3> cells = {name, state_name(state), message};
  for (int column = 0; column < 3; ++column) {
    auto* item = results_->item(row, column);
    if (item == nullptr) {
      item = new QTableWidgetItem;
      results_->setItem(row, column, item);
    }
    item->setText(cells[static_cast<std::size_t>(column)]);
  }
  results_->item(row, 2)->setToolTip(message);
}

void PublishDialog::settle(pb::StepState state, const std::string& message) {
  Run& r = *running_;
  if (r.next >= r.order.size()) return;
  pb::StepResult& result = r.results[r.next];
  result.state = state;
  result.message = message;
  set_row(static_cast<int>(r.next), name_of(result.destination), state, qs(message));
  ++r.next;
}

void PublishDialog::next_step() {
  if (running_ == nullptr) return;
  Run& r = *running_;
  while (r.next < r.order.size()) {
    const pb::Destination d = r.order[r.next];
    if (r.cancelled) {
      settle(pb::StepState::Cancelled, "cancelled");
      continue;
    }
    const std::vector<pb::StepResult> so_far(r.results.begin(),
                                             r.results.begin() + static_cast<std::ptrdiff_t>(r.next));
    if (pb::depends_on_failed(r.plan, d, so_far)) {
      // Named after the step that failed: the first Failed, else the first not Ok.
      auto failed = std::ranges::find(so_far, pb::StepState::Failed, &pb::StepResult::state);
      if (failed == so_far.end())
        failed = std::ranges::find_if(so_far, [](const pb::StepResult& x) { return x.state != pb::StepState::Ok; });
      const std::string_view name =
          failed == so_far.end() ? std::string_view("a step before it") : pb::destination_name(failed->destination);
      settle(pb::StepState::Skipped, "skipped: " + std::string(name) + " failed");
      continue;
    }
    r.results[r.next].state = pb::StepState::Running;
    set_row(static_cast<int>(r.next), name_of(d), pb::StepState::Running, {});
    progress_->setFormat(tr("%1 (%v of %m)").arg(name_of(d)));
    switch (d) {
      case pb::Destination::Folder: run_folder(); return;
      case pb::Destination::Store: run_store(); return;
      case pb::Destination::EarthBank: run_earthbank(); return;
      case pb::Destination::Doi:
      case pb::Destination::Hosted: settle(pb::StepState::Failed, "Not built yet"); break;
    }
  }
  finish();
}

void PublishDialog::run_folder() {
  Run& r = *running_;
  const int id = r.id;
  progress_->setRange(0, static_cast<int>(kBuildStages.size()));
  progress_->setValue(0);
  auto reporter = std::make_shared<StageReporter>(&bridge_, alive_, id);
  bridge_.run<pb::BuildResult>(
      this,
      [&source = source_, options = r.plan.folder->build, release = r.release.toStdString(),
       reporter](ps::IStore& store, const ps::Actor&) -> Result<pb::BuildResult> {
        auto uuid = ps::Uuid::parse(release);
        if (!uuid) return fail(ErrorKind::Config, "no release " + release);
        return pb::build_release(store, source, *uuid, options, reporter.get());
      },
      [this, id](Result<pb::BuildResult> built) {
        if (!current(id)) return;
        if (!built) {
          settle(pb::StepState::Failed, built.error().what);
        } else {
          running_->built = built->dir;
          progress_->setValue(progress_->maximum());
          std::string message = "built " + built->dir.string();
          if (!built->parquet_written) message += " (no Parquet tables: this program was built without Apache Arrow)";
          settle(pb::StepState::Ok, message);
        }
        next_step();
      });
}

void PublishDialog::build_stage(int run, const QString& stage) {
  if (!current(run) || running_->next >= running_->order.size() ||
      running_->order[running_->next] != pb::Destination::Folder)
    return;
  const std::string name = stage.toStdString();
  const auto at = std::ranges::find(kBuildStages, std::string_view(name));
  if (at != kBuildStages.end()) progress_->setValue(static_cast<int>(at - kBuildStages.begin()) + 1);
}

void PublishDialog::run_store() {
  Run& r = *running_;
  const int id = r.id;
  // The directory this run built, else the one named on the Store tab.
  std::optional<std::filesystem::path> dir = r.built ? r.built : r.plan.store->dir;
  if (!dir) {
    settle(pb::StepState::Failed, "No built directory of this release: tick Release folder or name one.");
    next_step();
    return;
  }
  progress_->setRange(0, 0);
  const bool make_public = r.plan.store->make_public;
  bridge_.run<ps::ReleaseRow>(
      this,
      [release = r.release.toStdString(), dir = *dir, make_public](ps::IStore& store, const ps::Actor& actor) {
        return pb::publish_built(store, actor, release, dir, make_public);
      },
      [this, id, make_public](Result<ps::ReleaseRow> row) {
        if (!current(id)) return;
        progress_->setRange(0, 1);
        progress_->setValue(1);
        if (!row) {
          settle(pb::StepState::Failed, row.error().what);
        } else {
          running_->made_public = make_public && row->visibility == ps::Visibility::Public;
          settle(pb::StepState::Ok,
                 std::string(ps::to_string(row->state)) + ", " + std::string(ps::to_string(row->visibility)));
        }
        next_step();
      });
}

void PublishDialog::run_earthbank() {
  Run& r = *running_;
  ensure_panel();
  progress_->setRange(0, 0);
  // The panel holds this release's analyses, or they are on their way.
  if (!r.created && dataset_for_ == r.release && dataset_loading_) {
    r.earthbank = Run::EarthBank::Waiting;
    return;  // load_dataset's result polls
  }
  if (!r.created && dataset_for_ == r.release && panel_->table()->rowCount() > 0) {
    r.earthbank = Run::EarthBank::Waiting;
    earthbank_poll();
    return;
  }
  const auto release = ps::Uuid::parse(r.release.toStdString());
  if (!release) {
    settle(pb::StepState::Failed, "no release " + r.release.toStdString());
    next_step();
    return;
  }
  r.earthbank = Run::EarthBank::Loading;
  dataset_for_ = r.release;
  dataset_loading_ = true;
  const int generation = ++dataset_generation_;
  const int id = r.id;
  bridge_.run<std::vector<processing::AnalysisPtr>>(
      this,
      [&source = source_, release = *release](ps::IStore& store, const ps::Actor&) {
        return pb::release_dataset(store, source, release);
      },
      [this, id, generation](Result<std::vector<processing::AnalysisPtr>> analyses) {
        if (generation == dataset_generation_) dataset_loading_ = false;
        if (!current(id) || running_->earthbank != Run::EarthBank::Loading) return;
        if (!analyses) {
          dataset_for_.clear();
          running_->earthbank = Run::EarthBank::Idle;
          settle(pb::StepState::Failed, "The release's analyses could not be read: " + analyses.error().what);
          next_step();
          return;
        }
        running_->earthbank = Run::EarthBank::Waiting;
        panel_->set_analyses(std::move(*analyses));
        earthbank_poll();
      });
}

void PublishDialog::earthbank_poll() {
  if (running_ == nullptr || running_->earthbank != Run::EarthBank::Waiting || panel_ == nullptr) return;
  Run& r = *running_;
  if (r.cancelled) {
    r.earthbank = Run::EarthBank::Idle;
    settle(pb::StepState::Cancelled, "cancelled");
    next_step();
    return;
  }
  if (!panel_->ready()) return;  // the rows are not in yet: controls_changed polls again
  if (r.plan.earthbank->mode != pb::EarthBankMode::Upload) {
    start_panel();
    return;
  }
  // D3 again, at the time of the upload: an embargo may have come since the check.
  const auto release = ps::Uuid::parse(r.release.toStdString());
  if (!release) {
    r.earthbank = Run::EarthBank::Idle;
    settle(pb::StepState::Failed, "no release " + r.release.toStdString());
    next_step();
    return;
  }
  r.earthbank = Run::EarthBank::Checking;
  const int id = r.id;
  bridge_.run<std::optional<std::string>>(
      this,
      [release = *release, made_public = r.made_public, now = now_()](ps::IStore& store, const ps::Actor&) {
        return pb::earthbank_upload_refusal(store, release, made_public, now);
      },
      [this, id](Result<std::optional<std::string>> refused) {
        if (!current(id) || running_->earthbank != Run::EarthBank::Checking) return;
        if (!refused || *refused || running_->cancelled) {
          running_->earthbank = Run::EarthBank::Idle;
          if (!refused) {
            settle(pb::StepState::Failed, "The upload could not be checked: " + refused.error().what);
          } else if (*refused) {
            settle(pb::StepState::Failed, **refused);
          } else {
            settle(pb::StepState::Cancelled, "cancelled");
          }
          next_step();
          return;
        }
        start_panel();
      });
}

void PublishDialog::start_panel() {
  Run& r = *running_;
  r.earthbank = Run::EarthBank::Running;
  progress_->setRange(0, 0);
  panel_->start(action_of(r.plan.earthbank->mode));  // may say finished at once (a hint): Failed
}

void PublishDialog::panel_finished(bool ok, const QString& message) {
  if (running_ == nullptr || running_->earthbank != Run::EarthBank::Running) return;
  running_->earthbank = Run::EarthBank::Idle;
  pb::StepState state = pb::StepState::Failed;
  if (ok) {
    state = pb::StepState::Ok;
  } else if (message == kNothingSent || running_->cancelled) {
    state = pb::StepState::Cancelled;  // the confirmation said No, or Cancel stopped it
  }
  settle(state, message.toStdString());
  next_step();
}

void PublishDialog::finish() {
  lock_panel(false);
  std::unique_ptr<Run> done = std::move(running_);
  const QPointer<PublishDialog> alive(this);
  set_running(false);
  if (done->created) {
    // The release section now holds the release this run made: a second run publishes it, not another.
    if (const auto made = ps::Uuid::parse(done->release.toStdString())) open_existing(*made);
  } else {
    schedule_check();  // what the run changed (a published release) is judged again
  }
  const bool release_ok = std::holds_alternative<pb::ExistingRelease>(done->plan.release) || done->created;
  Q_EMIT run_finished(done->results, release_ok);
  if (!alive) return;
  if (close_when_done_) {
    close_when_done_ = false;
    close();
  }
}

void PublishDialog::closeEvent(QCloseEvent* event) {
  if (!busy()) {
    QDialog::closeEvent(event);
    return;
  }
  event->ignore();
  if (close_when_done_) return;  // already cancelling, and closing after
  const QPointer<PublishDialog> alive(this);
  const bool yes = ask_close_ && ask_close_();
  if (!alive || !yes) return;
  if (!busy()) {  // the run ended while the question was open
    close();
    return;
  }
  close_when_done_ = true;
  cancel();
}

void PublishDialog::reject() {
  if (busy()) {
    close();  // asks, as closing the window does
    return;
  }
  QDialog::reject();
}

std::optional<pb::PublicationPlan> PublishDialog::plan() const {
  pb::PublicationPlan out;
  if (new_radio_->isChecked()) {
    pb::ReleaseSpec spec = spec_.value_or(pb::ReleaseSpec{});
    spec.analyses.clear();
    for (const QString& a : selection_) spec.analyses.push_back(a.toStdString());
    if (spec.options_json.empty()) spec.options_json = pb::release_options_json(pb::ReleaseOptions{});
    spec.reduction_version = std::string(reduction::kReductionVersion);
#ifdef PYCHRON_VERSION
    spec.software_version = std::string("pychron-cpp/") + PYCHRON_VERSION;
#else
    spec.software_version = "pychron-cpp";
#endif
    out.release = pb::NewRelease{std::move(spec)};
  } else {
    const QString chosen = chosen_release();
    if (chosen.isEmpty()) return std::nullopt;
    out.release = pb::ExistingRelease{chosen.toStdString()};
  }
  const bool folder = destination(pb::Destination::Folder)->isChecked();
  if (folder) {
    pb::BuildOptions build;
    build.out = folder_dir_->text().trimmed().toStdString();
    build.with_figures = with_figures_->isChecked();
    build.private_build = private_build_->isChecked();
    build.store_url = url_;
    out.folder = pb::FolderStep{std::move(build)};
  }
  if (destination(pb::Destination::Store)->isChecked()) {
    pb::StoreStep store;
    store.make_public = store_public_->isChecked();
    if (!folder && !store_dir_->text().trimmed().isEmpty()) store.dir = store_dir_->text().trimmed().toStdString();
    out.store = store;
  }
  if (destination(pb::Destination::EarthBank)->isChecked()) {
    pb::EarthBankStep step;
    if (panel_ != nullptr) step.mode = mode_of(panel_->current_action());
    out.earthbank = step;
  }
  out.doi = destination(pb::Destination::Doi)->isChecked();
  out.hosted = destination(pb::Destination::Hosted)->isChecked();
  return out;
}

QString PublishDialog::chosen_release() const { return release_combo_->currentData().toString(); }

std::optional<QString> PublishDialog::choose_directory() {
  if (!ask_directory_) return std::nullopt;
  return ask_directory_();
}

std::unique_ptr<QSettings> PublishDialog::settings() const {
  return settings_ ? settings_() : std::make_unique<QSettings>();
}

void PublishDialog::remember_destinations() const {
  QStringList ticked;
  for (pb::Destination d : kDestinations)
    if (destination(d)->isEnabled() && destination(d)->isChecked()) ticked << name_of(d);
  settings()->setValue(QStringLiteral("publish/destinations"), ticked);
}

void PublishDialog::recall_destinations() {
  const QStringList ticked = settings()->value(QStringLiteral("publish/destinations")).toStringList();
  restoring_ = true;
  for (pb::Destination d : kDestinations)
    if (destination(d)->isEnabled()) destination(d)->setChecked(ticked.contains(name_of(d)));
  restoring_ = false;
}

}  // namespace pychron::ui
