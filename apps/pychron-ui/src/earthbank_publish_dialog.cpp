#include "earthbank_publish_dialog.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <set>
#include <utility>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "brand.hpp"
#include "earthbank_package_dialog.hpp"
#include "pychron/earthbank/vocab.hpp"
#include "pychron/earthbank/xlsx.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace eb = pychron::earthbank;

namespace {

using Column = EarthbankPublishDialog::Column;

int index(Column column) { return static_cast<int>(column); }

QString qs(const std::string& text) { return QString::fromStdString(text); }

constexpr std::array<eb::AgeKind, 4> kAgeKinds = {eb::AgeKind::Plateau, eb::AgeKind::WeightedMean,
                                                  eb::AgeKind::Isochron, eb::AgeKind::Integrated};

QString kind_name(eb::AgeKind kind) {
  switch (kind) {
    case eb::AgeKind::Plateau: return QObject::tr("Plateau");
    case eb::AgeKind::WeightedMean: return QObject::tr("Weighted mean");
    case eb::AgeKind::Isochron: return QObject::tr("Isochron");
    case eb::AgeKind::Integrated: return QObject::tr("Integrated");
  }
  return {};
}

// What is kept in the settings for a kind: a word that does not change with
// the language of the interface.
QString kind_key(eb::AgeKind kind) {
  switch (kind) {
    case eb::AgeKind::Plateau: return QStringLiteral("plateau");
    case eb::AgeKind::WeightedMean: return QStringLiteral("weighted_mean");
    case eb::AgeKind::Isochron: return QStringLiteral("isochron");
    case eb::AgeKind::Integrated: return QStringLiteral("integrated");
  }
  return {};
}

QString age_text(const EarthbankCandidate& candidate) {
  if (!candidate.age) return QObject::tr("not available");
  const QString age = QString::number(*candidate.age, 'f', 3);
  if (!candidate.uncertainty) return QObject::tr("%1 Ma").arg(age);
  return QObject::tr("%1 ± %2 Ma").arg(age, QString::number(*candidate.uncertainty, 'f', 3));
}

// Under earthbank/samples/: a sample's name as one key (a name may hold `/`).
QString sample_group(const QString& sample) {
  return QStringLiteral("earthbank/samples/") + QString::fromLatin1(QUrl::toPercentEncoding(sample));
}

QString in_quotes(const std::string& value) {
  QString out = qs(value);
  for (QChar& c : out)
    if (c.unicode() < 0x20) c = QLatin1Char(' ');
  return QLatin1Char('\'') + out + QLatin1Char('\'');
}

QString times(std::size_t count) { return count > 1 ? QObject::tr(" (%1 records)").arg(count) : QString(); }

QString miss_line(const eb::Miss& miss) {
  const QString what = qs(miss.record + "." + miss.field) + QStringLiteral(": ") + in_quotes(miss.value);
  const QString verdict = miss.outcome == eb::LookupOutcome::CouldNotAsk ? QObject::tr(" could not be asked")
                                                                          : QObject::tr(" is not known to EarthBank");
  return what + verdict + times(miss.count) + (miss.detail.empty() ? QString() : QStringLiteral(": ") + qs(miss.detail));
}

QString word_for(eb::SampleStatus status) {
  switch (status) {
    case eb::SampleStatus::Complete: return QObject::tr("complete");
    case eb::SampleStatus::Partial: return QObject::tr("partial");
    case eb::SampleStatus::Stopped: return QObject::tr("stopped");
    case eb::SampleStatus::InDoubt: return QObject::tr("in doubt");
    case eb::SampleStatus::Refused: return QObject::tr("refused");
  }
  return QObject::tr("refused");
}

QString counted(std::size_t n, const QString& one, const QString& many) {
  return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
}

}  // namespace

