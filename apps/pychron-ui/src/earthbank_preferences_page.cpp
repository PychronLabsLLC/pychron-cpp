#include "earthbank_preferences_page.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

#include "earthbank_package_dialog.hpp"
#include "preferences_dialog.hpp"
#include "pychron/core/error.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace eb = pychron::earthbank;

namespace {

int index(EarthbankPreferencesPage::Column column) { return static_cast<int>(column); }

// An id cell: empty is none; anything but a whole number above zero is wrong.
bool read_id(const QString& text, std::optional<std::int64_t>& id) {
  id.reset();
  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty()) return true;
  bool ok = false;
  const qlonglong value = trimmed.toLongLong(&ok);
  if (!ok || value <= 0) return false;
  id = value;
  return true;
}

}  // namespace

EarthbankPreferencesPage::EarthbankPreferencesPage(EarthbankSetup setup, QWidget* parent)
    : QWidget(parent),
      setup_(std::move(setup)),
      table_(new QTableWidget(0, 6)),
      add_(new QPushButton(tr("Add"))),
      remove_(new QPushButton(tr("Remove"))),
      test_(new QPushButton(tr("Test"))),
      package_(new QPushButton(tr("Select / Create Package…"))),
      password_(new QLineEdit),
      store_line_(new QLabel),
      status_(new QLabel) {
  table_->setHorizontalHeaderLabels(
      {tr("Active"), tr("Name"), tr("Server"), tr("Username"), tr("Institution id"), tr("Data package id")});
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(index(Column::Server), QHeaderView::Stretch);
  table_->verticalHeader()->hide();
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  password_->setEchoMode(QLineEdit::Password);
  password_->setPlaceholderText(tr("Leave empty to use the one that is kept"));
  status_->setWordWrap(true);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  style::set_tone(store_line_, style::Tone::Muted);

  auto* row_buttons = new QHBoxLayout;
  row_buttons->addWidget(add_);
  row_buttons->addWidget(remove_);
  row_buttons->addStretch();
  row_buttons->addWidget(test_);
  row_buttons->addWidget(package_);
  auto* password_form = new QFormLayout;
  password_form->addRow(tr("Password of the selected profile:"), password_);
  auto* layout = new QVBoxLayout(this);
  layout->addWidget(table_, 1);
  layout->addLayout(row_buttons);
  layout->addLayout(password_form);
  layout->addWidget(store_line_);
  layout->addWidget(status_);
  layout->addWidget(preferences_note(tr("One row for each account on EarthBank. The active one is used when none is "
                                        "named; elctl earthbank reads the same profiles. A password is never written "
                                        "to the profiles file.")));

  connect(add_, &QPushButton::clicked, this, [this] {
    table_->selectRow(add_row(eb::Profile{}, table_->rowCount() == 0));
    update_buttons();
  });
  connect(remove_, &QPushButton::clicked, this, [this] {
    if (const int row = selected_row(); row >= 0) table_->removeRow(row);
    update_buttons();
  });
  connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
    password_->clear();  // it was for the row that was selected
    update_buttons();
  });
  // One active profile: ticking a row unticks the others.
  connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
    if (changing_ || item->column() != index(Column::Active) || item->checkState() != Qt::Checked) return;
    changing_ = true;
    for (int row = 0; row < table_->rowCount(); ++row) {
      if (row != item->row()) table_->item(row, index(Column::Active))->setCheckState(Qt::Unchecked);
    }
    changing_ = false;
  });
  connect(test_, &QPushButton::clicked, this, [this] {
    const auto profile = selected_profile();
    if (!profile) return;
    say(tr("Signing in to %1…").arg(QString::fromStdString(profile->base_url)), false);
    worker_->test(*profile, password_->text());
  });
  connect(package_, &QPushButton::clicked, this, [this] {
    const auto profile = selected_profile();
    if (!profile) return;
    say(QString(), false);
    auto* dialog = new EarthbankPackageDialog(*worker_, *profile, password_->text(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const int row = selected_row();
    connect(dialog, &QDialog::accepted, this, [this, dialog, row] {
      if (const auto id = dialog->selected_id(); id && row < table_->rowCount()) {
        table_->item(row, index(Column::Package))->setText(QString::number(*id));
      }
    });
    package_dialog_ = dialog;
    dialog->open();
  });
  update_buttons();
}

EarthbankPreferencesPage::~EarthbankPreferencesPage() = default;

EarthbankPackageDialog* EarthbankPreferencesPage::package_dialog() const { return package_dialog_; }

void EarthbankPreferencesPage::set_setup(EarthbankSetup setup) {
  if (!loaded_) setup_ = std::move(setup);
}

void EarthbankPreferencesPage::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  load();
}

