#pragma once

// PeakCenterWindow (spectrometer peak center design 5.1, 5.3): peak centring
// in a window of its own, opened from Spectrometer > Peak Center…. The
// PeakCenterPanel on the left runs and reports through the bridge by itself;
// the PeakCenterView on the right draws the run (startRequested clears it,
// the bridge's try and point events feed it, a finish that has a result,
// centred or failed, is kept). Splitter and geometry are saved under
// `peak_center_window/<spectrometer name>` when the window closes.
//
// Closing it while its run goes asks "Cancel peak center?": yes cancels the
// run with the scan put back (the scan is the spectrometer window's, which
// stays open) and closes once the run has finished; no keeps it open.

#include <filesystem>
#include <functional>
#include <memory>

#include <QMainWindow>
#include <QSettings>
#include <QString>

#include "spectrometer_bridge.hpp"

class QSplitter;

namespace pychron::ui {

class PeakCenterPanel;
class PeakCenterView;

class PeakCenterWindow : public QMainWindow {
  Q_OBJECT

 public:
  // `bridge` must outlive the window. `settings` defaults to the
  // application's QSettings; tests pass a temp file.
  PeakCenterWindow(SpectrometerBridge& bridge, bool simulation, std::unique_ptr<QSettings> settings = nullptr,
                   QWidget* parent = nullptr);

  // The peak_center.toml the panel reads its sets from and saves to (empty:
  // the built-in default only).
  void set_peak_center_file(const std::filesystem::path& file);
  // Whether a queue is running: the panel's Start and Apply are then disabled.
  void set_queue_running(std::function<bool()> queue_running);
  // A queue started or ended: the panel reads the state again.
  void queue_state_changed();
  // For code about to close and delete the window (a new spectrometer, quit):
  // cancels a run (scan left stopped) and waits for it, without the question
  // closeEvent asks.
  void cancel_peak_center_and_wait() { bridge_.cancel_peak_center_and_wait(); }
  bool peak_center_running() const { return bridge_.peak_center_running(); }
  PeakCenterPanel* peak_center_panel() const noexcept { return panel_; }
  PeakCenterView* peak_center_view() const noexcept { return view_; }
  // The question asked when the window is closed during a run ("Cancel peak
  // center?"). Default: a Yes/No QMessageBox, default No.
  void set_ask(std::function<bool(QString)> ask) { ask_ = std::move(ask); }

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  QString key(const char* name) const;

  SpectrometerBridge& bridge_;
  std::unique_ptr<QSettings> settings_;
  PeakCenterPanel* panel_ = nullptr;
  PeakCenterView* view_ = nullptr;
  QSplitter* splitter_ = nullptr;
  std::function<bool(QString)> ask_;
  bool closing_after_run_ = false;  // closeEvent's yes; close when the run has finished
};

}  // namespace pychron::ui
