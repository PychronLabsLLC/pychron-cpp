#pragma once

// The data windows over one analysis source: the browser, recall windows and
// figure windows, and the ProcessingBridge the figures use. The instrument's
// MainWindow holds one behind View > Data; a data-reduction install's
// DataMainWindow holds one as its whole UI.

#include <functional>
#include <memory>
#include <utility>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include "data_browser_window.hpp"
#include "preferences.hpp"
#include "pychron/processing/domain.hpp"
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

  // Null source, presets or domain disables data and deletes every window. All
  // three must outlive the workspace or be cleared first. The domain says
  // which figures, recall sections and exporters the windows offer.
  void set_source(processing::IAnalysisSource* source, processing::PresetStore* presets,
                  const processing::Domain* domain);
  bool enabled() const noexcept { return processing_ != nullptr; }

  // The browser, made on first use. `embed_in` makes it a child widget there
  // (a data-reduction main window) instead of its own window.
  DataBrowserWindow* browser(QWidget* embed_in = nullptr);
  DataBrowserWindow* existing_browser() const noexcept { return browser_; }
  // DataBrowserWindow::set_unavailable, for the browser open now and any made later.
  void set_unavailable(const QString& reason);
  ProcessingBridge* processing_bridge() const noexcept { return processing_.get(); }
  // The browser's page size, time-break threshold and type colours, for the
  // browser open now and any made later.
  void apply_preferences(const Preferences& preferences);
  // Where the browser's "Color by" choice is kept (empty: the application's
  // QSettings), under data_browser/color_by.
  void set_settings(std::function<std::unique_ptr<QSettings>()> settings) { settings_ = std::move(settings); }

  // Null without data (or for a figure kind the domain does not offer).
  QWidget* open_recall(const QString& uuid);
  QWidget* open_figure(const QString& kind, const QStringList& uuids);
  // Reduces `uuids` as the domain does, groups them (by aliquot when any is a
  // heating step, else by identifier) and hands them to the domain's exporter
  // `exporter_id`, which writes `path`; the browser's status line says what was
  // written, or why not. False without data, or for an exporter the domain
  // does not have (said in the log and the status line). The exporter sees
  // the analyses as the store has them: no user edits, the default reduction.
  bool export_report(const QString& exporter_id, const QString& path, const QStringList& uuids);
  // Where the browser's Publish button sends the selected analyses (uuid
  // text): the Publish dialog, opened by whoever owns the store. With none
  // (an install without a database) the button is off and says why; set
  // after the browser exists, it takes effect on the browser open now.
  void set_publish_handler(std::function<void(const QStringList&)> handler);

 private:
  void clear();

  QWidget* owner_;
  std::function<void(const QString&)> log_;
  processing::IAnalysisSource* source_ = nullptr;
  processing::PresetStore* presets_ = nullptr;
  const processing::Domain* domain_ = nullptr;
  std::unique_ptr<ProcessingBridge> processing_;
  DataBrowserWindow* browser_ = nullptr;
  Preferences preferences_;
  QString unavailable_;
  std::function<std::unique_ptr<QSettings>()> settings_;
  QList<QPointer<QWidget>> children_;  // recall and figure windows
  int export_channel_ = 0;             // the bridge channel exports run on
  std::function<void(const QStringList&)> publish_handler_;
};

}  // namespace pychron::ui