void EarthbankPreferencesPage::load() {
  if (loaded_) return;
  loaded_ = true;
  if (setup_.secrets) secrets_ = setup_.secrets();
  if (!secrets_) secrets_ = eb::make_memory_secret_store();
  store_line_->setText(earthbank_store_line(*secrets_));
  worker_ = new EarthbankWorker(setup_.http, secrets_, this);
  connect(worker_, &EarthbankWorker::outstanding_changed, this, [this] { update_buttons(); });
  connect(worker_, &EarthbankWorker::tested, this,
          [this](const QString& profile, const QString& name, const QString& login, const QString& note) {
            QString line = tr("%1: signed in as %2 (%3).").arg(profile, name, login);
            // The password that signed in is kept now: the field has done its work.
            if (note.isEmpty()) {
              password_->clear();
            } else {
              line += QLatin1Char(' ') + note;
            }
            say(line, !note.isEmpty());
          });
  connect(worker_, &EarthbankWorker::failed, this, [this](const QString& profile, const QString& what) {
    if (package_dialog_) return;  // the dialog says it
    say(tr("%1: %2").arg(profile, what), true);
  });
  connect(worker_, &EarthbankWorker::password_kept, this, [this](const QString& profile, const QString& problem) {
    if (!problem.isEmpty()) say(tr("%1: the password store failed. %2").arg(profile, problem), true);
  });

  auto file = eb::load_profiles(setup_.profiles_file);
  if (!file) {
    // What is there is not ours to replace: say so and write nothing.
    unreadable_ = true;
    say(tr("The profiles could not be read, and nothing here will be saved. %1")
            .arg(QString::fromStdString(file.error().what)),
        true);
    table_->setEnabled(false);
    update_buttons();
    return;
  }
  saved_ = std::move(*file);
  saved_.notes.clear();
  for (const eb::Profile& profile : saved_.profiles) add_row(profile, profile.name == saved_.active);
  if (table_->rowCount() > 0) table_->selectRow(0);
  update_buttons();
}

int EarthbankPreferencesPage::add_row(const eb::Profile& profile, bool active) {
  const QSignalBlocker quiet(table_);
  const int row = table_->rowCount();
  table_->insertRow(row);
  auto* tick = new QTableWidgetItem;
  tick->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
  tick->setCheckState(active ? Qt::Checked : Qt::Unchecked);
  table_->setItem(row, index(Column::Active), tick);
  auto id_text = [](const std::optional<std::int64_t>& id) { return id ? QString::number(*id) : QString(); };
  table_->setItem(row, index(Column::Name), new QTableWidgetItem(QString::fromStdString(profile.name)));
  table_->setItem(row, index(Column::Server), new QTableWidgetItem(QString::fromStdString(profile.base_url)));
  table_->setItem(row, index(Column::Username), new QTableWidgetItem(QString::fromStdString(profile.username)));
  table_->setItem(row, index(Column::Institution), new QTableWidgetItem(id_text(profile.institution_id)));
  table_->setItem(row, index(Column::Package), new QTableWidgetItem(id_text(profile.data_package_id)));
  return row;
}

int EarthbankPreferencesPage::selected_row() const {
  const auto rows = table_->selectionModel()->selectedRows();
  return rows.isEmpty() ? -1 : rows.first().row();
}

