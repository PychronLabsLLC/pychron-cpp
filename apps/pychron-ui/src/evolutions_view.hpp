#pragma once

// EvolutionsView (experiment-window design 5.4): the isotope evolutions of
// the run being measured. One graph per series (isotope on detector) of the
// selected kind (Signal, Baseline, Sniff), coloured by detector; a title with
// the run, block and count; the run's peak-center results underneath.
//
// Fit overlay: the collector's live fit of each series (FitsUpdated) is drawn
// as a curve in the series' colour from time zero to its last point, with a
// diamond at the intercept (time zero) and a grey cross on points the fit
// excluded as outliers. The intercepts are a table under the plot, a row per
// series: isotope, detector, fit, intercept, its error (absolute and percent)
// and the points the fit used.
//
// Each series of the shown kind is a panel of its own (isotope evolutions
// split design): stacked in arrival order on one time axis, each scaled to its
// own data, since isotopes differ by orders of magnitude. Tick labels and the
// time title are on the bottom panel only.
//
// A fourth tab, Peak center (peak center plot design 4.4), shows a
// PeakCenterView in place of the panels and the intercepts: it is selected
// when a peak center's try starts, and the Signal tab again when the next run
// starts. A line of the peak centers list brings that peak center back, while
// the view still keeps it (its newest 20 of the queue).
//
// Cleared when a run starts; the peak center graph and its kept results only
// when a queue starts (clear_peak_centers). Points arrive in the bridge's
// SeriesUpdated batches; repaints are capped at 20 Hz.
//
// A series' colour is its detector's config colour, or else a slot of the
// live palette; on PlotStyleHub::changed the panels take the new background
// and the palette series their slot's new colour. A config colour stays.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <QColor>
#include <QList>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include "pychron/experiment/collect/collector.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/reduction/fits.hpp"
#include "pychron/systems/jobs/peak_center.hpp"

class QCustomPlot;
class QCPAxisRect;
class QCPGraph;
class QCPMarginGroup;
class QScrollArea;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;
class QTabBar;
class QTableWidget;

namespace pychron::ui {

class PeakCenterView;

// One panel of the view: the series it draws and its y title.
struct EvolutionPanel {
  experiment::collect::SeriesKey key;
  QString title;
};

// Panels of `kind` in arrival order; `arrived` holds every key in the order
// its first point came. Titles: a signal "Ar40 H1"; a baseline "H1" (block
// baseline) or "Ar40 H1"; a sniff "Ar40" (sniffs are multicollection: one
// detector an isotope).
[[nodiscard]] std::vector<EvolutionPanel> evolution_panels(
    const std::vector<experiment::collect::SeriesKey>& arrived, experiment::collect::SeriesKind kind);

class EvolutionsView : public QWidget {
  Q_OBJECT

 public:
  static constexpr int kRefreshMs = 50;
  // A panel is never shorter: with more panels than the dock's height holds,
  // the plot grows and its area scrolls.
  static constexpr int kPanelMinHeight = 80;

  // `colors`: detector -> colour, the spectrometer config's only.
  // `palette_slots`: detector -> palette slot, for a detector without one
  // (its index in the config); a detector in neither takes the first slot
  // no series of its kind has.
  explicit EvolutionsView(std::map<std::string, QColor> colors = {},
                          std::map<std::string, std::size_t> palette_slots = {}, QWidget* parent = nullptr);

  void on_run_started(const experiment::executor::RunStarted& e);
  void on_series(const std::vector<experiment::collect::SeriesUpdated>& batch);
  void on_peak_center(const jobs::PeakCenterDone& e);
  void on_peak_center_try(const jobs::PeakCenterTryStarted& e);
  void on_peak_center_point(const jobs::PeakCenterPoint& e);
  void on_fits(const std::vector<experiment::collect::FitsUpdated>& batch);
  void clear();               // a run's: series, fits, the peak centers list
  void clear_peak_centers();  // a queue's: the peak center graph and its kept results

  void set_kind(experiment::collect::SeriesKind kind);
  experiment::collect::SeriesKind kind() const noexcept { return kind_; }

  // For tests.
  QCustomPlot* plot() const noexcept { return plot_; }
  int graph_count() const;                                     // panels of the shown kind
  // The panels, top to bottom: y titles, axis ranges as (lower, upper), and
  // whether a panel carries the time axis's labels.
  [[nodiscard]] QStringList panel_titles() const;
  [[nodiscard]] std::pair<double, double> panel_x_range(int i) const;
  [[nodiscard]] std::pair<double, double> panel_y_range(int i) const;
  [[nodiscard]] bool panel_has_time_labels(int i) const;
  [[nodiscard]] int panel_plot_height(int i) const;  // pixels inside the panel's axes, as last laid out
  [[nodiscard]] QScrollArea* scroll_area() const noexcept { return scroll_; }
  std::size_t point_count(experiment::collect::SeriesKind kind) const;  // over every series of `kind`
  QString title_text() const;
  QStringList peak_centers() const;
  // The intercepts table of the shown kind: its column titles, and each row's cells.
  QStringList intercept_columns() const;
  QList<QStringList> intercept_rows() const;
  // The latest fit of a series and its time zero, if one arrived.
  std::optional<reduction::Intercept> fit_of(const experiment::collect::SeriesKey& key) const;
  int fit_curve_count() const;  // fit curves drawn for the shown kind
  QColor series_color(const experiment::collect::SeriesKey& key) const;  // invalid if unknown
  [[nodiscard]] PeakCenterView* peak_center_view() const noexcept { return peak_view_; }

 private:
  struct Line {
    QVector<double> t, v;
    QColor color;
    std::optional<std::size_t> palette_slot;  // set: color follows the palette
    std::optional<reduction::Intercept> fit;
    double time_zero = 0;  // seconds since the epoch, as t
  };
  struct Graphs {
    experiment::collect::SeriesKey key;
    QString title;
    QCPAxisRect* rect = nullptr;
    QCPGraph* points = nullptr;
    QCPGraph* curve = nullptr;
    QCPGraph* intercept = nullptr;
    QCPGraph* excluded = nullptr;  // the fit's outliers
  };

  void on_tab(int index);
  void on_peak_line(const QListWidgetItem* item);
  void rebuild();  // graphs for the shown kind
  void refresh();  // push data, rescale, replot
  void restyle();  // PlotStyleHub::changed

  std::map<std::string, QColor> colors_;
  std::map<std::string, std::size_t> palette_slots_;
  std::map<experiment::collect::SeriesKey, Line> lines_;
  std::vector<experiment::collect::SeriesKey> arrived_;  // every key, in the order its first point came
  std::vector<Graphs> graphs_;                          // a panel a series of the shown kind
  experiment::collect::SeriesKind kind_ = experiment::collect::SeriesKind::Signal;
  QString run_;
  QString block_;
  QTabBar* tabs_;
  QLabel* title_;
  QCustomPlot* plot_;
  QCPMarginGroup* margins_;  // lines the panels' y axes up
  QScrollArea* scroll_;      // holds plot_
  QListWidget* peaks_;
  QTableWidget* intercepts_;
  QStackedWidget* pages_;    // the kind page, or the peak center view
  QWidget* kind_page_;       // title, panels, intercepts
  PeakCenterView* peak_view_;
  // Peak centers done since the queue started: a list line holds its number
  // among them, so that it finds its result while the view still keeps it.
  int peak_centers_done_ = 0;
  QTimer refresh_;
  bool dirty_ = false;
};

}  // namespace pychron::ui
