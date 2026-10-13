#include "installations_dialog.hpp"

#include <utility>

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace pychron::ui {

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }
}  // namespace

InstallationsDialog::InstallationsDialog(std::filesystem::path site_path, const setup::ProfileLibrary* library,
                                         SetupWizard::OpenDatabase open_database, std::string current, QWidget* parent)
    : QDialog(parent),
      site_path_(std::move(site_path)),
      library_(library),
      open_database_(std::move(open_database)),
      current_(std::move(current)),
      list_(new QListWidget(this)),
      message_(new QLabel(this)),
      open_(new QPushButton(tr("Open"), this)),
      default_(new QPushButton(tr("Open by default"), this)),
      remove_(new QPushButton(tr("Remove from list"), this)),
      new_(new QPushButton(tr("New…"), this)) {
  setWindowTitle(tr("Installations"));
  resize(620, 360);
  message_->setWordWrap(true);
  auto* buttons = new QVBoxLayout;
  for (QPushButton* b : {open_, default_, remove_, new_}) buttons->addWidget(b);
  buttons->addStretch(1);
  auto* close = new QPushButton(tr("Close"), this);
  buttons->addWidget(close);
  auto* top = new QHBoxLayout;
  top->addWidget(list_, 1);
  top->addLayout(buttons);
  auto* layout = new QVBoxLayout(this);
  layout->addLayout(top, 1);
  layout->addWidget(message_);

  run_wizard = [](InstallationsDialog& self) -> std::optional<setup::SiteInstall> {
    SetupWizard wizard(*self.library_, SetupWizard::Options{self.site_path_, self.open_database_, {}, {}, self.seed_database_}, &self);
    if (wizard.exec() != QDialog::Accepted || !wizard.installed()) return std::nullopt;
    if (wizard.open_now()) self.to_open_ = wizard.installed()->name;
    return wizard.installed();
  };

  connect(close, &QPushButton::clicked, this, &QDialog::reject);
  connect(list_, &QListWidget::currentRowChanged, this, [this] { update_buttons(); });
  connect(list_, &QListWidget::itemDoubleClicked, this, [this] { open_->click(); });
  connect(open_, &QPushButton::clicked, this, [this] {
    const std::string name = selected();
    if (name.empty()) return;
    to_open_ = name;
    accept();
  });
  connect(default_, &QPushButton::clicked, this, [this] { make_default(); });
  connect(remove_, &QPushButton::clicked, this, [this] { remove_selected(); });
  connect(new_, &QPushButton::clicked, this, [this] {
    auto made = run_wizard(*this);
    reload();
    if (made && to_open_) accept();
  });
  new_->setEnabled(library_ != nullptr);
  reload();
}

void InstallationsDialog::reload() {
  const std::string keep = selected();
  list_->clear();
  auto site = setup::load_site(site_path_);
  if (!site) {
    message_->setText(qs(site.error().what));
    update_buttons();
    return;
  }
  for (const auto& i : site->installs) {
    QString text = qs(i.name);
    QStringList tags;
    if (i.name == site->default_install) tags << tr("opens by default");
    if (i.name == current_) tags << tr("open now");
    if (!tags.isEmpty()) text += QStringLiteral("  (%1)").arg(tags.join(QStringLiteral(", ")));
    text += QStringLiteral("\n    %1 — %2").arg(qs(i.profile), qs(i.root.string()));
    auto* item = new QListWidgetItem(text, list_);
    item->setData(Qt::UserRole, qs(i.name));
    if (i.name == keep || (keep.empty() && i.name == current_)) list_->setCurrentItem(item);
  }
  if (list_->currentRow() < 0 && list_->count() > 0) list_->setCurrentRow(0);
  if (site->installs.empty()) message_->setText(tr("Nothing is installed yet. New… sets up data reduction or an instrument."));
  update_buttons();
}

std::string InstallationsDialog::selected() const {
  const QListWidgetItem* item = list_->currentItem();
  return item ? item->data(Qt::UserRole).toString().toStdString() : std::string{};
}

void InstallationsDialog::update_buttons() {
  const bool any = !selected().empty();
  open_->setEnabled(any && selected() != current_);
  default_->setEnabled(any);
  remove_->setEnabled(any && selected() != current_);
}

void InstallationsDialog::make_default() {
  auto site = setup::load_site(site_path_);
  if (!site) return message_->setText(qs(site.error().what));
  site->default_install = selected();
  if (auto saved = setup::save_site(*site, site_path_); !saved) return message_->setText(qs(saved.error().what));
  message_->setText(tr("Pychron opens %1 when started without --install.").arg(qs(selected())));
  reload();
}

void InstallationsDialog::remove_selected() {
  const std::string name = selected();
  auto site = setup::load_site(site_path_);
  if (!site) return message_->setText(qs(site.error().what));
  const auto* entry = site->find(name);
  if (entry == nullptr) return;
  const QString root = qs(entry->root.string());
  site->remove(name);
  if (auto saved = setup::save_site(*site, site_path_); !saved) return message_->setText(qs(saved.error().what));
  message_->setText(tr("Removed %1 from the list. Its folder %2 is still there.").arg(qs(name), root));
  reload();
}

}  // namespace pychron::ui