eb::Profiles EarthbankPreferencesPage::profiles(QString& problem) const {
  eb::Profiles all;
  auto text = [this](int row, Column column) { return table_->item(row, index(column))->text().trimmed(); };
  for (int row = 0; row < table_->rowCount(); ++row) {
    eb::Profile p;
    p.name = text(row, Column::Name).toStdString();
    p.base_url = text(row, Column::Server).toStdString();
    p.username = text(row, Column::Username).toStdString();
    const QString shown = p.name.empty() ? tr("row %1").arg(row + 1) : text(row, Column::Name);
    if (!read_id(text(row, Column::Institution), p.institution_id)) {
      problem = tr("EarthBank profile %1: the institution id is not a number.").arg(shown);
      return all;
    }
    if (!read_id(text(row, Column::Package), p.data_package_id)) {
      problem = tr("EarthBank profile %1: the data package id is not a number.").arg(shown);
      return all;
    }
    if (table_->item(row, index(Column::Active))->checkState() == Qt::Checked) all.active = p.name;
    all.profiles.push_back(std::move(p));
  }
  return all;
}

std::optional<eb::Profile> EarthbankPreferencesPage::selected_profile() {
  const int row = selected_row();
  if (row < 0 || worker_ == nullptr) return std::nullopt;
  QString problem;
  const eb::Profiles all = profiles(problem);
  if (!problem.isEmpty()) {
    say(problem, true);
    return std::nullopt;
  }
  const eb::Profile& profile = all.profiles[static_cast<std::size_t>(row)];
  if (!eb::valid_profile_name(profile.name)) {
    say(tr("Give the profile a name first: letters, digits, - and _."), true);
    return std::nullopt;
  }
  if (const auto url = eb::checked_base_url(profile.base_url); !url) {
    say(QString::fromStdString(url.error().what), true);
    return std::nullopt;
  }
  return profile;
}

QString EarthbankPreferencesPage::apply() {
  if (!loaded_ || unreadable_) return {};
  QString problem;
  eb::Profiles now = profiles(problem);
  if (!problem.isEmpty()) return problem;
  if (now.active != saved_.active || now.profiles != saved_.profiles) {
    if (const auto written = eb::save_profiles(now, setup_.profiles_file); !written) {
      return tr("The EarthBank profiles were not saved. %1").arg(QString::fromStdString(written.error().what));
    }
    // A profile that is gone (or now names another account) takes its password with it.
    for (const eb::Profile& old : saved_.profiles) {
      const std::string account = eb::secret_account(old);
      const bool kept = std::any_of(now.profiles.begin(), now.profiles.end(),
                                    [&](const eb::Profile& p) { return eb::secret_account(p) == account; });
      if (!kept) worker_->erase_password(old);
    }
  }
  if (const int row = selected_row(); row >= 0 && !password_->text().isEmpty()) {
    const eb::Profile& profile = now.profiles[static_cast<std::size_t>(row)];
    if (!eb::valid_profile_name(profile.name) || !eb::checked_base_url(profile.base_url)) {
      return tr("The password was not kept: its profile needs a name and a server first.");
    }
    worker_->store_password(profile, password_->text());
    password_->clear();
  }
  saved_ = std::move(now);
  return {};
}

void EarthbankPreferencesPage::say(const QString& what, bool wrong) {
  status_->setText(what);
  style::set_tone(status_, wrong ? style::Tone::Error : style::Tone::Normal);
}

void EarthbankPreferencesPage::update_buttons() {
  const bool usable = loaded_ && !unreadable_;
  const bool row = usable && selected_row() >= 0;
  const bool idle = worker_ == nullptr || worker_->outstanding() == 0;
  add_->setEnabled(usable);
  remove_->setEnabled(row);
  test_->setEnabled(row && idle);
  package_->setEnabled(row && idle);
  password_->setEnabled(row);
}

}  // namespace pychron::ui
