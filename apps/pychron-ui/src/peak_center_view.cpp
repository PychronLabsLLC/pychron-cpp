#include "peak_center_view.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <QLabel>
#include <QMouseEvent>
#include <QToolTip>
#include <QVBoxLayout>

#include <qcustomplot.h>

#include "peak_center_scale.hpp"
#include "strip_chart_model.hpp"
#include "plot_style.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

constexpr double kReferenceWidth = 2.5;
constexpr double kAdditionalWidth = 1.5;
constexpr double kFaintWidth = 1.0;
constexpr int kFaintAlpha = 70;
constexpr double kTooltipPixels = 12.0;
constexpr double kPadding = 0.05;      // of the values' span, each side
constexpr double kDefaultLower = 0.0;  // an axis with nothing to span
constexpr double kDefaultUpper = 1.0;

// One detector's points in a try: the steps that have a value for it.
struct Column {
  QVector<double> x, y;
};

Column column(const std::vector<jobs::SweepPoint>& points, const std::string& detector) {
  Column c;
  for (const auto& p : points) {
    auto it = p.y.find(detector);
    if (it == p.y.end()) continue;
    c.x.push_back(p.x);
    c.y.push_back(it->second);
  }
  return c;
}

QString outcome(const jobs::PeakCenterResult& r) {
  if (r.ok && r.center) {
    QString text = PeakCenterView::tr("center %1").arg(*r.center, 0, 'f', 6);
    if (r.table_updated) text += PeakCenterView::tr(", table updated");
    return text;
  }
  return PeakCenterView::tr("failed: %1").arg(QString::fromStdString(r.message));
}

}  // namespace

PeakCenterView::PeakCenterView(std::map<std::string, QColor> colors,
                               std::map<std::string, std::size_t> palette_slots, QWidget* parent)
    : QWidget(parent),
      colors_(std::move(colors)),
      palette_slots_(std::move(palette_slots)),
      title_(new QLabel),
      plot_(new QCustomPlot) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(title_);
  layout->addWidget(plot_, 1);

  plot_->setMinimumHeight(200);
  plot_->axisRect()->setBackground(QBrush(plot_style().plot_background));
  plot_->xAxis->setLabel(tr("Magnet position"));
  plot_->yAxis->setLabel(tr("Intensity"));
  plot_->legend->setVisible(true);
  plot_->axisRect()->insetLayout()->setInsetAlignment(0, Qt::AlignTop | Qt::AlignLeft);
  plot_->setPlottingHint(QCP::phFastPolylines, true);
  connect(plot_, &QCustomPlot::mouseMove, this, &PeakCenterView::on_mouse_move);
  connect(&PlotStyleHub::instance(), &PlotStyleHub::changed, this, [this] {
    plot_->axisRect()->setBackground(QBrush(plot_style().plot_background));
    redraw();  // the pens, from plot_style()
  });
  title_->setText(tr("No peak center"));
  set_axes();  // the defaults, not QCustomPlot's
}

void PeakCenterView::start_fresh() {
  tries_.clear();
  done_.reset();
  abandoned_ = false;
}

void PeakCenterView::on_try_started(const jobs::PeakCenterTryStarted& e) {
  // A new peak center, or the one in hand was done (or was a kept result).
  if (e.try_index == 0 || done_ || abandoned_) start_fresh();
  detector_ = e.detector;
  isotope_ = e.isotope;
  additional_ = e.additional;
  n_tries_ = e.n_tries;
  tries_.push_back(Try{e.try_index, e.initial_center, e.start, e.end, {}});
  redraw();
  emit tryStarted();
}

void PeakCenterView::on_point(const jobs::PeakCenterPoint& e) {
  if (done_ || tries_.empty()) return;
  auto it = std::ranges::find(tries_, e.try_index, &Try::index);
  if (it == tries_.end()) return;
  it->points.push_back(e.point);
  redraw();
}

void PeakCenterView::on_done(const jobs::PeakCenterDone& e) {
  kept_.push_back(e.result);
  while (kept_.size() > kKept) kept_.pop_front();
  display(e.result);
}

void PeakCenterView::show_result(int index) {
  if (index < 0 || std::cmp_greater_equal(index, kept_.size())) return;
  display(kept_[static_cast<std::size_t>(index)]);
}

void PeakCenterView::display(const jobs::PeakCenterResult& r) {
  start_fresh();
  detector_ = r.detector;
  isotope_ = r.isotope;
  additional_ = r.config.additional_detectors;
  n_tries_ = r.config.n_tries;
  for (std::size_t i = 0; i < r.tries.size(); ++i)
    tries_.push_back(Try{static_cast<int>(i), r.initial_center, r.tries[i].start, r.tries[i].end, r.tries[i].points});
  done_ = r;
  redraw();
}