EarthbankPublishDialog::EarthbankPublishDialog(std::vector<processing::AnalysisPtr> analyses, EarthbankSetup setup,
                                               SettingsFactory settings, QWidget* parent)
    : QDialog(parent),
      analyses_(std::move(analyses)),
      setup_(std::move(setup)),
      settings_(std::move(settings)),
      action_(new QComboBox),
      profile_(new QComboBox),
      package_(new QLineEdit),
      package_button_(new QPushButton(tr("Select / Create Package…"))),
      password_(new QLineEdit),
      hint_(new QLabel),
      table_(new QTableWidget(0, 6)),
      setup_name_(new QLineEdit),
      decay_constant_(new QLineEdit),
      air_ratio_(new QLineEdit),
      export_box_(new QGroupBox(tr("Workbooks"))),
      folder_(new QLineEdit),
      browse_(new QPushButton(tr("Browse…"))),
      per_sample_(new QCheckBox(tr("One workbook per sample"))),
      include_samples_(new QCheckBox(tr("Write the Sample workbook too"))),
      upload_box_(new QGroupBox(tr("Upload"))),
      allow_unresolved_(new QCheckBox(tr("Allow unresolved terms (send without the fields EarthBank does not know)"))),
      new_datapoint_(new QCheckBox(tr("Send as new records (the data changed since it was first uploaded)"))),
      run_(new QPushButton(tr("Run"))),
      cancel_(new QPushButton(tr("Cancel run"))),
      show_folder_(new QPushButton(tr("Show folder"))),
      in_doubt_box_(new QWidget),
      in_doubt_label_(new QLabel),
      in_doubt_id_(new QLineEdit),
      there_(new QPushButton(tr("It is there, with this id"))),
      not_there_(new QPushButton(tr("It is not there"))),
      progress_(new QProgressBar),
      result_(new QPlainTextEdit) {
  setWindowTitle(tr("Publish to EarthBank"));
  resize(980, 720);
  ask_confirm = [this](const QString& text) {
    return QMessageBox::question(this, tr("Upload to EarthBank"), text, QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) == QMessageBox::Yes;
  };
  show_folder = [](const QString& folder) { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); };

  action_->addItem(tr("Pre-flight: check the terms"), static_cast<int>(Action::Preflight));
  action_->addItem(tr("Export xlsx: write the workbooks"), static_cast<int>(Action::ExportXlsx));
  action_->addItem(tr("Upload"), static_cast<int>(Action::Upload));

  // The profiles are the file's, as Preferences left it; a file that cannot
  // be read leaves the workbooks, which need no account.
  if (auto file = eb::load_profiles(setup_.profiles_file)) {
    profiles_ = std::move(*file);
  } else {
    say(tr("The EarthBank profiles could not be read. %1").arg(qs(file.error().what)));
  }
  for (const eb::Profile& p : profiles_.profiles) profile_->addItem(qs(p.name));
  if (const int active = profile_->findText(qs(profiles_.active)); active >= 0) profile_->setCurrentIndex(active);
  package_->setPlaceholderText(tr("The data package's id"));
  password_->setEchoMode(QLineEdit::Password);
  password_->setPlaceholderText(tr("Leave empty to use the one that is kept"));
  hint_->setWordWrap(true);
  style::set_tone(hint_, style::Tone::Warning);

  table_->setHorizontalHeaderLabels(
      {tr("Sample"), tr("Analyses"), tr("Interpretation"), tr("Preferred age"), tr("Flux monitor"), tr("Notes")});
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->verticalHeader()->hide();
  table_->setSelectionMode(QAbstractItemView::NoSelection);
  table_->setWordWrap(true);

  setup_name_->setPlaceholderText(tr("The lab's analytical set-up, as EarthBank names it"));
  decay_constant_->setPlaceholderText(tr("From the value the reduction used"));
  air_ratio_->setPlaceholderText(tr("From the value the reduction used"));
  include_samples_->setChecked(true);
  cancel_->setEnabled(false);
  show_folder_->setEnabled(false);
  in_doubt_id_->setPlaceholderText(tr("Its id on EarthBank"));
  in_doubt_label_->setWordWrap(true);
  in_doubt_box_->hide();
  progress_->setRange(0, 1);
  progress_->setValue(0);
  result_->setReadOnly(true);
  result_->setPlaceholderText(tr("What was written, with its ids; what was not, and why."));

  if (secrets_ = setup_.secrets ? setup_.secrets() : nullptr; !secrets_) secrets_ = eb::make_memory_secret_store();
  worker_ = new EarthbankWorker(setup_.http, secrets_, this);

  auto* account = new QHBoxLayout;
  account->addWidget(new QLabel(tr("Profile:")));
  account->addWidget(profile_);
  account->addWidget(new QLabel(tr("Package:")));
  account->addWidget(package_);
  account->addWidget(package_button_);
  account->addWidget(new QLabel(tr("Password:")));
  account->addWidget(password_, 1);
  auto* names = new QFormLayout;
  names->addRow(tr("Analytical set-up:"), setup_name_);
  names->addRow(tr("Decay constant:"), decay_constant_);
  names->addRow(tr("Air ratio:"), air_ratio_);
  auto* folder_row = new QHBoxLayout;
  folder_row->addWidget(new QLabel(tr("Folder:")));
  folder_row->addWidget(folder_, 1);
  folder_row->addWidget(browse_);
  folder_row->addWidget(show_folder_);
  auto* export_layout = new QVBoxLayout(export_box_);
  export_layout->addLayout(folder_row);
  export_layout->addWidget(per_sample_);
  export_layout->addWidget(include_samples_);
  auto* upload_layout = new QVBoxLayout(upload_box_);
  upload_layout->addWidget(allow_unresolved_);
  upload_layout->addWidget(new_datapoint_);
  auto* doubt = new QHBoxLayout(in_doubt_box_);
  doubt->setContentsMargins(0, 0, 0, 0);
  doubt->addWidget(in_doubt_label_, 1);
  doubt->addWidget(in_doubt_id_);
  doubt->addWidget(there_);
  doubt->addWidget(not_there_);
  auto* run_row = new QHBoxLayout;
  run_row->addWidget(new QLabel(tr("Action:")));
  run_row->addWidget(action_);
  run_row->addWidget(run_);
  run_row->addWidget(cancel_);
  run_row->addWidget(progress_, 1);
  auto* close = new QPushButton(tr("Close"));
  auto* bottom = new QHBoxLayout;
  bottom->addStretch();
  bottom->addWidget(close);
  auto* layout = new QVBoxLayout(this);
  layout->addLayout(account);
  layout->addWidget(table_, 3);
  layout->addLayout(names);
  layout->addWidget(export_box_);
  layout->addWidget(upload_box_);
  layout->addLayout(run_row);
  layout->addWidget(hint_);
  layout->addWidget(in_doubt_box_);
  layout->addWidget(result_, 2);
  layout->addLayout(bottom);

  connect(close, &QPushButton::clicked, this, &QDialog::close);
  connect(action_, &QComboBox::currentIndexChanged, this, [this] { update_controls(); });
  connect(folder_, &QLineEdit::textChanged, this, [this] { update_controls(); });
  connect(in_doubt_id_, &QLineEdit::textChanged, this, [this] { update_controls(); });
  connect(profile_, &QComboBox::currentIndexChanged, this, [this] {
    // The package is the profile's until the user names another for this run.
    const auto p = current_profile();
    package_->setText(p && p->data_package_id ? QString::number(*p->data_package_id) : QString());
    password_->clear();
    update_controls();
  });
  connect(browse_, &QPushButton::clicked, this, [this] {
    const QString chosen = QFileDialog::getExistingDirectory(this, tr("Folder for the workbooks"), folder_->text());
    if (!chosen.isEmpty()) folder_->setText(chosen);
  });
  connect(show_folder_, &QPushButton::clicked, this, [this] {
    if (show_folder && !last_folder_.isEmpty()) show_folder(last_folder_);
  });
  connect(package_button_, &QPushButton::clicked, this, [this] {
    const auto p = current_profile();
    if (!p) return;
    auto* dialog = new EarthbankPackageDialog(*worker_, *p, password_->text(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog] {
      if (const auto id = dialog->selected_id()) package_->setText(QString::number(*id));
    });
    package_dialog_ = dialog;
    dialog->open();
  });
  connect(run_, &QPushButton::clicked, this, [this] { start(); });
  connect(cancel_, &QPushButton::clicked, this, [this] {
    worker_->cancel();
    cancel_->setEnabled(false);
    say(tr("Stopping after the record that is being sent…"));
  });
  connect(there_, &QPushButton::clicked, this,
          [this] { start("written:" + in_doubt_id_->text().trimmed().toStdString()); });
  connect(not_there_, &QPushButton::clicked, this, [this] { start(std::string("not-written")); });

  connect(worker_, &EarthbankWorker::progress, this, [this](int done, int total, const QString& what) {
    progress_->setRange(0, std::max(total, 1));
    progress_->setValue(std::min(done, std::max(total, 1)));
    if (surveyed_) say(what);  // a record written; the survey's samples are not each a line
  });
  connect(worker_, &EarthbankWorker::surveyed, this, [this](const std::vector<EarthbankSampleRow>& rows) {
    fill(rows);
    surveyed_ = true;
    progress_->setRange(0, 1);
    progress_->setValue(1);
    update_controls();
  });
  connect(worker_, &EarthbankWorker::failed, this, [this](const QString& profile, const QString& what) {
    if (package_dialog_ || !running_) return;  // the package dialog's own request
    say(profile.isEmpty() ? what : tr("%1: %2").arg(profile, what));
    finished();
  });
  connect(worker_, &EarthbankWorker::preflighted, this, [this](const eb::PreflightReport& report) {
    show_preflight(report);
    finished();
  });
  connect(worker_, &EarthbankWorker::exported, this, [this](const eb::XlsxResult& written) {
    for (const auto& file : written.files) say(tr("Wrote %1").arg(QString::fromStdString(file.filename().string())));
    for (const std::string& note : written.notes) say(tr("note: %1").arg(qs(note)));
    if (!written.files.empty()) {
      last_folder_ = QString::fromStdString(written.files.front().parent_path().string());
      say(tr("%n workbook(s) in %1. Import them on EarthBank's website.", nullptr, static_cast<int>(written.files.size()))
              .arg(last_folder_));
    }
    finished();
  });
  connect(worker_, &EarthbankWorker::upload_prepared, this, [this](const EarthbankConfirmation& c) {
    show_preflight(c.preflight);
    if (!c.blocked.isEmpty()) {
      say(c.blocked);
      finished();
      return;
    }
    if (!agreed_) {
      QStringList lines;
      lines << tr("Upload to %1 as profile %2?").arg(c.base_url, c.profile)
            << tr("Account: %1 (%2)").arg(qs(c.account.name), qs(c.account.login))
            << tr("Data package: %1, id %2").arg(qs(c.package.name)).arg(c.package.id) << QString();
      for (const auto& sample : c.samples) {
        lines << tr("%1: 1 data point, %2, %3, 1 age calculation, 1 age summary")
                     .arg(sample.name, counted(sample.aliquots, tr("aliquot"), tr("aliquots")),
                          counted(sample.measurements, tr("measurement"), tr("measurements")));
      }
      lines << QString() << tr("A record on EarthBank cannot be changed or removed from here.");
      if (!ask_confirm || !ask_confirm(lines.join(QLatin1Char('\n')))) {
        worker_->abandon_upload();
        say(tr("Nothing was sent."));
        finished();
        return;
      }
    }
    cancel_->setEnabled(true);
    worker_->upload();
  });
  connect(worker_, &EarthbankWorker::uploaded, this, [this](const eb::UploadReport& report) {
    show_upload(report);
    finished();
  });

  recall_fields();
  if (const auto p = current_profile(); p && p->data_package_id) package_->setText(QString::number(*p->data_package_id));
  update_controls();

  // What each sample would report, with nothing the lab has said: the
  // monitor found is the analyses' own, and every kind of age is tried.
  EarthbankPublication all;
  all.analyses = analyses_;
  all.options = base_options();
  worker_->survey(all);
}

