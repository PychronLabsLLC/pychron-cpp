#pragma once

// The Fit menu (flux window design, section 5.1): Flux…, contributed for as
// long as the owning main window lives. The flux window is made on first use
// and kept; it reads levels through a StoreSource these actions open and own,
// and writes through the EntryBridge that EntryActions owns, so that a flux
// saved here reaches the Packages window (and "Fit flux…" there, this one).
//
// Lifetimes. The window refers to the bridge (EntryActions') and to the
// source (ours), and a level it asked for is read through the source on the
// bridge's worker. So, whichever of the two owners goes first:
//   1. the window is destroyed before the bridge, and
//   2. the bridge's worker has run every job before the source is destroyed.
// Both owners are children of the main window, EntryActions made first, and
// Qt deletes children in the order they were made:
//   - EntryActions first (the application): its destructor starts with
//     closing(), on which the window is deleted here (1); when it returns the
//     bridge is gone and its worker joined, and the source dies after it, in
//     our destructor (2).
//   - FitActions first (a test): the destructor deletes the window (1), then
//     waits for the worker to run what was posted (2), and only then are the
//     members, the source among them, destroyed.

#include <functional>
#include <memory>
#include <string>

#include <QObject>
#include <QPointer>
#include <QString>

#include "pychron/processing/options.hpp"

class QAction;
class QWidget;

namespace pychron::processing {
class StoreSource;
}

namespace pychron::ui {

class EntryActions;
class FluxWindow;

class FitActions : public QObject {
  Q_OBJECT

 public:
  // `owner` is the main window the menu item belongs to. `presets` must
  // outlive this; `entry` may go first (see above). `open_recall` (may be
  // empty) is what the plot's Recall does with an analysis.
  FitActions(QWidget* owner, std::string store_url, EntryActions& entry, processing::PresetStore& presets,
             std::function<void(const QString& uuid)> open_recall = {});
  ~FitActions() override;

  // Made on first use; nullptr when the store cannot be opened (said through
  // report_error, or by EntryActions for its bridge) and once `entry` is gone.
  FluxWindow* flux();
  // Shows the window on that level.
  void open_flux(const QString& irradiation, const QString& level);
  QAction* flux_action() const noexcept { return flux_action_; }  // "Flux…"

  // How a store that cannot be opened is said (tests answer without a
  // dialog); a message box titled Flux by default.
  void set_report_error(std::function<void(const QString&)> report) { report_error_ = std::move(report); }

 private:
  void show_flux();

  QWidget* owner_;
  std::string url_;
  QPointer<EntryActions> entry_;
  processing::PresetStore& presets_;
  std::function<void(const QString&)> open_recall_;
  std::function<void(const QString&)> report_error_;
  std::unique_ptr<processing::StoreSource> source_;  // declared before the window: it is deleted first
  QPointer<FluxWindow> flux_;
  QAction* flux_action_;
};

}  // namespace pychron::ui
