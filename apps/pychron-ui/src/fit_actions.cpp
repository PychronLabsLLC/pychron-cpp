#include "fit_actions.hpp"

#include <QAction>
#include <QMessageBox>

#include "entry_actions.hpp"
#include "entry_bridge.hpp"
#include "flux_window.hpp"
#include "menu_hub.hpp"
#include "packages_window.hpp"

// Qt's `signals` keyword macro would rewrite persistence::CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/processing/store_source.hpp"
#pragma pop_macro("signals")

namespace pychron::ui {

FitActions::FitActions(QWidget* owner, std::string store_url, EntryActions& entry, processing::PresetStore& presets,
                       std::function<void(const QString& uuid)> open_recall)
    : QObject(owner),
      owner_(owner),
      url_(std::move(store_url)),
      entry_(&entry),
      presets_(presets),
      open_recall_(std::move(open_recall)),
      flux_action_(new QAction(tr("Flux…"), this)) {
  report_error_ = [this](const QString& text) { QMessageBox::critical(owner_, tr("Flux"), text); };
  connect(flux_action_, &QAction::triggered, this, [this] { show_flux(); });
  connect(&entry, &EntryActions::flux_requested, this, &FitActions::open_flux);
  // The bridge is about to go: the window on it goes first.
  connect(&entry, &EntryActions::closing, this, [this] { delete flux_.data(); });
  MenuHub::instance().contribute(owner_, MenuHub::Menu::Fit, {flux_action_}, MenuHub::Scope::App);
}

FitActions::~FitActions() {
  delete flux_.data();
  // What the window asked for may still be read through the source: the
  // worker runs it out before the source goes. (With the bridge gone already,
  // its worker was joined.)
  if (entry_ != nullptr && source_ != nullptr) {
    if (EntryBridge* bridge = entry_->bridge())
      (void)bridge->run_sync<bool>([](persistence::IStore&, const persistence::Actor&) -> Result<bool> { return true; });
  }
}

FluxWindow* FitActions::flux() {
  if (flux_ != nullptr) return flux_;
  if (entry_ == nullptr) return nullptr;
  if (source_ == nullptr) {
    auto opened = processing::StoreSource::open(persistence::StoreConfig{url_, false}, {});
    if (!opened) {
      if (report_error_)
        report_error_(tr("The store could not be opened:\n%1").arg(QString::fromStdString(to_string(opened.error()))));
      return nullptr;
    }
    source_ = std::move(*opened);
  }
  if (!entry_->ensure_bridge()) return nullptr;
  flux_ = new FluxWindow(*entry_->bridge(), *source_, presets_, owner_);
  flux_->setWindowFlag(Qt::Window);
  flux_->set_open_recall(open_recall_);
  connect(flux_, &FluxWindow::packages_requested, this, [this](const QString& irradiation, const QString& level) {
    PackagesWindow* packages = entry_ != nullptr ? entry_->packages() : nullptr;
    if (packages == nullptr) return;
    packages->show_level(irradiation, level);
    packages->show();
    packages->raise();
    packages->activateWindow();
  });
  return flux_;
}

void FitActions::show_flux() {
  FluxWindow* w = flux();
  if (w == nullptr) return;
  w->show();
  w->raise();
  w->activateWindow();
}

void FitActions::open_flux(const QString& irradiation, const QString& level) {
  show_flux();
  if (flux_ != nullptr) flux_->open_level(irradiation, level);
}

}  // namespace pychron::ui