void PeakCenterView::clear() {
  start_fresh();
  kept_.clear();
  detector_.clear();
  isotope_.clear();
  additional_.clear();
  n_tries_ = 0;
  redraw();
  title_->setText(tr("No peak center"));
}

void PeakCenterView::abandon() {
  if (tries_.empty() || done_) return;
  abandoned_ = true;
  redraw();
}

int PeakCenterView::kept() const { return static_cast<int>(kept_.size()); }

QString PeakCenterView::title() const { return title_->text(); }

QCustomPlot* PeakCenterView::plot() const { return plot_; }

QCPRange PeakCenterView::x_range() const { return plot_->xAxis->range(); }

QCPRange PeakCenterView::y_range() const { return plot_->yAxis->range(); }

QColor PeakCenterView::color_of(const std::string& detector, std::size_t index) const {
  auto it = colors_.find(detector);
  if (it != colors_.end() && it->second.isValid()) return it->second;
  auto slot = palette_slots_.find(detector);
  return StripChartModel::palette_color(slot != palette_slots_.end() ? slot->second : index);
}

std::vector<PeakCenterView::Drawn> PeakCenterView::build() const {
  std::vector<Drawn> out;
  // The current try first, then the earlier ones, newest first.
  for (auto t = tries_.rbegin(); t != tries_.rend(); ++t) {
    const bool faint = t != tries_.rbegin();
    const Column reference = column(t->points, detector_);
    {
      Drawn d{{faint ? QString() : QStringLiteral("*%1").arg(QString::fromStdString(detector_)), reference.x,
               reference.y, reference.y, faint},
              detector_,
              color_of(detector_, 0)};
      out.push_back(std::move(d));
    }
    for (std::size_t i = 0; i < additional_.size(); ++i) {
      const std::string& detector = additional_[i];
      if (detector == detector_) continue;
      const Column c = column(t->points, detector);
      const auto scaled = scale_to_reference(std::span<const double>(reference.y.data(), reference.y.size()),
                                             std::span<const double>(c.y.data(), c.y.size()));
      QVector<double> y = c.y;
      if (scaled) y = QVector<double>(scaled->begin(), scaled->end());
      out.push_back(Drawn{{faint ? QString() : QString::fromStdString(detector), c.x, std::move(y), c.y, faint},
                          detector,
                          color_of(detector, i + 1)});
    }
  }
  return out;
}

std::vector<PeakCenterView::Series> PeakCenterView::series() const {
  std::vector<Series> out;
  for (auto& d : build()) out.push_back(std::move(d.series));
  return out;
}

QVector<double> PeakCenterView::marker_x() const {
  if (!done_ || !done_->ok || !done_->shape) return {};
  const auto& s = *done_->shape;
  return {s.low_x, s.center_x, s.high_x, s.max_x};
}

QVector<double> PeakCenterView::marker_y() const {
  if (!done_ || !done_->ok || !done_->shape) return {};
  const auto& s = *done_->shape;
  return {s.low_y, s.center_y, s.high_y, s.max_y};
}

QVector<double> PeakCenterView::rule_x() const {
  if (!done_ && tries_.empty()) return {};
  QVector<double> out{done_ ? done_->initial_center : tries_.back().initial_center};
  if (done_ && done_->ok && done_->center) {
    out.push_back(*done_->center);
    if (done_->config.dac_offset != 0) out.push_back(*done_->center - done_->config.dac_offset);
  }
  return out;
}

QString PeakCenterView::tooltip_at(int series, int point) const {
  const auto drawn = build();
  if (series < 0 || std::cmp_greater_equal(series, drawn.size())) return {};
  const auto& d = drawn[static_cast<std::size_t>(series)];
  if (point < 0 || point >= d.series.x.size()) return {};
  return tr("%1\nx %2\nintensity %3")
      .arg(QString::fromStdString(d.detector))
      .arg(d.series.x[point], 0, 'f', 5)
      .arg(d.series.raw[point], 0, 'g', 6);
}

QString PeakCenterView::heading() const {
  const QString who = tr("%1 on %2").arg(QString::fromStdString(isotope_), QString::fromStdString(detector_));
  if (done_) return QStringLiteral("%1: %2").arg(who, outcome(*done_));
  if (tries_.empty()) return tr("No peak center");
  QString text = tr("%1, try %2 of %3").arg(who).arg(tries_.back().index + 1).arg(n_tries_);
  if (abandoned_) text += tr(": not finished");
  return text;
}

