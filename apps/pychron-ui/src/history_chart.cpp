#include "history_chart.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <qcustomplot.h>

namespace pychron::ui {

namespace {

constexpr std::chrono::seconds kDateFrom{6 * 3600};  // from this range on, the labels carry the date
constexpr int kTicks = 5;
constexpr int kCompactTicks = 3;

QString wall_format(std::chrono::seconds range) { return range >= kDateFrom ? "MM-dd hh:mm" : "hh:mm"; }

QString minutes_label(double x) { return QString::number(x / 60.0, 'g', 4); }

// Seconds on the axis, labelled in minutes, with steps that are round in minutes.
class MinutesTicker : public QCPAxisTicker {
 protected:
  double getTickStep(const QCPRange& range) override {
    return QCPAxisTicker::getTickStep(QCPRange(range.lower / 60.0, range.upper / 60.0)) * 60.0;
  }
  QString getTickLabel(double tick, const QLocale&, QChar, int) override { return minutes_label(tick); }
};

}  // namespace

HistoryChart::HistoryChart(QWidget* parent)
    : QWidget(parent), stack_(new QStackedWidget(this)), empty_(new QLabel(tr("No readings"))), plot_(new QCustomPlot) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(stack_);
  empty_->setAlignment(Qt::AlignCenter);
  stack_->addWidget(empty_);  // the stack owns both
  stack_->addWidget(plot_);

  plot_->axisRect()->setBackground(QBrush(theme().plot_bg));
  plot_->legend->setVisible(false);
  plot_->setPlottingHint(QCP::phFastPolylines, true);  // as StripChartView: a day of points is many
  QCPGraph* graph = plot_->addGraph();
  graph->setPen(QPen(theme().series[0], 1.5));
  redraw();
}

void HistoryChart::set_series(const ReadingHistory::Series& readings, std::chrono::seconds range,
                              const QDateTime& newest_wall) {
  range_ = range;
  newest_wall_ = newest_wall;
  units_ = readings.units;
  const auto oldest = -static_cast<double>(range.count());
  points_.clear();
  std::copy_if(readings.points.begin(), readings.points.end(), std::back_inserter(points_),
               [oldest](const TimeSeriesRing::Point& p) { return p.t >= oldest; });
  redraw();
}

void HistoryChart::set_log(bool log) {
  if (log == log_) return;
  log_ = log;
  redraw();
}

void HistoryChart::set_guides(std::vector<double> y) {
  guides_ = std::move(y);
  redraw();
}

void HistoryChart::set_compact(bool compact) {
  if (compact == compact_) return;
  compact_ = compact;
  redraw();
}

std::optional<HistoryChart::Summary> HistoryChart::summary() const {
  std::optional<Summary> s;
  for (const auto& p : points_) {
    if (!std::isfinite(p.value)) continue;
    if (!s) s = Summary{0, p.value, p.value, p.value};
    ++s->points;
    s->latest = p.value;
    s->lowest = std::min(s->lowest, p.value);
    s->highest = std::max(s->highest, p.value);
  }
  return s;
}

int HistoryChart::point_count() const {
  const auto drawn = plot_->graph(0)->data();
  return static_cast<int>(
      std::count_if(drawn->constBegin(), drawn->constEnd(), [](const QCPGraphData& d) { return !std::isnan(d.value); }));
}

AxisRange HistoryChart::shown_x() const { return {plot_->xAxis->range().lower, plot_->xAxis->range().upper}; }

AxisRange HistoryChart::shown_y() const { return {plot_->yAxis->range().lower, plot_->yAxis->range().upper}; }

QString HistoryChart::x_label_at(double x) const {
  if (!wall_axis()) return minutes_label(x);
  return QCPAxisTickerDateTime::keyToDateTime(x).toLocalTime().toString(wall_format(range_));
}

double HistoryChart::x_of(double t) const {
  if (!wall_axis()) return t;
  return static_cast<double>(newest_wall_.toMSecsSinceEpoch()) / 1000.0 + t;
}

