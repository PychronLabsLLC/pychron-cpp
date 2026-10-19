#include "field_table_plot.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QMouseEvent>
#include <QToolTip>
#include <QVBoxLayout>

#include <qcustomplot.h>

#include "plot_style.hpp"

namespace pychron::ui {

namespace sp = pychron::spectrometer;

namespace {

constexpr int kCurveGraph = 0;
constexpr int kPointGraph = 1;
constexpr double kHoverPixels = 8.0;

}  // namespace

FieldTablePlot::FieldTablePlot(QWidget* parent) : QWidget(parent), plot_(new QCustomPlot) {
  plot_->setObjectName(QStringLiteral("plot"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(plot_);
  plot_->setMinimumHeight(160);
  plot_->xAxis->setLabel(tr("Mass (amu)"));
  plot_->addGraph();  // kCurveGraph
  plot_->addGraph();  // kPointGraph
  plot_->graph(kPointGraph)->setLineStyle(QCPGraph::lsNone);
  setMouseTracking(true);
  connect(plot_, &QCustomPlot::mouseMove, this, &FieldTablePlot::on_mouse_move);
  connect(&PlotStyleHub::instance(), &PlotStyleHub::changed, this, [this] { redraw(); });
  redraw();
}

void FieldTablePlot::show_detector(const sp::FieldTable& table, const std::string& detector) {
  table_ = table;
  detector_ = detector;
  redraw();
}

QColor FieldTablePlot::point_color() const { return plot_->graph(kPointGraph)->scatterStyle().pen().color(); }

QColor FieldTablePlot::curve_color() const { return plot_->graph(kCurveGraph)->pen().color(); }

void FieldTablePlot::redraw() {
  const QColor color = plot_series_color(0);
  plot_->axisRect()->setBackground(QBrush(plot_style().plot_background));
  auto* curve = plot_->graph(kCurveGraph);
  auto* points = plot_->graph(kPointGraph);
  curve->setPen(QPen(color, 1.5));
  points->setPen(QPen(color));
  points->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCircle, color, color, 7));

  masses_.clear();
  values_.clear();
  tips_.clear();
  curve_samples_ = 0;
  curve_range_ = {0, 0};

  const bool fits = !detector_.empty() && table_.fit(detector_) != sp::FitKind::Discrete &&
                    table_.check_fit(detector_).has_value();
  for (const auto& p : table_.points()) {
    const auto it = p.values.find(detector_);
    if (it == p.values.end()) continue;
    masses_.push_back(p.mass);
    values_.push_back(it->second);
    QString tip = tr("%1: value %2").arg(QString::fromStdString(p.isotope)).arg(it->second, 0, 'f', 5);
    if (fits) {
      if (const auto fit = table_.value_for(p.mass, detector_)) {
        tip += tr(", residual %1").arg(it->second - *fit, 0, 'f', 5);
      }
    }
    tips_.append(tip);
  }
  points->setData(QVector<double>(masses_.begin(), masses_.end()), QVector<double>(values_.begin(), values_.end()),
                  false);

  QVector<double> cx, cy;
  if (fits && !masses_.empty()) {
    const auto [lo_it, hi_it] = std::ranges::minmax_element(masses_);
    const double lo = *lo_it - sp::FieldTable::kInverseMargin;
    const double hi = *hi_it + sp::FieldTable::kInverseMargin;
    cx.reserve(kCurveSamples);
    cy.reserve(kCurveSamples);
    for (int i = 0; i < kCurveSamples; ++i) {
      const double m = lo + (hi - lo) * i / (kCurveSamples - 1);
      const auto v = table_.value_for(m, detector_);
      if (!v) continue;
      cx.append(m);
      cy.append(*v);
    }
    if (!cx.isEmpty()) {
      curve_samples_ = static_cast<int>(cx.size());
      curve_range_ = {lo, hi};
    }
  }
  curve->setData(cx, cy, true);

  const QString axis = QString::fromUtf8(to_string(table_.axis()).data(),
                                         static_cast<qsizetype>(to_string(table_.axis()).size()));
  plot_->yAxis->setLabel(detector_.empty() ? QString() : tr("%1 (%2)").arg(QString::fromStdString(detector_), axis));
  if (!masses_.empty() || !cx.isEmpty()) {
    plot_->rescaleAxes();
    const QCPRange x = plot_->xAxis->range();
    const QCPRange y = plot_->yAxis->range();
    const double xpad = std::max(x.size() * 0.05, 0.5);
    const double ypad = std::max(y.size() * 0.05, 1e-3);
    plot_->xAxis->setRange(x.lower - xpad, x.upper + xpad);
    plot_->yAxis->setRange(y.lower - ypad, y.upper + ypad);
  }
  plot_->replot(QCustomPlot::rpQueuedReplot);
}

void FieldTablePlot::on_mouse_move(QMouseEvent* event) {
  const QPointF at = event->position();
  double best = std::numeric_limits<double>::max();
  int hit = -1;
  for (std::size_t i = 0; i < masses_.size(); ++i) {
    const double px = plot_->xAxis->coordToPixel(masses_[i]);
    const double py = plot_->yAxis->coordToPixel(values_[i]);
    const double d = std::hypot(px - at.x(), py - at.y());
    if (d < best) {
      best = d;
      hit = static_cast<int>(i);
    }
  }
  if (hit >= 0 && best <= kHoverPixels) {
    QToolTip::showText(event->globalPosition().toPoint(), tips_[hit], plot_);
  } else {
    QToolTip::hideText();
  }
}

}  // namespace pychron::ui
