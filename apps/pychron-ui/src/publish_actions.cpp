#include "publish_actions.hpp"

#include <QAction>
#include <QMessageBox>
#include <QWidget>

#include "entry_bridge.hpp"
#include "menu_hub.hpp"
#include "publish_dialog.hpp"
#include "publish_window.hpp"

// Qt's `signals` keyword macro would rewrite persistence::CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/processing/store_source.hpp"
#pragma pop_macro("signals")

namespace pychron::ui {

PublishActions::PublishActions(QWidget* owner, std::string store_url, std::function<QStringList()> selection)
    : QObject(owner),
      owner_(owner),
      url_(std::move(store_url)),
      selection_(std::move(selection)),
      earthbank_(EarthbankSetup::system()),
      action_(new QAction(tr("Publish…"), this)) {
  report_error_ = [this](const QString& text) { QMessageBox::critical(owner_, tr("Publish"), text); };
  connect(action_, &QAction::triggered, this, [this] { show_window(); });
  MenuHub::instance().contribute(owner_, MenuHub::Menu::View, {action_}, MenuHub::Scope::App);
}

PublishActions::~PublishActions() {
  // No callback reaches them once they are gone; then the bridge, then the source.
  delete dialog_.data();
  delete window_.data();
}

bool PublishActions::open_store() {
  if (source_ == nullptr) {
    auto opened = processing::StoreSource::open(persistence::StoreConfig{url_, false}, {});
    if (!opened) {
      if (report_error_)
        report_error_(tr("The store %1 could not be opened:\n%2")
                          .arg(QString::fromStdString(url_), QString::fromStdString(to_string(opened.error()))));
      return false;
    }
    source_ = std::move(*opened);
  }
  if (bridge_ == nullptr) {
    EntryBridge::Options options;
    options.url = url_;
    options.role = "publisher";
#ifdef PYCHRON_VERSION
    options.software = std::string("pychron-ui/") + PYCHRON_VERSION;
#endif
    auto opened = EntryBridge::open(std::move(options));
    if (!opened) {
      if (report_error_)
        report_error_(tr("The store %1 could not be opened as publisher:\n%2")
                          .arg(QString::fromStdString(url_), QString::fromStdString(to_string(opened.error()))));
      return false;
    }
    bridge_ = std::move(*opened);
  }
  return true;
}

PublishWindow* PublishActions::window() {
  if (window_ != nullptr) return window_;
  if (!open_store()) return nullptr;
  window_ = new PublishWindow(*bridge_, *source_, url_, selection_, owner_);
  window_->setWindowFlag(Qt::Window);
  return window_;
}

void PublishActions::show_window() {
  PublishWindow* w = window();
  if (w == nullptr) return;
  w->refresh();
  w->show();
  w->raise();
  w->activateWindow();
}

PublishDialog* PublishActions::dialog() const { return dialog_; }

PublishDialog* PublishActions::ensure_dialog() {
  if (dialog_ != nullptr) return dialog_;
  if (!open_store()) return nullptr;
  dialog_ = new PublishDialog(*bridge_, *source_, url_, earthbank_, settings_, owner_);
  return dialog_;
}

void PublishActions::show_dialog(const std::function<void(PublishDialog&)>& open) {
  PublishDialog* d = ensure_dialog();
  if (d == nullptr) return;
  if (!d->busy()) open(*d);  // a run under way keeps its plan
  d->show();
  d->raise();
  d->activateWindow();
}

void PublishActions::open_dialog_new(const QStringList& analyses) {
  show_dialog([&analyses](PublishDialog& d) { d.open_new(analyses); });
}

void PublishActions::open_dialog_existing(persistence::Uuid release) {
  show_dialog([release](PublishDialog& d) { d.open_existing(release); });
}

}  // namespace pychron::ui
