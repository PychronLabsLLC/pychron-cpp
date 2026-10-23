#include "earthbank_package_dialog.hpp"

#include <utility>

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "earthbank_worker.hpp"
#include "error_report.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace eb = pychron::earthbank;

EarthbankPackageDialog::EarthbankPackageDialog(EarthbankWorker& worker, eb::Profile profile, QString password,
                                               QWidget* parent)
    : QDialog(parent),
      worker_(worker),
      profile_(std::move(profile)),
      password_(std::move(password)),
      filter_(new QLineEdit),
      packages_(new QTableWidget(0, 3)),
      name_(new QLineEdit),
      create_(new QPushButton(tr("Create"))),
      status_(new QLabel),
      buttons_(new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel)) {
  setWindowTitle(tr("EarthBank data package"));
  resize(520, 420);

  filter_->setPlaceholderText(tr("Filter by name"));
  filter_->setClearButtonEnabled(true);
  packages_->setHorizontalHeaderLabels({tr("Name"), tr("Id"), tr("State")});
  packages_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  packages_->verticalHeader()->hide();
  packages_->setSelectionBehavior(QAbstractItemView::SelectRows);
  packages_->setSelectionMode(QAbstractItemView::SingleSelection);
  packages_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  name_->setPlaceholderText(tr("Name of a new package"));
  status_->setWordWrap(true);

  auto* create_row = new QHBoxLayout;
  create_row->addWidget(name_, 1);
  create_row->addWidget(create_);
  auto* layout = new QVBoxLayout(this);
  layout->addWidget(filter_);
  layout->addWidget(packages_, 1);
  layout->addLayout(create_row);
  layout->addWidget(status_);
  layout->addWidget(buttons_);

  connect(filter_, &QLineEdit::textChanged, this, [this] { apply_filter(); });
  connect(name_, &QLineEdit::textChanged, this, [this] { update_buttons(); });
  connect(packages_, &QTableWidget::itemSelectionChanged, this, [this] { update_buttons(); });
  connect(packages_, &QTableWidget::cellDoubleClicked, this, [this] {
    if (selected_id()) accept();
  });
  connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(create_, &QPushButton::clicked, this, [this] {
    creating_ = true;
    update_buttons();
    say(tr("Asking EarthBank to make the package…"), false);
    worker_.create_package(profile_, password_, name_->text().trimmed());
  });

  const QString mine = QString::fromStdString(profile_.name);
  connect(&worker_, &EarthbankWorker::packages_listed, this,
          [this, mine](const QString& of, const std::vector<eb::DataPackage>& packages) {
            if (of != mine) return;
            packages_->setRowCount(0);
            for (const eb::DataPackage& package : packages) add_row(package);
            apply_filter();
            say(packages.empty() ? tr("EarthBank lists no package here. Name one and press Create.") : QString(), false);
          });
  connect(&worker_, &EarthbankWorker::package_created, this, [this, mine](const QString& of, const eb::DataPackage& made) {
    if (of != mine) return;
    creating_ = false;
    filter_->clear();  // or the new one may be hidden
    name_->clear();
    packages_->selectRow(add_row(made));
    say(tr("Made package %1, id %2.").arg(QString::fromStdString(made.name)).arg(made.id), false);
    update_buttons();
  });
  connect(&worker_, &EarthbankWorker::package_failed, this,
          [this, mine](const QString& of, const QString& what, bool creating) {
            if (of != mine) return;
            // A list that failed says nothing of a Create that is still out.
            if (creating) creating_ = false;
            log_shown("earthbank", creating ? tr("Create package") : tr("List packages"), what);
            say(what, true);
            update_buttons();
          });

  say(tr("Asking EarthBank for the packages…"), false);
  update_buttons();
  worker_.list_packages(profile_, password_);
}

std::optional<std::int64_t> EarthbankPackageDialog::selected_id() const {
  const auto rows = packages_->selectionModel()->selectedRows();
  if (rows.isEmpty() || packages_->isRowHidden(rows.first().row())) return std::nullopt;
  return packages_->item(rows.first().row(), 1)->data(Qt::UserRole).toLongLong();
}

int EarthbankPackageDialog::add_row(const eb::DataPackage& package) {
  const int row = packages_->rowCount();
  packages_->insertRow(row);
  packages_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(package.name)));
  auto* id = new QTableWidgetItem(QString::number(package.id));
  id->setData(Qt::UserRole, static_cast<qlonglong>(package.id));
  packages_->setItem(row, 1, id);
  packages_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(package.workflow_state)));
  return row;
}

void EarthbankPackageDialog::apply_filter() {
  const QString wanted = filter_->text().trimmed();
  for (int row = 0; row < packages_->rowCount(); ++row) {
    packages_->setRowHidden(row, !packages_->item(row, 0)->text().contains(wanted, Qt::CaseInsensitive));
  }
  update_buttons();
}

void EarthbankPackageDialog::update_buttons() {
  create_->setEnabled(!creating_ && !name_->text().trimmed().isEmpty());
  buttons_->button(QDialogButtonBox::Ok)->setEnabled(selected_id().has_value());
}

void EarthbankPackageDialog::say(const QString& what, bool wrong) {
  status_->setText(what);
  style::set_tone(status_, wrong ? style::Tone::Error : style::Tone::Muted);
}

}  // namespace pychron::ui
