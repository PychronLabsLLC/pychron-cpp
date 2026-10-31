#include "follow_plot_view.hpp"

#include <algorithm>
#include <cmath>

#include <QVBoxLayout>

#include <qcustomplot.h>

#include "plot_style.hpp"

namespace pychron::ui {

namespace {

struct StripSpec {
  const char* title;
  const char* first;
  const char* second;
};

// The display names of spec 4.1; stage / grain on the first two, seen / true on
// the others.
const std::array<StripSpec, FollowPlotView::kStrips>& strips() {
  static const std::array<StripSpec, FollowPlotView::kStrips> s = {{
      {QT_TR_NOOP("Stage X (mm)"), QT_TR_NOOP("stage"), QT_TR_NOOP("grain")},
      {QT_TR_NOOP("Stage Y (mm)"), QT_TR_NOOP("stage"), QT_TR_NOOP("grain")},
      {QT_TR_NOOP("Beam deviation (mm)"), QT_TR_NOOP("seen"), QT_TR_NOOP("true")},
      {QT_TR_NOOP("Luminosity"), QT_TR_NOOP("seen"), QT_TR_NOOP("true")},
      {QT_TR_NOOP("Grain size (mm)"), QT_TR_NOOP("seen"), QT_TR_NOOP("true")},
  }};
  return s;
}

constexpr std::size_t line_index(FollowLine line) { return static_cast<std::size_t>(line); }

bool usable(double lo, double hi) { return std::isfinite(lo) && std::isfinite(hi) && lo < hi; }

}  // namespace

FollowPlotView::FollowPlotView(FollowPlotModel& model, QWidget* parent)
    : QWidget(parent, Qt::Window), model_(model), plot_(new QCustomPlot(this)) {
  setWindowTitle(tr("Follow plots"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(plot_);
  resize(720, 760);

  plot_->setPlottingHint(QCP::phFastPolylines, true);
  plot_->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

  auto* margins = new QCPMarginGroup(plot_);
  for (int i = 0; i < kStrips; ++i) {
    QCPAxisRect* rect = i == 0 ? plot_->axisRect() : new QCPAxisRect(plot_);
    if (i > 0) plot_->plotLayout()->addElement(i, 0, rect);
    rect->setMarginGroup(QCP::msLeft | QCP::msRight, margins);
    rect->setRangeDrag(Qt::Horizontal);
    rect->setRangeZoom(Qt::Horizontal);
    rect->setRangeDragAxes(rect->axis(QCPAxis::atBottom), nullptr);
    rect->setRangeZoomAxes(rect->axis(QCPAxis::atBottom), nullptr);
    rect->axis(QCPAxis::atLeft)->setLabel(tr(strips()[static_cast<std::size_t>(i)].title));
    const bool last = i == kStrips - 1;
    rect->axis(QCPAxis::atBottom)->setTickLabels(last);
    if (last) rect->axis(QCPAxis::atBottom)->setLabel(tr("Time (s)"));
    rect->axis(QCPAxis::atBottom)->setRange(0, 1);
    rect->axis(QCPAxis::atLeft)->setRange(0, 1);
    rects_[static_cast<std::size_t>(i)] = rect;

    // The plot's own legend sits in the first rect already; the rest get one.
    QCPLegend* legend = plot_->legend;
    if (i > 0) {
      legend = new QCPLegend;
      rect->insetLayout()->addElement(legend, Qt::AlignTop | Qt::AlignRight);
      legend->setLayer(QStringLiteral("legend"));
    }
    legend->setVisible(true);
    legends_[static_cast<std::size_t>(i)] = legend;

    connect(rect->axis(QCPAxis::atBottom), qOverload<const QCPRange&>(&QCPAxis::rangeChanged), this,
            [this, rect](const QCPRange& r) {
              if (setting_x_) return;
              auto_x_ = false;  // the user moved it
              setting_x_ = true;
              for (QCPAxisRect* other : rects_)
                if (other != rect) other->axis(QCPAxis::atBottom)->setRange(r);
              setting_x_ = false;
            });
  }

  for (std::size_t k = 0; k < kFollowLines; ++k) {
    QCPAxisRect* rect = rects_[k / 2];
    QCPGraph* graph = plot_->addGraph(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
    graph->removeFromLegend();  // the plot's legend; each strip's lists only what it shows
    const StripSpec& spec = strips()[k / 2];
    graph->setName(tr(k % 2 == 0 ? spec.first : spec.second));
    graphs_[k] = graph;
  }

  deferred_.setSingleShot(true);
  connect(&deferred_, &QTimer::timeout, this, [this] { replot(); });
  connect(&PlotStyleHub::instance(), &PlotStyleHub::changed, this, [this] { restyle(); });
  restyle();
  refresh();
}

void FollowPlotView::restyle() {
  for (QCPAxisRect* rect : rects_) rect->setBackground(QBrush(plot_style().plot_background));
  for (std::size_t k = 0; k < kFollowLines; ++k) graphs_[k]->setPen(QPen(plot_series_color(k % 2), 1.5));
  replot();
}

void FollowPlotView::set_all_x(double lo, double hi) {
  setting_x_ = true;
  for (QCPAxisRect* rect : rects_) rect->axis(QCPAxis::atBottom)->setRange(lo, hi);
  setting_x_ = false;
}

void FollowPlotView::rebuild_legends() {
  unsigned mask = 0;
  for (std::size_t k = 0; k < kFollowLines; ++k)
    if (model_.has(static_cast<FollowLine>(k))) mask |= 1u << k;
  if (mask == shown_mask_) return;
  shown_mask_ = mask;
  for (std::size_t s = 0; s < legends_.size(); ++s) {
    legends_[s]->clearItems();
    for (std::size_t k = 2 * s; k < 2 * s + 2; ++k)
      if ((mask & (1u << k)) != 0) graphs_[k]->addToLegend(legends_[s]);
  }
}

void FollowPlotView::refresh() {
  for (std::size_t k = 0; k < kFollowLines; ++k) {
    const auto line = static_cast<FollowLine>(k);
    const TimeSeriesRing& ring = model_.series(line);
    QVector<double> x;
    QVector<double> y;
    x.reserve(static_cast<qsizetype>(ring.size()));
    y.reserve(static_cast<qsizetype>(ring.size()));
    for (std::size_t i = 0; i < ring.size(); ++i) {
      x.push_back(ring[i].t);
      y.push_back(ring[i].value);
    }
    graphs_[k]->setData(x, y, true);  // the ring is in time order
    graphs_[k]->setVisible(model_.has(line));
  }
  rebuild_legends();

  // A new run (the model began again) takes the x range back.
  const double latest = model_.latest_x();
  if (latest < last_x_) auto_x_ = true;
  last_x_ = latest;
  const double hi = std::max(latest, 1.0);
  const double lo = std::max(0.0, hi - FollowPlotModel::kSpanSeconds);
  if (auto_x_ && usable(lo, hi)) set_all_x(lo, hi);

  const QCPRange shown = rects_[0]->axis(QCPAxis::atBottom)->range();
  for (std::size_t s = 0; s < rects_.size(); ++s) {
    double lo_y = 0;
    double hi_y = 0;
    bool any = false;
    for (std::size_t k = 2 * s; k < 2 * s + 2; ++k) {
      if (!model_.has(static_cast<FollowLine>(k))) continue;
      if (const auto r = model_.series(static_cast<FollowLine>(k)).range(shown.lower, shown.upper)) {
        lo_y = any ? std::min(lo_y, r->first) : r->first;
        hi_y = any ? std::max(hi_y, r->second) : r->second;
        any = true;
      }
    }
    if (!any || !std::isfinite(lo_y) || !std::isfinite(hi_y)) continue;  // keep what is shown
    const double pad = hi_y > lo_y ? (hi_y - lo_y) * 0.08 : std::max(std::abs(lo_y) * 0.05, 0.5);
    if (usable(lo_y - pad, hi_y + pad)) rects_[s]->axis(QCPAxis::atLeft)->setRange(lo_y - pad, hi_y + pad);
  }

  if (!since_replot_.isValid() || since_replot_.elapsed() >= kMinReplotMs) {
    replot();
  } else if (!deferred_.isActive()) {
    deferred_.start(kMinReplotMs - static_cast<int>(since_replot_.elapsed()));
  }
}

void FollowPlotView::replot() {
  deferred_.stop();
  since_replot_.start();
  plot_->replot();
}

int FollowPlotView::point_count(FollowLine line) const { return graphs_[line_index(line)]->dataCount(); }

bool FollowPlotView::line_visible(FollowLine line) const { return graphs_[line_index(line)]->visible(); }

}  // namespace pychron::ui
