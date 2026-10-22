#pragma once

// View > Publish… (data publishing spec, section 6.3), contributed for as
// long as the owning main window lives. The Publish window is made on first
// use and kept; so is the Publish dialog (publication workflow design,
// section 6.1). Both work on the store through a bridge of their own, opened
// as a publisher client (the role elctl publish writes as), and build
// releases through a StoreSource these actions open.
//
// Lifetimes: the window and the dialog refer to the bridge and to the
// source, and a build they started reads the source on the bridge's worker.
// Members are declared source, bridge, window, dialog, so they go dialog and
// window first (no more callbacks), then the bridge (its destructor runs out
// the worker's jobs and joins it), then the source no job reads any more.

#include <functional>
#include <memory>
#include <string>

#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <QStringList>

#include "earthbank_worker.hpp"
#include "pychron/persistence/ids.hpp"

class QAction;
class QWidget;

namespace pychron::processing {
class StoreSource;
}

namespace pychron::ui {

class EntryBridge;
class PublishDialog;
class PublishWindow;

class PublishActions : public QObject {
  Q_OBJECT

 public:
  // `owner` is the main window the menu item belongs to. `selection` gives
  // the analyses a new release is made of (the Data browser's, as Export
  // takes them); it may be empty, and may give nothing.
  PublishActions(QWidget* owner, std::string store_url, std::function<QStringList()> selection = {});
  ~PublishActions() override;
  PublishActions(const PublishActions&) = delete;
  PublishActions& operator=(const PublishActions&) = delete;

  // Made on first use; nullptr when the store cannot be opened (said once
  // through report_error).
  PublishWindow* window();
  QAction* action() const noexcept { return action_; }  // "Publish…"

  // The Publish dialog, made on first use and kept; nullptr when the store
  // cannot be opened (said through report_error). open_dialog_new opens it on
  // a new release of `analyses` (uuid text), open_dialog_existing on that
  // release; while a run of it is under way it is only raised.
  void open_dialog_new(const QStringList& analyses);
  void open_dialog_existing(persistence::Uuid release);
  PublishDialog* dialog() const;  // null until opened

  // What the dialog's EarthBank tab talks to (default: EarthbankSetup::system())
  // and where it remembers its choices (empty, the default: the application's
  // QSettings). Taken by the dialog when it is made.
  void set_earthbank_setup(EarthbankSetup setup) { earthbank_ = std::move(setup); }
  using SettingsFactory = std::function<std::unique_ptr<QSettings>()>;
  void set_settings_factory(SettingsFactory settings) { settings_ = std::move(settings); }

  // How a store that cannot be opened is said (tests answer without a
  // dialog); a message box titled Publish by default.
  void set_report_error(std::function<void(const QString&)> report) { report_error_ = std::move(report); }

 private:
  void show_window();
  bool open_store();  // the source and the bridge, once
  PublishDialog* ensure_dialog();
  void show_dialog(const std::function<void(PublishDialog&)>& open);

  QWidget* owner_;
  std::string url_;
  std::function<QStringList()> selection_;
  std::function<void(const QString&)> report_error_;
  EarthbankSetup earthbank_;
  SettingsFactory settings_;
  std::unique_ptr<processing::StoreSource> source_;  // first: destroyed last
  std::unique_ptr<EntryBridge> bridge_;
  QPointer<PublishWindow> window_;  // last two: destroyed first
  QPointer<PublishDialog> dialog_;
  QAction* action_;
};

}  // namespace pychron::ui
