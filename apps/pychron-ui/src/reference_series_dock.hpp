#pragma once

// ReferenceSeriesDock: the References pane of the experiment window
// (docs/superpowers/specs/2026-10-13-experiment-reference-series-design.md,
// 5.1 to 5.4). Two tabs, Blanks and Airs, each a time series of the newest
// references of the queue's spectrometer, drawn by a SceneView from what
// reference_pipeline() gives on the data workspace's ProcessingBridge. The
// dock computes nothing and reads nothing on the GUI thread: each tab submits
// on a channel of its own, with the source brought up to date on the
// bridge's worker first.
//
// It reads when first shown, on Refresh, when the figure options of a tab or
// "Last" change, and after a saved run (run_saved()). A run's RunFinished may
// come before the store has it: a read after a saved run that finds nothing
// new is made again 2 s and 10 s later in the session's time, then not again.

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include <QPointer>
#include <QString>
#include <QWidget>

#include "pychron/processing/options.hpp"
#include "processing_bridge.hpp"
#include "pychron/processing/scene.hpp"
#include "reference_series.hpp"

class QDialog;
class QLabel;
class QPushButton;
class QSettings;
class QSpinBox;
class QTabWidget;
class QToolButton;

namespace pychron::ui {

class SceneView;

class ReferenceSeriesDock : public QWidget {
  Q_OBJECT

 public:
  struct Deps {
    ProcessingBridge* bridge = nullptr;               // required; must outlive the dock
    processing::PresetStore* presets = nullptr;       // required; must outlive the dock
    std::function<double()> wall_now;                 // UTC epoch s, the session's
    std::function<std::string()> mass_spectrometer;   // the queue's; empty: any
    QSettings* settings = nullptr;                    // the window's; null: nothing kept
    // Calls `what` once, `delay` later in the session's time, on the GUI
    // thread. For the reads again after a saved run; null: none.
    std::function<void(std::chrono::seconds delay, std::function<void()> what)> later;
  };

  static constexpr int kMinLastN = 5;
  static constexpr int kMaxLastN = 500;
  static constexpr int kDefaultLastN = 50;

  explicit ReferenceSeriesDock(Deps deps, QWidget* parent = nullptr);
  ~ReferenceSeriesDock() override;
  ReferenceSeriesDock(const ReferenceSeriesDock&) = delete;
  ReferenceSeriesDock& operator=(const ReferenceSeriesDock&) = delete;
  ReferenceSeriesDock(ReferenceSeriesDock&&) = delete;
  ReferenceSeriesDock& operator=(ReferenceSeriesDock&&) = delete;

  // Both tabs; called when first shown, on Refresh, after a saved run.
  void read();
  // Only analyses from now on (the session's time), then read(). The reset
  // point is the dock's: it is not kept in the settings.
  void reset();
  // Clamped to 5..500 and kept in the settings; reads when the dock has
  // read before.
  void set_last_n(int n);
  [[nodiscard]] int last_n() const;
  // A run was saved: read, and read again 2 s and 10 s later while nothing
  // new is found.
  void run_saved();

  [[nodiscard]] SceneView* view(ReferenceKind kind) const;
  // "read 14:02:11" (the session's time), or "could not read: <why>".
  [[nodiscard]] QString status() const;
  // The preset the tab's options came from.
  [[nodiscard]] QString preset(ReferenceKind kind) const;
  [[nodiscard]] const processing::Options& figure_options(ReferenceKind kind) const;
  // The options dialog of a tab (a PresetBar and an OptionsEditor), opened
  // by the button on the tab; made when first asked for.
  QDialog* options_dialog(ReferenceKind kind);

 signals:
  // Every tab submitted has delivered its result (or its error).
  void read_finished();

 protected:
  void showEvent(QShowEvent* event) override;

 private:
  struct Tab {
    ReferenceKind kind = ReferenceKind::Blanks;
    int channel = 0;
    SceneView* view = nullptr;
    QToolButton* options_button = nullptr;
    QPointer<QDialog> dialog;
    processing::Options figure;
    QString preset;
    bool pending = false;  // submitted, result not yet delivered
    bool changed = false;  // the last result drew a scene not on show before
    std::optional<QString> error;
  };

  Tab& tab(ReferenceKind kind);
  const Tab& tab(ReferenceKind kind) const;
  void load_preset(Tab& tab, const QString& name);
  void submit(Tab& tab);
  void on_result(ReferenceKind kind, const PipelineResult& result);
  void finished();
  [[nodiscard]] std::optional<double> now() const;

  Deps deps_;
  std::array<Tab, 2> tabs_;
  QTabWidget* tab_widget_ = nullptr;
  QSpinBox* last_n_ = nullptr;
  QPushButton* reset_ = nullptr;
  QPushButton* refresh_ = nullptr;
  QLabel* status_ = nullptr;
  std::optional<double> since_;
  bool shown_ = false;    // the first showEvent has read
  bool started_ = false;  // a read has been asked for
  // After a saved run: how many of the reads again are left, and which saved
  // run they are for (a newer one replaces them).
  bool awaiting_new_ = false;
  std::size_t retries_made_ = 0;
  unsigned saved_serial_ = 0;
};

}  // namespace pychron::ui