void HistoryChart::apply_scale() {
  if (log_) {
    plot_->yAxis->setScaleType(QCPAxis::stLogarithmic);
    plot_->yAxis->setTicker(QSharedPointer<QCPAxisTickerLog>::create());
  } else {
    plot_->yAxis->setScaleType(QCPAxis::stLinear);
    plot_->yAxis->setTicker(QSharedPointer<QCPAxisTicker>::create());
  }
  plot_->yAxis->ticker()->setTickCount(compact_ ? kCompactTicks : kTicks);
}

void HistoryChart::apply_axes() {
  // x: the range, ending at the newest reading.
  plot_->xAxis->setRange(x_of(-static_cast<double>(range_.count())), x_of(0.0));
  if (wall_axis()) {
    auto ticker = QSharedPointer<QCPAxisTickerDateTime>::create();
    ticker->setDateTimeSpec(Qt::LocalTime);
    ticker->setDateTimeFormat(wall_format(range_));
    plot_->xAxis->setTicker(ticker);
  } else {
    plot_->xAxis->setTicker(QSharedPointer<MinutesTicker>::create());
  }
  plot_->xAxis->ticker()->setTickCount(compact_ ? kCompactTicks : kTicks);
  plot_->xAxis->setLabel(compact_ ? QString() : wall_axis() ? tr("Time") : tr("Minutes before the newest reading"));
  plot_->yAxis->setLabel(compact_ ? QString() : units_);

  // y: what is drawn, with room. On a log axis the room is a factor and the
  // range is never non-positive; there is no floor, a pressure may be 1e-10.
  double lo = std::numeric_limits<double>::infinity();
  double hi = -lo;
  for (const auto& p : points_) {
    if (!std::isfinite(p.value) || (log_ && !(p.value > 0.0))) continue;
    lo = std::min(lo, p.value);
    hi = std::max(hi, p.value);
  }
  AxisRange y{0.0, 1.0};
  if (lo <= hi && log_) {
    const double factor = lo < hi ? std::max(std::pow(hi / lo, 0.05), 1.2) : 2.0;
    y = {lo / factor, hi * factor};
  } else if (lo <= hi) {
    const double pad = lo < hi ? (hi - lo) * 0.05 : (lo != 0.0 ? std::abs(lo) * 0.1 : 1.0);
    y = {lo - pad, hi + pad};
  } else if (log_) {
    y = {1.0, 10.0};
  }
  // The scale first: a log axis cuts any range it is given to fit itself, so a
  // linear range set while it is still logarithmic would lose its lower end.
  apply_scale();
  plot_->yAxis->setRange(y.lo, y.hi);
}

void HistoryChart::redraw() {
  QVector<double> x;
  QVector<double> y;
  x.reserve(static_cast<qsizetype>(points_.size()));
  y.reserve(static_cast<qsizetype>(points_.size()));
  for (const auto& p : points_) {
    x.push_back(x_of(p.t));
    const bool shown = std::isfinite(p.value) && (!log_ || p.value > 0.0);
    y.push_back(shown ? p.value : std::numeric_limits<double>::quiet_NaN());
  }
  plot_->graph(0)->setData(x, y, true);  // already in time order
  apply_axes();

  for (QCPItemStraightLine* line : guide_lines_) plot_->removeItem(line);
  guide_lines_.clear();
  const AxisRange shown = shown_y();
  for (const double g : guides_) {
    if (!(g >= shown.lo && g <= shown.hi)) continue;
    auto* line = new QCPItemStraightLine(plot_);  // the plot owns its items
    for (QCPItemPosition* end : {line->point1, line->point2}) {
      end->setTypeX(QCPItemPosition::ptAxisRectRatio);
      end->setTypeY(QCPItemPosition::ptPlotCoords);
    }
    line->point1->setCoords(0.0, g);
    line->point2->setCoords(1.0, g);
    line->setPen(QPen(theme().error, 1.0, Qt::DashLine));
    guide_lines_.push_back(line);
  }

  const bool any = summary().has_value();
  stack_->setCurrentWidget(any ? static_cast<QWidget*>(plot_) : empty_);
  if (any) plot_->replot();  // behind the empty label there is nothing to draw
}

}  // namespace pychron::ui
