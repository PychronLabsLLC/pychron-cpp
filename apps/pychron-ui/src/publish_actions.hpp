#pragma once

// View > Publish… (data publishing spec, section 6.3), contributed for as
// long as the owning main window lives. The Publish window is made on first
// use and kept. It works on the store through a bridge of its own, opened as
// a publisher client (the role elctl publish writes as), and builds releases
// through a StoreSource these actions open.
//
// Lifetimes: the window refers to the bridge and to the source, and a build
// it started reads the source on the bridge's worker. Members are declared
// source, bridge, window, so they go window first (no more callbacks), then
// the bridge (its destructor runs out the worker's jobs and joins it), then
// the source no job reads any more.

#include <functional>
#include <memory>
#include <string>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QAction;
class QWidget;

namespace pychron::processing {
class StoreSource;
}

namespace pychron::ui {

class EntryBridge;
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

  // How a store that cannot be opened is said (tests answer without a
  // dialog); a message box titled Publish by default.
  void set_report_error(std::function<void(const QString&)> report) { report_error_ = std::move(report); }

 private:
  void show_window();

  QWidget* owner_;
  std::string url_;
  std::function<QStringList()> selection_;
  std::function<void(const QString&)> report_error_;
  std::unique_ptr<processing::StoreSource> source_;  // first: destroyed last
  std::unique_ptr<EntryBridge> bridge_;
  QPointer<PublishWindow> window_;  // last: destroyed first
  QAction* action_;
};

}  // namespace pychron::ui