EarthbankPublishDialog::~EarthbankPublishDialog() = default;

std::unique_ptr<QSettings> EarthbankPublishDialog::settings() const {
  return settings_ ? settings_() : std::make_unique<QSettings>();
}

EarthbankPublishDialog::Action EarthbankPublishDialog::current_action() const {
  return static_cast<Action>(action_->currentData().toInt());
}

std::optional<eb::Profile> EarthbankPublishDialog::current_profile() const {
  const eb::Profile* found = profiles_.find(profile_->currentText().toStdString());
  return found != nullptr ? std::optional<eb::Profile>(*found) : std::nullopt;
}

QComboBox* EarthbankPublishDialog::preferred_age(int row) const {
  return qobject_cast<QComboBox*>(table_->cellWidget(row, index(Column::PreferredAge)));
}

void EarthbankPublishDialog::fill(const std::vector<EarthbankSampleRow>& rows) {
  const auto kept = settings();
  table_->setRowCount(0);
  rows_.clear();
  for (const EarthbankSampleRow& found : rows) {
    const int row = table_->rowCount();
    table_->insertRow(row);
    const bool usable = found.refused.isEmpty();
    auto fixed = [&](const QString& text) {
      auto* item = new QTableWidgetItem(text);
      item->setFlags(usable ? Qt::ItemIsEnabled : Qt::NoItemFlags);
      return item;
    };
    auto editable = [&](const QString& text) {
      auto* item = new QTableWidgetItem(text);
      item->setFlags(usable ? Qt::ItemIsEnabled | Qt::ItemIsEditable | Qt::ItemIsSelectable : Qt::NoItemFlags);
      return item;
    };
    kept->beginGroup(sample_group(found.sample));
    table_->setItem(row, index(Column::Sample), fixed(found.sample));
    table_->setItem(row, index(Column::Analyses), fixed(QString::number(found.analyses)));
    table_->setItem(row, index(Column::Interpretation), editable(kept->value(QStringLiteral("interpretation")).toString()));
    table_->setItem(row, index(Column::FluxMonitor),
                    editable(kept->value(QStringLiteral("flux_monitor"), found.flux_monitor).toString()));

    // The rule first, with the age it gives; then each kind with its own.
    auto* ages = new QComboBox;
    if (found.rule) {
      const auto& picked = found.candidates[static_cast<std::size_t>(std::find(kAgeKinds.begin(), kAgeKinds.end(), *found.rule) -
                                                                      kAgeKinds.begin())];
      ages->addItem(tr("By the rule: %1 %2").arg(kind_name(*found.rule).toLower(), age_text(picked)), QString());
    } else {
      ages->addItem(tr("Choose an age…"), QString());
    }
    for (const EarthbankCandidate& candidate : found.candidates) {
      ages->addItem(tr("%1: %2").arg(kind_name(candidate.kind), age_text(candidate)), kind_key(candidate.kind));
      if (!candidate.age) {
        auto* model = qobject_cast<QStandardItemModel*>(ages->model());
        model->item(ages->count() - 1)->setEnabled(false);
        ages->setItemData(ages->count() - 1, candidate.unavailable, Qt::ToolTipRole);
      }
    }
    if (const int remembered = ages->findData(kept->value(QStringLiteral("preferred_age")).toString());
        remembered > 0 && found.candidates[static_cast<std::size_t>(remembered - 1)].age) {
      ages->setCurrentIndex(remembered);
    }
    ages->setEnabled(usable);
    connect(ages, &QComboBox::currentIndexChanged, this, [this] { update_controls(); });
    table_->setCellWidget(row, index(Column::PreferredAge), ages);
    kept->endGroup();

    Row kept_row{found, usable ? QString() : found.refused};
    auto* notes = fixed(kept_row.base_note);
    notes->setToolTip(usable ? found.notes.join(QLatin1Char('\n')) : found.refused);
    if (!usable) notes->setFlags(Qt::ItemIsEnabled);  // readable, though the row is out
    table_->setItem(row, index(Column::Notes), notes);
    rows_.push_back(std::move(kept_row));
  }
  table_->resizeColumnsToContents();
  table_->resizeRowsToContents();
}

