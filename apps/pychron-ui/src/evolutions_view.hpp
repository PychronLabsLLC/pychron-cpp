#pragma once

// EvolutionsView (experiment-window design 5.4): the isotope evolutions of
// the run being measured. One graph per series (isotope on detector) of the
// selected kind (Signal, Baseline, Sniff), coloured by detector; a title with
// the run, block and count; the run's peak-center results underneath.
//
// Fit overlay: the collector's live fit of each series (FitsUpdated) is drawn
// as a curve in the series' colour from time zero to its last point, with a
// diamond at the intercept (time zero) and a grey cross on points the fit
// excluded as outliers. The intercepts are listed under the plot.
//
// Isotopes differ by orders of magnitude, so a series selector shows either
// every series of the kind or one, with the axes scaled to it.
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
#include "pychron/reduction/fits.hpp"
#include "pychron/systems/jobs/peak_center.hpp"

class QCustomPlot;
class QCPGraph;
class QComboBox;
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
  void on_fits(const std::vector<experiment::collect::FitsUpdated>& batch);
  void clear();

  void set_kind(experiment::collect::SeriesKind kind);
  // Shows only `key` ("All" when nullopt); a key not of the shown kind shows nothing.
  void set_focus(std::optional<experiment::collect::SeriesKey> key);
  std::optional<experiment::collect::SeriesKey> focus() const noexcept { return focus_; }
  QStringList focus_choices() const;  // "All series", then the shown kind's series
  experiment::collect::SeriesKind kind() const noexcept { return kind_; }

  // For tests.
  QCustomPlot* plot() const noexcept { return plot_; }
  int graph_count() const;                                     // graphs of the shown kind
  std::size_t point_count(experiment::collect::SeriesKind kind) const;  // over every series of `kind`
  QString title_text() const;
  QStringList peak_centers() const;
  QStringList intercept_lines() const;  // the listed intercepts of the shown kind
  // The latest fit of a series and its time zero, if one arrived.
  std::optional<reduction::Intercept> fit_of(const experiment::collect::SeriesKey& key) const;
  int fit_curve_count() const;  // fit curves drawn for the shown kind
  QColor series_color(const experiment::collect::SeriesKey& key) const;  // invalid if unknown

 private:
  struct Line {
    QVector<double> t, v;
    QColor color;
    std::optional<reduction::Intercept> fit;
    double time_zero = 0;  // seconds since the epoch, as t
  };
  struct Graphs {
    experiment::collect::SeriesKey key;
    QCPGraph* points = nullptr;
    QCPGraph* curve = nullptr;
    QCPGraph* intercept = nullptr;
  };

  void rebuild();  // graphs for the shown kind
  void refresh();  // push data, rescale, replot

  std::map<std::string, QColor> colors_;
  std::map<experiment::collect::SeriesKey, Line> lines_;
  std::vector<Graphs> graphs_;
  QCPGraph* excluded_ = nullptr;  // outliers of every shown fit
  experiment::collect::SeriesKind kind_ = experiment::collect::SeriesKind::Signal;
  std::optional<experiment::collect::SeriesKey> focus_;
  void fill_focus();
  QString run_;
  QString block_;
  QTabBar* tabs_;
  QComboBox* focus_box_;
  QLabel* title_;
  QCustomPlot* plot_;
  QListWidget* peaks_;
  QListWidget* intercepts_;
  QTimer refresh_;
  bool dirty_ = false;
};

}  // namespace pychron::ui
