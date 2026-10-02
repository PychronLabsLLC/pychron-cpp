#pragma once

// EvolutionsView (experiment-window design 5.4): the isotope evolutions of
// the run being measured. One graph per series (isotope on detector) of the
// selected kind (Signal, Baseline, Sniff), coloured by detector; a title with
// the run, block and count; the run's peak-center results underneath.
//
// Cleared when a run starts. Points arrive in the bridge's SeriesUpdated
// batches; repaints are capped at 20 Hz.

#include <map>
#include <string>
#include <vector>

#include <QColor>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include "pychron/experiment/collect/collector.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/systems/jobs/peak_center.hpp"

class QCustomPlot;
class QCPGraph;
class QLabel;
class QListWidget;
class QTabBar;

namespace pychron::ui {

class EvolutionsView : public QWidget {
  Q_OBJECT

 public:
  static constexpr int kRefreshMs = 50;

  // `colors`: detector -> colour (spectrometer config); others use the palette.
  explicit EvolutionsView(std::map<std::string, QColor> colors = {}, QWidget* parent = nullptr);

  void on_run_started(const experiment::executor::RunStarted& e);
  void on_series(const std::vector<experiment::collect::SeriesUpdated>& batch);
  void on_peak_center(const jobs::PeakCenterDone& e);
  void clear();

  void set_kind(experiment::collect::SeriesKind kind);
  experiment::collect::SeriesKind kind() const noexcept { return kind_; }

  // For tests.
  QCustomPlot* plot() const noexcept { return plot_; }
  int graph_count() const;                                     // graphs of the shown kind
  std::size_t point_count(experiment::collect::SeriesKind kind) const;  // over every series of `kind`
  QString title_text() const;
  QStringList peak_centers() const;
  QColor series_color(const experiment::collect::SeriesKey& key) const;  // invalid if unknown

 private:
  struct Line {
    QVector<double> t, v;
    QColor color;
  };

  void rebuild();  // graphs for the shown kind
  void refresh();  // push data, rescale, replot

  std::map<std::string, QColor> colors_;
  std::map<experiment::collect::SeriesKey, Line> lines_;
  std::vector<std::pair<experiment::collect::SeriesKey, QCPGraph*>> graphs_;
  experiment::collect::SeriesKind kind_ = experiment::collect::SeriesKind::Signal;
  QString run_;
  QString block_;
  QTabBar* tabs_;
  QLabel* title_;
  QCustomPlot* plot_;
  QListWidget* peaks_;
  QTimer refresh_;
  bool dirty_ = false;
};

}  // namespace pychron::ui