bool EarthbankPublishDialog::included(int row, QString* why) const {
  if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) return false;  // a row still being filled
  const Row& r = rows_[static_cast<std::size_t>(row)];
  if (!r.found.refused.isEmpty()) {
    if (why != nullptr) *why = r.found.refused;
    return false;
  }
  if (!r.found.rule && preferred_age(row)->currentIndex() == 0) {
    if (why != nullptr) *why = tr("choose the age it reports. %1").arg(r.found.rule_says);
    return false;
  }
  return true;
}

eb::PublishOptions EarthbankPublishDialog::base_options() const {
  eb::PublishOptions options;
  options.software = (QStringLiteral("pychron-cpp ") + app_version()).toStdString();
  options.setup = setup_name_->text().trimmed().toStdString();
  options.decay_constant = decay_constant_->text().trimmed().toStdString();
  options.air_ratio = air_ratio_->text().trimmed().toStdString();
  return options;
}

EarthbankPublication EarthbankPublishDialog::publication() const {
  EarthbankPublication out;
  out.options = base_options();
  std::set<std::string> taking_part;
  for (int row = 0; row < table_->rowCount(); ++row) {
    if (!included(row)) continue;
    const Row& r = rows_[static_cast<std::size_t>(row)];
    eb::SampleOptions said;
    said.interpretation = table_->item(row, index(Column::Interpretation))->text().trimmed().toStdString();
    const int age = preferred_age(row)->currentIndex();
    if (age > 0) said.preferred_age = kAgeKinds[static_cast<std::size_t>(age - 1)];
    // The monitor only when it is not the one found: found, it is the analyses' own.
    const QString monitor = table_->item(row, index(Column::FluxMonitor))->text().trimmed();
    if (monitor != r.found.flux_monitor) said.flux_monitor = monitor.toStdString();
    const std::string sample = r.found.sample.toStdString();
    out.options.samples[sample] = std::move(said);
    taking_part.insert(sample);
  }
  for (const auto& analysis : analyses_) {
    if (taking_part.contains(analysis->sample)) out.analyses.push_back(analysis);
  }
  return out;
}

