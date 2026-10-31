#include "follow_plot_view.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <QHBoxLayout>

#include <qcustomplot.h>

#include "plot_style.hpp"

namespace pychron::ui {

namespace {

struct StripSpec {
  const char* title;
  const char* first;
  const char* second;
};

// The first line a strip shows; the lines before it are the plane's.
constexpr std::size_t kFirstStripLine = static_cast<std::size_t>(FollowLine::DeviationSeen);

// The display names of spec 4.1, seen / true on every strip.
const std::array<StripSpec, FollowPlotView::kStrips>& strips() {
  static const std::array<StripSpec, FollowPlotView::kStrips> s = {{
      {QT_TR_NOOP("Beam deviation (mm)"), QT_TR_NOOP("seen"), QT_TR_NOOP("true")},
      {QT_TR_NOOP("Luminosity"), QT_TR_NOOP("seen"), QT_TR_NOOP("true")},
      {QT_TR_NOOP("Grain size (diameter, mm)"), QT_TR_NOOP("seen"), QT_TR_NOOP("true")},
  }};
  return s;
}

constexpr std::size_t line_index(FollowLine line) { return static_cast<std::size_t>(line); }

bool usable(double lo, double hi) { return std::isfinite(lo) && std::isfinite(hi) && lo < hi; }

// The newest point of a track, and everything it has; a row with a coordinate
// missing is left out.
struct Track {
  QVector<double> t, x, y;
};

Track track_of(const TimeSeriesRing& xs, const TimeSeriesRing& ys) {
  Track out;
  const std::size_t n = std::min(xs.size(), ys.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (!std::isfinite(xs[i].value) || !std::isfinite(ys[i].value)) continue;
    out.t.push_back(xs[i].t);
    out.x.push_back(xs[i].value);
    out.y.push_back(ys[i].value);
  }
  return out;
}

}  // namespace

FollowPlotView::FollowPlotView(FollowPlotModel& model, QWidget* parent)
    : QWidget(parent, Qt::Window), model_(model), xy_(new QCustomPlot(this)), plot_(new QCustomPlot(this)) {
  setWindowTitle(tr("Follow plots"));
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(xy_, 1);
  layout->addWidget(plot_, 1);
  resize(980, 620);

  xy_->setPlottingHint(QCP::phFastPolylines, true);
  xy_->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
  xy_->xAxis->setLabel(tr("Stage X (mm)"));
  xy_->yAxis->setLabel(tr("Stage Y (mm)"));
  stage_track_ = new QCPCurve(xy_->xAxis, xy_->yAxis);
  stage_track_->setName(tr("stage"));
  grain_track_ = new QCPCurve(xy_->xAxis, xy_->yAxis);
  grain_track_->setName(tr("grain"));
  fence_ = new QCPCurve(xy_->xAxis, xy_->yAxis);
  fence_->setName(tr("fence"));
  stage_now_ = xy_->addGraph();
  grain_now_ = xy_->addGraph();
  for (QCPGraph* now : {stage_now_, grain_now_}) {
    now->setLineStyle(QCPGraph::lsNone);
    now->removeFromLegend();
  }
  xy_->legend->setVisible(true);
  for (QCPAxis* axis : {xy_->xAxis, xy_->yAxis}) {
    connect(axis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged), this, [this] {
      if (!setting_plane_) auto_plane_ = false;  // the user moved it
    });
  }
  connect(xy_, &QCustomPlot::afterLayout, this, [this] { fit_plane(); });

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

  for (std::size_t k = 0; k < graphs_.size(); ++k) {
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
  for (std::size_t k = 0; k < graphs_.size(); ++k) graphs_[k]->setPen(QPen(plot_series_color(k % 2), 1.5));
  xy_->axisRect()->setBackground(QBrush(plot_style().plot_background));
  const QColor stage = plot_series_color(0);
  const QColor grain = plot_series_color(1);
  stage_track_->setPen(QPen(stage, 1.5));
  grain_track_->setPen(QPen(grain, 1.5));
  fence_->setPen(QPen(plot_series_color(2), 1.5, Qt::DashLine));
  stage_now_->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, stage, stage, 8));
  grain_now_->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, grain, grain, 8));
  replot();
}

void FollowPlotView::set_all_x(double lo, double hi) {
  setting_x_ = true;
  for (QCPAxisRect* rect : rects_) rect->axis(QCPAxis::atBottom)->setRange(lo, hi);
  setting_x_ = false;
}

void FollowPlotView::rebuild_legends() {
  unsigned mask = 0;
  for (std::size_t k = 0; k < graphs_.size(); ++k)
    if (model_.has(static_cast<FollowLine>(k + kFirstStripLine))) mask |= 1u << k;
  if (mask == shown_mask_) return;
  shown_mask_ = mask;
  for (std::size_t s = 0; s < legends_.size(); ++s) {
    legends_[s]->clearItems();
    for (std::size_t k = 2 * s; k < 2 * s + 2; ++k)
      if ((mask & (1u << k)) != 0) graphs_[k]->addToLegend(legends_[s]);
  }
}