void PeakCenterView::redraw() {
  plot_->clearGraphs();
  plot_->clearItems();
  for (const auto& d : build()) {
    auto* g = plot_->addGraph();
    QColor color = d.color;
    double width = d.detector == detector_ ? kReferenceWidth : kAdditionalWidth;
    if (d.series.faint) {
      color.setAlpha(kFaintAlpha);
      width = kFaintWidth;
    }
    g->setPen(QPen(color, width));
    g->setName(d.series.name);
    g->setAdaptiveSampling(false);
    g->setData(d.series.x, d.series.y, true);
    if (d.series.faint) g->removeFromLegend();
  }

  const QVector<double> mx = marker_x();
  if (!mx.isEmpty()) {
    const QVector<double> my = marker_y();
    auto* edges = plot_->addGraph();
    edges->setLineStyle(QCPGraph::lsNone);
    edges->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCircle, theme().accent, theme().accent, 8));
    edges->setData(mx.mid(0, 3), my.mid(0, 3), true);
    edges->removeFromLegend();
    auto* top = plot_->addGraph();
    top->setLineStyle(QCPGraph::lsNone);
    top->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDiamond, theme().error, theme().error, 9));
    top->setData(mx.mid(3, 1), my.mid(3, 1), true);
    top->removeFromLegend();
  }

  for (const double x : rule_x()) {
    auto* rule = new QCPItemStraightLine(plot_);  // owned by the plot
    rule->point1->setCoords(x, 0);
    rule->point2->setCoords(x, 1);
    rule->setPen(QPen(theme().muted_text, 1, Qt::DashLine));
  }

  set_axes();
  plot_->replot();
  title_->setText(heading());
}

void PeakCenterView::set_axes() {
  // x: the whole sweep from its start, so the trace grows across a fixed
  // axis. A try whose ends are unknown (both 0) counts for nothing.
  double lo = std::numeric_limits<double>::infinity();
  double hi = -std::numeric_limits<double>::infinity();
  for (const auto& t : tries_) {
    if (t.start == t.end) continue;
    lo = std::min({lo, t.start, t.end});
    hi = std::max({hi, t.start, t.end});
  }
  if (lo < hi) {
    plot_->xAxis->setRange(lo, hi);
  } else {
    // No sweep known (none yet, cleared, a result without a try): around
    // what is drawn and the rules, else the default.
    for (int i = 0; i < plot_->graphCount(); ++i) {
      bool found = false;
      const QCPRange r = plot_->graph(i)->getKeyRange(found);
      if (!found) continue;
      lo = std::min(lo, r.lower);
      hi = std::max(hi, r.upper);
    }
    for (const double x : rule_x()) {
      lo = std::min(lo, x);
      hi = std::max(hi, x);
    }
    if (!std::isfinite(lo) || !std::isfinite(hi)) {
      plot_->xAxis->setRange(kDefaultLower, kDefaultUpper);
    } else {
      const double pad = hi > lo ? (hi - lo) * kPadding : kDefaultUpper * kPadding;
      plot_->xAxis->setRange(lo - pad, hi + pad);
    }
  }

  // y: every drawn value (markers too), padded.
  double ylo = std::numeric_limits<double>::infinity();
  double yhi = -std::numeric_limits<double>::infinity();
  for (int i = 0; i < plot_->graphCount(); ++i) {
    bool found = false;
    const QCPRange r = plot_->graph(i)->getValueRange(found);
    if (!found) continue;
    ylo = std::min(ylo, r.lower);
    yhi = std::max(yhi, r.upper);
  }
  if (!std::isfinite(ylo) || !std::isfinite(yhi)) {
    plot_->yAxis->setRange(kDefaultLower, kDefaultUpper);
    return;
  }
  double pad = (yhi - ylo) * kPadding;
  if (pad <= 0) pad = std::abs(yhi) > 0 ? std::abs(yhi) * kPadding : kDefaultUpper * kPadding;
  plot_->yAxis->setRange(ylo - pad, yhi + pad);
}

void PeakCenterView::on_mouse_move(QMouseEvent* event) {
  const auto drawn = build();
  const QPointF at = event->position();
  double best = kTooltipPixels;
  int best_series = -1;
  int best_point = -1;
  for (std::size_t s = 0; s < drawn.size(); ++s) {
    const auto& d = drawn[s].series;
    if (d.faint) continue;
    for (int i = 0; i < d.x.size(); ++i) {
      const double px = plot_->xAxis->coordToPixel(d.x[i]);
      const double py = plot_->yAxis->coordToPixel(d.y[i]);
      const double dist = std::hypot(px - at.x(), py - at.y());
      if (dist < best) {
        best = dist;
        best_series = static_cast<int>(s);
        best_point = i;
      }
    }
  }
  if (best_series < 0) {
    QToolTip::hideText();
    return;
  }
  QToolTip::showText(event->globalPosition().toPoint(), tooltip_at(best_series, best_point), plot_);
}

}  // namespace pychron::ui