void EarthbankPublishDialog::remember() const {
  const auto kept = settings();
  for (int row = 0; row < table_->rowCount(); ++row) {
    const Row& r = rows_[static_cast<std::size_t>(row)];
    if (!r.found.refused.isEmpty()) continue;
    kept->beginGroup(sample_group(r.found.sample));
    kept->setValue(QStringLiteral("interpretation"), table_->item(row, index(Column::Interpretation))->text().trimmed());
    kept->setValue(QStringLiteral("preferred_age"), preferred_age(row)->currentData().toString());
    const QString monitor = table_->item(row, index(Column::FluxMonitor))->text().trimmed();
    if (monitor != r.found.flux_monitor) {
      kept->setValue(QStringLiteral("flux_monitor"), monitor);
    } else {
      kept->remove(QStringLiteral("flux_monitor"));
    }
    kept->endGroup();
  }
  kept->setValue(QStringLiteral("earthbank/setup"), setup_name_->text().trimmed());
  kept->setValue(QStringLiteral("earthbank/decay_constant"), decay_constant_->text().trimmed());
  kept->setValue(QStringLiteral("earthbank/air_ratio"), air_ratio_->text().trimmed());
  kept->setValue(QStringLiteral("earthbank/folder"), folder_->text());
}

void EarthbankPublishDialog::recall_fields() {
  const auto kept = settings();
  setup_name_->setText(kept->value(QStringLiteral("earthbank/setup")).toString());
  decay_constant_->setText(kept->value(QStringLiteral("earthbank/decay_constant")).toString());
  air_ratio_->setText(kept->value(QStringLiteral("earthbank/air_ratio")).toString());
  folder_->setText(kept->value(QStringLiteral("earthbank/folder")).toString());
}

