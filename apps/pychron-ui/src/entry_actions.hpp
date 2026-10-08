#pragma once

// The Entry menu (entry spec, section 9): Samples…, Packages…, Import
// Samples…, Holders…, Entry Settings…, contributed for as long as the owning
// main window lives. The store is opened on first use, by an EntryBridge the
// actions own, and the windows are made once and kept.

#include <memory>
#include <string>

#include <QObject>
#include <QPointer>

class QAction;
class QWidget;

namespace pychron::ui {

class EntryBridge;
class PackagesWindow;
class SamplesWindow;

class EntryActions : public QObject {
  Q_OBJECT

 public:
  // `owner` is the main window the menu items belong to.
  EntryActions(QWidget* owner, std::string store_url);
  ~EntryActions() override;

  // Null until a window was opened (or when the store could not be opened).
  EntryBridge* bridge() const noexcept { return bridge_.get(); }
  SamplesWindow* samples();
  PackagesWindow* packages();
  QAction* samples_action() const noexcept { return samples_action_; }
  QAction* packages_action() const noexcept { return packages_action_; }
  // Opens the bridge on first use; a message box, and false, when the store
  // cannot be opened.
  bool ensure_bridge();

 Q_SIGNALS:
  // The Packages window's "Fit flux…", on the level it has open.
  void flux_requested(const QString& irradiation, const QString& level);
  // The first thing the destructor does: whoever else has a window on the
  // bridge (FitActions) deletes it now, while the bridge is still there.
  void closing();

 private:
  QWidget* owner_;
  std::string url_;
  std::unique_ptr<EntryBridge> bridge_;  // declared before the windows: they are deleted first
  QPointer<SamplesWindow> samples_;
  QPointer<PackagesWindow> packages_;
  QAction* samples_action_;
  QAction* packages_action_;
};

}  // namespace pychron::ui