void FollowPlotView::refresh_plane() {
  const Track stage = track_of(model_.series(FollowLine::StageX), model_.series(FollowLine::StageY));
  const Track grain = track_of(model_.series(FollowLine::GrainX), model_.series(FollowLine::GrainY));
  stage_track_->setData(stage.t, stage.x, stage.y, true);
  grain_track_->setData(grain.t, grain.x, grain.y, true);
  stage_track_->setVisible(!stage.t.isEmpty());
  grain_track_->setVisible(!grain.t.isEmpty());

  const auto mark = [](QCPGraph* now, const Track& track) {
    now->data()->clear();
    if (!track.t.isEmpty()) now->addData(track.x.back(), track.y.back());
    now->setVisible(!track.t.isEmpty());
  };
  mark(stage_now_, stage);
  mark(grain_now_, grain);

  QVector<double> around, fence_x, fence_y;
  const auto& fence = model_.fence();
  if (fence) {
    for (int i = 0; i < kFencePoints; ++i) {
      const double a = 2 * std::numbers::pi * i / (kFencePoints - 1);
      around.push_back(i);
      fence_x.push_back(fence->x + fence->radius_mm * std::cos(a));
      fence_y.push_back(fence->y + fence->radius_mm * std::sin(a));
    }
  }
  fence_->setData(around, fence_x, fence_y, true);
  fence_->setVisible(fence.has_value());

  xy_->legend->clearItems();
  if (!stage.t.isEmpty()) stage_track_->addToLegend();
  if (!grain.t.isEmpty()) grain_track_->addToLegend();
  if (fence) fence_->addToLegend();

  bool any = false;
  double lo_x = 0, hi_x = 0, lo_y = 0, hi_y = 0;
  const auto widen = [&any, &lo_x, &hi_x, &lo_y, &hi_y](const QVector<double>& xs, const QVector<double>& ys) {
    for (qsizetype i = 0; i < xs.size(); ++i) {
      lo_x = any ? std::min(lo_x, xs[i]) : xs[i];
      hi_x = any ? std::max(hi_x, xs[i]) : xs[i];
      lo_y = any ? std::min(lo_y, ys[i]) : ys[i];
      hi_y = any ? std::max(hi_y, ys[i]) : ys[i];
      any = true;
    }
  };
  widen(stage.x, stage.y);
  widen(grain.x, grain.y);
  widen(fence_x, fence_y);
  if (any) {
    const double pad = std::max(std::max(hi_x - lo_x, hi_y - lo_y) * 0.08, 0.05);
    fit_x_ = (lo_x + hi_x) / 2;
    fit_y_ = (lo_y + hi_y) / 2;
    fit_width_ = hi_x - lo_x + 2 * pad;
    fit_height_ = hi_y - lo_y + 2 * pad;
  }
}

void FollowPlotView::fit_plane() {
  const QRect shown = xy_->axisRect()->rect();
  if (shown.width() <= 0 || shown.height() <= 0) return;
  const QCPRange was_x = xy_->xAxis->range();
  const QCPRange was_y = xy_->yAxis->range();
  setting_plane_ = true;
  if (auto_plane_) {
    // The scale that shows all of it both ways; the longer way shows more.
    const double px_per_mm = std::min(shown.width() / fit_width_, shown.height() / fit_height_);
    xy_->xAxis->setRange(fit_x_, shown.width() / px_per_mm, Qt::AlignCenter);
    xy_->yAxis->setRange(fit_y_, shown.height() / px_per_mm, Qt::AlignCenter);
  } else {
    xy_->yAxis->setScaleRatio(xy_->xAxis, 1.0);
  }
  setting_plane_ = false;
  // The ticks of this replot are already made, for the ranges it began with.
  const auto moved = [](const QCPRange& a, const QCPRange& b) {
    return std::abs(a.lower - b.lower) + std::abs(a.upper - b.upper) > 1e-9 * (a.size() + b.size());
  };
  if (moved(was_x, xy_->xAxis->range()) || moved(was_y, xy_->yAxis->range())) xy_->replot(QCustomPlot::rpQueuedReplot);
}

void FollowPlotView::refresh() {
  for (std::size_t k = 0; k < graphs_.size(); ++k) {
    const auto line = static_cast<FollowLine>(k + kFirstStripLine);
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
  refresh_plane();

  // A new run (the model began again) takes the x range back.
  const double latest = model_.latest_x();
  if (latest < last_x_) {
    auto_x_ = true;
    auto_plane_ = true;
  }
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
      const auto line = static_cast<FollowLine>(k + kFirstStripLine);
      if (!model_.has(line)) continue;
      if (const auto r = model_.series(line).range(shown.lower, shown.upper)) {
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
  xy_->replot();
  plot_->replot();
}

int FollowPlotView::point_count(FollowLine line) const {
  if (line == FollowLine::StageX || line == FollowLine::StageY) return stage_track_->dataCount();
  if (line == FollowLine::GrainX || line == FollowLine::GrainY) return grain_track_->dataCount();
  return graphs_[line_index(line) - kFirstStripLine]->dataCount();
}

bool FollowPlotView::line_visible(FollowLine line) const {
  if (line == FollowLine::StageX || line == FollowLine::StageY) return stage_track_->visible();
  if (line == FollowLine::GrainX || line == FollowLine::GrainY) return grain_track_->visible();
  return graphs_[line_index(line) - kFirstStripLine]->visible();
}

int FollowPlotView::fence_point_count() const { return fence_->dataCount(); }

}  // namespace pychron::ui