void EarthbankPublishDialog::start(std::optional<std::string> resolve_in_doubt) {
  if (!surveyed_ || running_) return;
  remember();
  running_ = true;
  agreed_ = resolve_in_doubt.has_value();
  in_doubt_box_->hide();
  result_->clear();
  progress_->setRange(0, 0);  // busy until the first record says how many
  for (int row = 0; row < table_->rowCount(); ++row) {
    table_->item(row, index(Column::Notes))->setText(rows_[static_cast<std::size_t>(row)].base_note);
    QString why;
    if (!included(row, &why)) say(tr("%1: left out: %2").arg(rows_[static_cast<std::size_t>(row)].found.sample, why));
  }
  update_controls();

  const EarthbankPublication what = publication();
  const auto profile = current_profile();
  switch (current_action()) {
    case Action::ExportXlsx: {
      eb::XlsxOptions options;
      options.per_sample = per_sample_->isChecked();
      options.samples = include_samples_->isChecked();
      worker_->export_xlsx(what, std::filesystem::path(folder_->text().toStdString()), options);
      break;
    }
    case Action::Preflight:
      worker_->preflight(*profile, password_->text(), what);
      break;
    case Action::Upload: {
      EarthbankUploadSettings upload;
      bool ok = false;
      if (const qlonglong id = package_->text().trimmed().toLongLong(&ok); ok && id > 0) upload.package = id;
      // The package is the one shown: an empty field is no package, not the profile's.
      eb::Profile as = *profile;
      as.data_package_id = upload.package;
      upload.allow_unresolved = allow_unresolved_->isChecked();
      upload.new_datapoint = new_datapoint_->isChecked();
      upload.resolve_in_doubt = std::move(resolve_in_doubt);
      if (!setup_.profiles_file.empty()) upload.journal_dir = setup_.profiles_file.parent_path() / "journal";
      worker_->prepare_upload(as, password_->text(), what, upload);
      break;
    }
  }
}

