#pragma once

// PeakCenterView (peak center plot design 4.4): the sweep of a peak center,
// live and after it. x is the magnet value applied (native units), y the
// reference detector's intensity; each additional detector is scaled onto
// the reference's range within its try (scale_to_reference), for display
// only. Earlier tries of the same peak center stay, drawn faint. When it is
// done: low, centre and high marked from the reference's PeakShape, a rule at
// the fitted centre beside the one at the initial centre, and the outcome in
// the title. The newest 20 results are kept and can be shown again.
//
// State is plain data and one redraw() builds every graph from it, so the
// live events and PeakCenterDone alone give the same picture. Fed on the GUI
// thread; it never reads the spectrometer or a clock.

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

#include "pychron/systems/jobs/peak_center.hpp"

class QCPRange;
class QCustomPlot;
class QLabel;
class QMouseEvent;

namespace pychron::ui {

class PeakCenterView : public QWidget {
  Q_OBJECT

 public:
  static constexpr std::size_t kKept = 20;

  // `colors`: a colour per detector, as the spectrometer config gives them.
  // `palette_slots`: the live palette's slot of a detector without one; a
  // detector in neither takes its place in the sweep. Background and palette
  // colours follow PlotStyleHub::changed.
  explicit PeakCenterView(std::map<std::string, QColor> colors = {},
                          std::map<std::string, std::size_t> palette_slots = {}, QWidget* parent = nullptr);

  void on_try_started(const jobs::PeakCenterTryStarted& e);
  void on_point(const jobs::PeakCenterPoint& e);
  void on_done(const jobs::PeakCenterDone& e);  // keeps the result; draws it whole
  void show_result(int index);                   // 0 = oldest kept
  void clear();                                  // queue start: graph and kept results
  void abandon();                                // a run started with a try still open

  [[nodiscard]] int kept() const;
  [[nodiscard]] QString title() const;
  [[nodiscard]] QCustomPlot* plot() const;
  // The axes as drawn. x is the sweep's whole width from a try's start: the
  // union of the tries in hand (their start and end), never grown by a point.
  // y follows every drawn value, padded 5 % each side (0..1 with none).
  [[nodiscard]] QCPRange x_range() const;
  [[nodiscard]] QCPRange y_range() const;

  // What is drawn, for tests: one entry per series, the current try's first.
  struct Series {
    QString name;          // "*H1" for the reference, the detector's name otherwise; empty for a faint try
    QVector<double> x, y;  // y as drawn (scaled for an additional detector)
    QVector<double> raw;   // y as measured: what the tooltip gives
    bool faint = false;
  };
  [[nodiscard]] std::vector<Series> series() const;
  [[nodiscard]] QVector<double> marker_x() const;  // low, centre, high, maximum; empty when none
  [[nodiscard]] QVector<double> marker_y() const;
  [[nodiscard]] QString tooltip_at(int series, int point) const;  // detector, x, raw intensity
  [[nodiscard]] QVector<double> rule_x() const;  // initial centre, fitted centre, centre without offset

 signals:
  void tryStarted();  // EvolutionsView selects the tab on it

 private:
  struct Try {
    int index = 0;
    double initial_center = 0;
    double start = 0, end = 0;  // the sweep's ends, native, in either order
    std::vector<jobs::SweepPoint> points;
  };
  // A series before it is drawn: the detector it is of, and its colour.
  struct Drawn {
    Series series;
    std::string detector;
    QColor color;
  };

  void start_fresh();
  void display(const jobs::PeakCenterResult& r);
  void redraw();
  void set_axes();  // x the tries' width, y the drawn values'
  void on_mouse_move(QMouseEvent* event);
  [[nodiscard]] std::vector<Drawn> build() const;
  [[nodiscard]] QColor color_of(const std::string& detector, std::size_t index) const;
  [[nodiscard]] QString heading() const;

  std::map<std::string, QColor> colors_;
  std::map<std::string, std::size_t> palette_slots_;
  QLabel* title_;
  QCustomPlot* plot_;

  // The peak center in hand: as the events gave it, or a kept result.
  std::string detector_;
  std::string isotope_;
  std::vector<std::string> additional_;
  int n_tries_ = 0;
  std::vector<Try> tries_;
  std::optional<jobs::PeakCenterResult> done_;
  bool abandoned_ = false;

  std::deque<jobs::PeakCenterResult> kept_;
};

}  // namespace pychron::ui
