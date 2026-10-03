#pragma once

// The data windows over one analysis source: the browser, recall windows and
// figure windows, and the ProcessingBridge the figures use. The instrument's
// MainWindow holds one behind Window > Data; a data-reduction install's
// DataMainWindow holds one as its whole UI.

#include <functional>
#include <memory>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include "data_browser_window.hpp"
#include "preferences.hpp"
#include "processing_bridge.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/source.hpp"

namespace pychron::ui {

class DataWorkspace : public QObject {
  Q_OBJECT

 public:
  // Windows are parented to `owner` (they stay separate top-level windows);
  // `log` gets an error line when a recall fails.
  DataWorkspace(QWidget* owner, std::function<void(const QString&)> log);
  // Deletes every data window before the bridge they use.
  ~DataWorkspace() override;

  // Null source or presets disables data and deletes every window. Both must
  // outlive the workspace or be cleared first.
  void set_source(processing::IAnalysisSource* source, processing::PresetStore* presets);
  bool enabled() const noexcept { return processing_ != nullptr; }

  // The browser, made on first use. `embed_in` makes it a child widget there
  // (a data-reduction main window) instead of its own window.
  DataBrowserWindow* browser(QWidget* embed_in = nullptr);
  DataBrowserWindow* existing_browser() const noexcept { return browser_; }
  ProcessingBridge* processing_bridge() const noexcept { return processing_.get(); }
  // The browser's page size, for the browser open now and any made later.
  void set_page_size(int rows);

  // Null without data (or for an unknown figure kind).
  QWidget* open_recall(const QString& uuid);
  QWidget* open_figure(const QString& kind, const QStringList& uuids);

 private:
  void clear();

  QWidget* owner_;
  std::function<void(const QString&)> log_;
  processing::IAnalysisSource* source_ = nullptr;
  processing::PresetStore* presets_ = nullptr;
  std::unique_ptr<ProcessingBridge> processing_;
  DataBrowserWindow* browser_ = nullptr;
  int page_size_ = Preferences::kDefaultPageSize;
  QList<QPointer<QWidget>> children_;  // recall and figure windows
};

}  // namespace pychron::ui