void EarthbankPublishDialog::finished() {
  running_ = false;
  agreed_ = false;
  if (progress_->maximum() == 0) progress_->setRange(0, 1);
  progress_->setValue(progress_->maximum());
  update_controls();
}

void EarthbankPublishDialog::say(const QString& line) { result_->appendPlainText(line); }

void EarthbankPublishDialog::show_preflight(const eb::PreflightReport& report) {
  for (int row = 0; row < table_->rowCount(); ++row) {
    const std::string sample = rows_[static_cast<std::size_t>(row)].found.sample.toStdString();
    QStringList lines;
    for (const eb::Miss& miss : report.misses) {
      if (miss.sample != sample) continue;
      lines << miss_line(miss);
      say(tr("%1: %2").arg(qs(sample), miss_line(miss)));
    }
    if (!lines.isEmpty()) table_->item(row, index(Column::Notes))->setText(lines.join(QLatin1Char('\n')));
    for (const eb::SampleNotes& notes : report.notes) {
      if (notes.sample != sample) continue;
      for (const std::string& note : notes.notes) say(tr("%1: note: %2").arg(qs(sample), qs(note)));
    }
  }
  table_->resizeRowsToContents();
  if (!report.resolved.empty()) {
    say(tr("Found as something other than what is written (an id that is written cannot be corrected):"));
    for (const eb::Resolved& r : report.resolved) {
      say(tr("  %1: %2: %3 is %4%5%6")
              .arg(qs(r.sample), qs(r.record + "." + r.field), in_quotes(r.value), in_quotes(r.matched_name), times(r.count),
                   r.detail.empty() ? QString() : QStringLiteral("; ") + qs(r.detail)));
    }
  }
  if (!report.not_checked.empty()) {
    say(tr("Not checked:"));
    for (const eb::NotChecked& n : report.not_checked) say(tr("  %1: %2").arg(qs(n.field), qs(n.reason)));
  }
  if (report.clean()) {
    say(tr("Every term the upload would send is known to EarthBank."));
  } else {
    say(tr("%1 not known to EarthBank; %2 could not be asked about.")
            .arg(counted(report.unknown().size(), tr("term is"), tr("terms are")),
                 counted(report.could_not_ask().size(), tr("term"), tr("terms"))));
  }
}

