#include "publish_dialog.hpp"

#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
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

QString counted_analyses(qsizetype n) {
  return n == 1 ? QObject::tr("1 analysis") : QObject::tr("%1 analyses").arg(n);
}

}  // namespace

PublishDialog::PublishDialog(EntryBridge& bridge, processing::StoreSource& source, std::string store_url,
                             EarthbankSetup earthbank, SettingsFactory settings, QWidget* parent)
    : QDialog(parent),
      bridge_(bridge),
      source_(source),
      url_(std::move(store_url)),
      earthbank_(std::move(earthbank)),
      settings_(std::move(settings)),
      now_([] { return ps::UtcTime::now(); }),
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

  // Release section.
  auto* release_box = new QGroupBox(tr("Release"));
  auto* release_layout = new QVBoxLayout(release_box);
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
  auto* destination_box = new QGroupBox(tr("Destinations"));
  auto* destination_layout = new QHBoxLayout(destination_box);
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

  // Results (the run, task 6).
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
  // Run is driven by the run of task 6; until then it only says whether the
  // plan may run.

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(release_box);
  layout->addWidget(destination_box);
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
  connect(cancel_, &QPushButton::clicked, this, [this] { close(); });

  recall_destinations();
  update_release_section();
  update_tabs();
  update_store_dir();
  update_run();
}

PublishDialog::~PublishDialog() = default;

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
    panel_->set_read_only(fresh);
    if (fresh && !dataset_for_.isEmpty()) {
      // The panel is filled with the new release when the run makes it.
      dataset_for_.clear();
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
  connect(panel_, &EarthbankPublishPanel::controls_changed, this, [this] { update_run(); });
  connect(panel_->action(), &QComboBox::currentIndexChanged, this, [this] { schedule_check(); });
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
  const int generation = ++dataset_generation_;
  bridge_.run<std::vector<processing::AnalysisPtr>>(
      this,
      [&source = source_, release = *release](ps::IStore& store, const ps::Actor&) {
        return pb::release_dataset(store, source, release);
      },
      [this, generation](Result<std::vector<processing::AnalysisPtr>> analyses) {
        if (generation != dataset_generation_ || panel_ == nullptr) return;
        if (!analyses) {
          dataset_for_.clear();
          earthbank_note_->setText(tr("The release's analyses could not be read: %1").arg(qs(analyses.error().what)));
          earthbank_note_->show();
          return;
        }
        panel_->set_analyses(std::move(*analyses));
      });
}

void PublishDialog::schedule_check() {
  ++generation_;
  checked_ = false;
  update_run();
  check_timer_->start();
}

void PublishDialog::check() {
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
  const bool earthbank_ready = !destination(pb::Destination::EarthBank)->isChecked() || new_radio_->isChecked() ||
                               (panel_ != nullptr && panel_->can_run());
  run_->setEnabled(!busy() && any && checked_ && check_error_.isEmpty() && refusals_list_.empty() &&
                   local_refusals().empty() && plan().has_value() && earthbank_ready);
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
