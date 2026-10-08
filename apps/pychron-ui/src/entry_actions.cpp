#include "entry_actions.hpp"

#include <QAction>
#include <QMessageBox>

#include "entry_bridge.hpp"
#include "menu_hub.hpp"
#include "package_dialogs.hpp"
#include "packages_window.hpp"
#include "sample_import_dialog.hpp"
#include "samples_window.hpp"

namespace pychron::ui {

EntryActions::EntryActions(QWidget* owner, std::string store_url)
    : QObject(owner),
      owner_(owner),
      url_(std::move(store_url)),
      samples_action_(new QAction(tr("Samples…"), this)),
      packages_action_(new QAction(tr("Packages…"), this)) {
  auto* import_samples = new QAction(tr("Import Samples…"), this);
  auto* holders = new QAction(tr("Holders…"), this);
  auto* settings = new QAction(tr("Entry Settings…"), this);
  connect(samples_action_, &QAction::triggered, this, [this] {
    if (auto* w = samples()) {
      w->show();
      w->raise();
      w->activateWindow();
    }
  });
  connect(packages_action_, &QAction::triggered, this, [this] {
    if (auto* w = packages()) {
      w->show();
      w->raise();
      w->activateWindow();
    }
  });
  connect(import_samples, &QAction::triggered, this, [this] {
    if (auto* w = samples()) w->open_import();
  });
  connect(holders, &QAction::triggered, this, [this] {
    if (!ensure_bridge()) return;
    auto* dialog = new HoldersDialog(*bridge_, owner_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
  });
  connect(settings, &QAction::triggered, this, [this] {
    if (!ensure_bridge()) return;
    auto* dialog = new EntrySettingsDialog(*bridge_, owner_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
  });
  MenuHub::instance().contribute(owner_, MenuHub::Menu::Entry, {samples_action_, packages_action_}, MenuHub::Scope::App);
  MenuHub::instance().contribute(owner_, MenuHub::Menu::Entry, {import_samples, holders, settings}, MenuHub::Scope::App);
}

EntryActions::~EntryActions() {
  Q_EMIT closing();
  // The windows use the bridge: they go first.
  delete samples_.data();
  delete packages_.data();
}

bool EntryActions::ensure_bridge() {
  if (bridge_) return true;
  auto opened = EntryBridge::open({url_, {}, {}});
  if (!opened) {
    QMessageBox::critical(owner_, tr("Entry"),
                          tr("The store could not be opened for entry:\n%1").arg(QString::fromStdString(to_string(opened.error()))));
    return false;
  }
  bridge_ = std::move(*opened);
  return true;
}

SamplesWindow* EntryActions::samples() {
  if (!samples_ && ensure_bridge()) samples_ = new SamplesWindow(*bridge_, owner_);
  if (samples_) samples_->setWindowFlag(Qt::Window);
  return samples_;
}

PackagesWindow* EntryActions::packages() {
  if (!packages_ && ensure_bridge()) {
    packages_ = new PackagesWindow(*bridge_, owner_);
    connect(packages_, &PackagesWindow::flux_requested, this, &EntryActions::flux_requested);
  }
  if (packages_) packages_->setWindowFlag(Qt::Window);
  return packages_;
}

}  // namespace pychron::ui