void EarthbankPublishDialog::show_upload(const eb::UploadReport& report) {
  const eb::SampleReport* doubtful = nullptr;
  int doubts = 0;
  for (const eb::SampleReport& sample : report.samples) {
    QString line = tr("%1: %2").arg(qs(sample.sample), word_for(sample.status));
    if (sample.data_point_id) line += tr(", data point %1").arg(*sample.data_point_id);
    say(line);
    for (const std::string& message : sample.messages) say(QStringLiteral("  ") + qs(message));
    if (sample.in_doubt) {
      doubtful = &sample;
      ++doubts;
    } else if (sample.changed) {
      say(tr("  To send it as new records, tick \"Send as new records\" and run again."));
    } else if (sample.status == eb::SampleStatus::Partial || sample.status == eb::SampleStatus::Stopped) {
      say(tr("  To finish it, run the upload again: what is already there is not sent twice."));
    }
  }
  // The answer names no sample, so it can be given for one at a time.
  if (doubts == 1) {
    in_doubt_label_->setText(tr("%1: is the %2 on EarthBank? Look in the data package, then say which.")
                                 .arg(qs(doubtful->sample), qs(*doubtful->in_doubt)));
    in_doubt_id_->clear();
    in_doubt_box_->show();
  } else if (doubts > 1) {
    say(tr("More than one sample has a record in doubt. Each is settled on its own: run the upload again and it stops "
           "at the first."));
  }
}

void EarthbankPublishDialog::update_controls() {
  const Action action = current_action();
  const bool network = action != Action::ExportXlsx;
  const bool idle = surveyed_ && !running_;
  bool any = false;
  for (int row = 0; row < table_->rowCount(); ++row) any = any || included(row);

  QString hint;
  if (surveyed_ && !any) {
    hint = tr("No sample can be published as it stands: see the notes of each row.");
  } else if (network && profiles_.profiles.empty()) {
    hint = tr("There is no EarthBank profile yet: add one in File > Preferences, EarthBank. The workbooks need none.");
  } else if (action == Action::ExportXlsx && folder_->text().trimmed().isEmpty()) {
    hint = tr("Name the folder the workbooks go in.");
  }
  hint_->setText(hint);
  hint_->setVisible(!hint.isEmpty());

  run_->setEnabled(idle && hint.isEmpty());
  if (!running_) cancel_->setEnabled(false);
  action_->setEnabled(!running_);
  table_->setEnabled(idle);
  export_box_->setVisible(action == Action::ExportXlsx);
  upload_box_->setVisible(action == Action::Upload);
  profile_->setEnabled(network && idle);
  package_->setEnabled(action == Action::Upload && idle);
  package_button_->setEnabled(network && idle && current_profile().has_value());
  password_->setEnabled(network && idle);
  show_folder_->setEnabled(!last_folder_.isEmpty());
  bool id_ok = false;
  const qlonglong id = in_doubt_id_->text().trimmed().toLongLong(&id_ok);
  there_->setEnabled(idle && id_ok && id > 0);
  not_there_->setEnabled(idle);
}

}  // namespace pychron::ui
