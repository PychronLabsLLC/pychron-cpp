#include "strip_chart_view.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include <QVBoxLayout>

#include <qcustomplot.h>

namespace pychron::ui {

namespace {

constexpr double kLogFloor = 1e-6;

// What the axis can take: finite, lo < hi, and positive on a log axis.
// `shown` is what the axis has now, kept when the model's range is unusable.
AxisRange usable(AxisRange wanted, AxisRange shown, bool log) {
  AxisRange r = wanted;
  if (!std::isfinite(r.lo) || !std::isfinite(r.hi) || !(r.lo < r.hi)) {
    r = shown;
  }
  if (log) {
    if (!(r.hi > kLogFloor)) {
      r.hi = 1.0;
    }
    if (!(r.lo > 0.0) || !(r.lo < r.hi)) {
      r.lo = std::min(kLogFloor, r.hi / 10.0);
    }
  }
  return r;
}

}  // namespace

StripChartView::StripChartView(StripChartModel& model, QWidget* parent)
    : QWidget(parent), model_(model), plot_(new QCustomPlot(this)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(plot_);

  plot_->axisRect()->setBackground(QBrush(QColor(0xfa, 0xfa, 0xd2)));  // pychron's light yellow
  plot_->xAxis->setLabel(tr("Time (s)"));
  plot_->yAxis->setLabel(tr("Signal"));
  plot_->legend->setVisible(false);  // the intensities table is the legend
  // Stroking a wide antialiased polyline as one path costs far more than its
  // segments drawn one by one, and grows with every point: a few hundred
  // points per trace stalled the event loop for over a second.
  plot_->setPlottingHint(QCP::phFastPolylines, true);
  for (const auto& detector : model_.detectors()) {
    QCPGraph* graph = plot_->addGraph();
    graph->setPen(QPen(detector.color, 1.5));
    graph->setName(QString::fromStdString(detector.name));
  }

  deferred_.setSingleShot(true);
  connect(&deferred_, &QTimer::timeout, this, [this] { replot(); });
  refresh();
}

void StripChartView::apply_scale() {
  const bool log = model_.scale() == YScale::Log;
  if (log) {
    // The range goes positive before the axis turns logarithmic.
    plot_->yAxis->setRange(1.0, 10.0);
    plot_->yAxis->setScaleType(QCPAxis::stLogarithmic);
    plot_->yAxis->setTicker(QSharedPointer<QCPAxisTickerLog>::create());
  } else {
    plot_->yAxis->setScaleType(QCPAxis::stLinear);
    plot_->yAxis->setTicker(QSharedPointer<QCPAxisTicker>::create());
  }
  applied_scale_ = model_.scale();
}

void StripChartView::refresh() {
  if (model_.scale() != applied_scale_) {
    apply_scale();
  }
  const bool log = applied_scale_ == YScale::Log;
  const auto& detectors = model_.detectors();
  for (std::size_t i = 0; i < detectors.size(); ++i) {
    const TimeSeriesRing& ring = model_.series(i);
    QVector<double> x;
    QVector<double> y;
    x.reserve(static_cast<qsizetype>(ring.size()));
    y.reserve(static_cast<qsizetype>(ring.size()));
    for (std::size_t k = 0; k < ring.size(); ++k) {
      const auto& point = ring[k];
      x.push_back(point.t);
      y.push_back(log && !(point.value > 0.0) ? std::numeric_limits<double>::quiet_NaN() : point.value);
    }
    QCPGraph* graph = plot_->graph(static_cast<int>(i));
    graph->setData(x, y, true);  // the ring is already in time order
    graph->setVisible(detectors[i].visible);
  }

  const AxisRange x = usable(model_.x_range(), shown_x(), false);
  const AxisRange y = usable(model_.y_range(std::chrono::steady_clock::now()), shown_y(), log);
  plot_->xAxis->setRange(x.lo, x.hi);
  plot_->yAxis->setRange(y.lo, y.hi);

  if (!since_replot_.isValid() || since_replot_.elapsed() >= kMinReplotMs) {
    replot();
  } else if (!deferred_.isActive()) {
    deferred_.start(kMinReplotMs - static_cast<int>(since_replot_.elapsed()));
  }
}

void StripChartView::replot() {
  deferred_.stop();
  since_replot_.start();
  plot_->replot();
}

int StripChartView::graph_count() const { return plot_->graphCount(); }

int StripChartView::point_count(int graph) const {
  return graph >= 0 && graph < plot_->graphCount() ? plot_->graph(graph)->dataCount() : 0;
}

bool StripChartView::graph_visible(int graph) const {
  return graph >= 0 && graph < plot_->graphCount() && plot_->graph(graph)->visible();
}

AxisRange StripChartView::shown_x() const { return {plot_->xAxis->range().lower, plot_->xAxis->range().upper}; }

AxisRange StripChartView::shown_y() const { return {plot_->yAxis->range().lower, plot_->yAxis->range().upper}; }

}  // namespace pychron::ui
