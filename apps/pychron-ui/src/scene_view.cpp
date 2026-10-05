#include "scene_view.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>
#include <map>
#include <numeric>

#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QRubberBand>
#include <QToolTip>
#include <QVBoxLayout>

#include <qcustomplot.h>

namespace pychron::ui {

namespace pp = pychron::processing;

namespace {

QColor qcolor(const pp::Color& c) { return QColor(c.r, c.g, c.b, c.a); }

Qt::PenStyle pen_style(pp::LineDash d) {
  switch (d) {
    case pp::LineDash::Dash:
      return Qt::DashLine;
    case pp::LineDash::Dot:
      return Qt::DotLine;
    case pp::LineDash::DashDot:
      return Qt::DashDotLine;
    case pp::LineDash::Solid:
      break;
  }
  return Qt::SolidLine;
}

QCPScatterStyle::ScatterShape shape(pp::MarkerShape s) {
  switch (s) {
    case pp::MarkerShape::Square:
      return QCPScatterStyle::ssSquare;
    case pp::MarkerShape::Diamond:
      return QCPScatterStyle::ssDiamond;
    case pp::MarkerShape::Triangle:
      return QCPScatterStyle::ssTriangle;
    case pp::MarkerShape::Cross:
      return QCPScatterStyle::ssCross;
    case pp::MarkerShape::Plus:
      return QCPScatterStyle::ssPlus;
    case pp::MarkerShape::Star:
      return QCPScatterStyle::ssStar;
    case pp::MarkerShape::Circle:
      break;
  }
  return QCPScatterStyle::ssCircle;
}

QCPScatterStyle scatter(const pp::MarkerStyle& m) {
  const QColor c = qcolor(m.color);
  return QCPScatterStyle(shape(m.shape), QPen(c, 1.2), m.filled ? QBrush(c) : QBrush(Qt::NoBrush), m.size);
}

// strftime-style pattern -> QDateTime format.
QString qt_time_format(const std::string& f) {
  if (f.empty()) return QStringLiteral("yyyy-MM-dd\nhh:mm");
  QString out;
  for (std::size_t i = 0; i < f.size(); ++i) {
    if (f[i] != '%' || i + 1 >= f.size()) {
      const QChar c = QChar::fromLatin1(f[i]);
      out += c.isLetter() ? QStringLiteral("'") + c + QStringLiteral("'") : QString(c);
      continue;
    }
    switch (f[++i]) {
      case 'Y': out += QStringLiteral("yyyy"); break;
      case 'y': out += QStringLiteral("yy"); break;
      case 'm': out += QStringLiteral("MM"); break;
      case 'd': out += QStringLiteral("dd"); break;
      case 'b': out += QStringLiteral("MMM"); break;
      case 'H': out += QStringLiteral("hh"); break;
      case 'M': out += QStringLiteral("mm"); break;
      case 'S': out += QStringLiteral("ss"); break;
      case 'n': out += QStringLiteral("\n"); break;
      case '%': out += QStringLiteral("%"); break;
      default: break;
    }
  }
  return out;
}

QFont scene_font(const pp::SceneStyle& s, double size, bool bold = false) {
  QFont f = QApplication::font();
  if (!s.fonts.family.empty()) f.setFamily(QString::fromStdString(s.fonts.family));
  if (size > 0) f.setPointSizeF(size);
  f.setBold(bold);
  return f;
}

Qt::Alignment corner_alignment(pp::Corner c) {
  switch (c) {
    case pp::Corner::TopRight:
      return Qt::AlignTop | Qt::AlignRight;
    case pp::Corner::BottomLeft:
      return Qt::AlignBottom | Qt::AlignLeft;
    case pp::Corner::BottomRight:
      return Qt::AlignBottom | Qt::AlignRight;
    case pp::Corner::TopLeft:
      break;
  }
  return Qt::AlignTop | Qt::AlignLeft;
}

QPointF corner_ratio(pp::Corner c) {
  switch (c) {
    case pp::Corner::TopRight:
      return {0.99, 0.02};
    case pp::Corner::BottomLeft:
      return {0.01, 0.98};
    case pp::Corner::BottomRight:
      return {0.99, 0.98};
    case pp::Corner::TopLeft:
      break;
  }
  return {0.01, 0.02};
}

constexpr int kHitPixels = 8;

}  // namespace

SceneView::SceneView(QWidget* parent) : QWidget(parent), plot_(new QCustomPlot(this)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(plot_);
  plot_->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
  plot_->setAutoAddPlottableToLegend(false);
  plot_->setMouseTracking(true);
  plot_->installEventFilter(this);
  plot_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(plot_, &QWidget::customContextMenuRequested, this, &SceneView::show_context_menu);
  band_ = new QRubberBand(QRubberBand::Rectangle, plot_);
  rebuild();
}

void SceneView::set_scene(pp::ScenePtr scene) {
  // Keep the view when the new scene has the same shape (a click-to-omit
  // rerun): only the data changes.
  std::vector<std::pair<QCPRange, QCPRange>> kept;
  const bool same_shape = scene_ && scene && scene_->graphs.size() == scene->graphs.size() &&
                          std::equal(scene_->graphs.begin(), scene_->graphs.end(), scene->graphs.begin(),
                                     [](const pp::Graph& a, const pp::Graph& b) {
                                       if (a.panels.size() != b.panels.size()) return false;
                                       for (std::size_t i = 0; i < a.panels.size(); ++i)
                                         if (a.panels[i].quantity != b.panels[i].quantity) return false;
                                       return a.x.format == b.x.format;
                                     });
  if (same_shape)
    for (const auto& r : rects_)
      kept.emplace_back(r.rect->axis(QCPAxis::atBottom)->range(), r.rect->axis(QCPAxis::atLeft)->range());
  scene_ = std::move(scene);
  rebuild();
  if (same_shape && kept.size() == rects_.size()) {
    for (std::size_t i = 0; i < rects_.size(); ++i) {
      rects_[i].rect->axis(QCPAxis::atBottom)->setRange(kept[i].first);
      rects_[i].rect->axis(QCPAxis::atLeft)->setRange(kept[i].second);
    }
    plot_->replot();
  }
}

void SceneView::rebuild() {
  plot_->clearItems();
  plot_->clearPlottables();
  plot_->plotLayout()->clear();
  rects_.clear();
  if (!scene_ || scene_->graphs.empty()) {
    auto* empty = new QCPTextElement(plot_, tr("No data"));
    plot_->plotLayout()->addElement(0, 0, empty);
    plot_->replot();
    return;
  }
  const pp::Scene& s = *scene_;
  plot_->setBackground(QBrush(qcolor(s.style.background)));
  const int columns = std::max(1, s.columns);
  int index = 0;
  for (const auto& g : s.graphs) {
    const int row = index / columns, col = index % columns;
    ++index;
    auto* sub = new QCPLayoutGrid;
    plot_->plotLayout()->addElement(row, col, sub);
    sub->setRowSpacing(s.style.panel_spacing);
    int sub_row = 0;
    if (!g.title.empty()) {
      auto* title = new QCPTextElement(plot_, QString::fromStdString(g.title), scene_font(s.style, s.style.fonts.title, true));
      sub->addElement(sub_row++, 0, title);
      sub->setRowStretchFactor(0, 0.001);
    }
    auto* margins = new QCPMarginGroup(plot_);
    std::vector<QCPAxis*> xaxes;
    for (std::size_t pi = 0; pi < g.panels.size(); ++pi) {
      const auto& panel = g.panels[pi];
      auto* rect = new QCPAxisRect(plot_);
      sub->addElement(sub_row, 0, rect);
      sub->setRowStretchFactor(sub_row, std::max(0.05, panel.height));
      ++sub_row;
      rect->setMarginGroup(QCP::msLeft | QCP::msRight, margins);
      rect->setBackground(QBrush(qcolor(s.style.plot_background)));
      QCPAxis* x = rect->axis(QCPAxis::atBottom);
      QCPAxis* y = rect->axis(QCPAxis::atLeft);
      rect->setRangeDragAxes(x, y);
      rect->setRangeZoomAxes(x, y);
      x->grid()->setVisible(s.style.grid);
      y->grid()->setVisible(s.style.grid);
      x->setTickLabelFont(scene_font(s.style, s.style.fonts.tick));
      y->setTickLabelFont(scene_font(s.style, s.style.fonts.tick));
      x->setLabelFont(scene_font(s.style, s.style.fonts.axis_title));
      y->setLabelFont(scene_font(s.style, s.style.fonts.axis_title));
      y->setLabel(QString::fromStdString(panel.y.title));
      const bool bottom = pi + 1 == g.panels.size();
      x->setTickLabels(bottom);
      if (bottom) x->setLabel(QString::fromStdString(g.x.title));
      if (g.x.format == pp::AxisFormat::Time) {
        QSharedPointer<QCPAxisTickerDateTime> ticker(new QCPAxisTickerDateTime);
        ticker->setDateTimeFormat(qt_time_format(g.x.time_format));
        ticker->setDateTimeSpec(Qt::UTC);
        ticker->setTickCount(6);
        x->setTicker(ticker);
      }
      RectInfo info;
      info.rect = rect;
      info.x_min = g.x.min;
      info.x_max = g.x.max;
      info.y_min = panel.y.min;
      info.y_max = panel.y.max;
      if (panel.y.scale == pp::AxisScale::Log) {
        info.log = true;
        y->setScaleType(QCPAxis::stLogarithmic);
        y->setTicker(QSharedPointer<QCPAxisTickerLog>(new QCPAxisTickerLog));
      }
      QCPLegend* legend = nullptr;
      if (pi == 0 && s.style.legend) {
        legend = new QCPLegend;
        rect->insetLayout()->addElement(legend, corner_alignment(s.style.legend_corner));
        legend->setFont(scene_font(s.style, s.style.fonts.annotation));
        legend->setBrush(QBrush(theme().overlay));
        legend->setLayer(QStringLiteral("legend"));
      }
      std::map<pp::Corner, QStringList> corner_text;

      for (const auto& layer : panel.layers) {
        if (const auto* band = std::get_if<pp::BandLayer>(&layer)) {
          QCPGraph* lo = plot_->addGraph(x, y);
          QCPGraph* hi = plot_->addGraph(x, y);
          lo->setData(QVector<double>(band->x.begin(), band->x.end()), QVector<double>(band->low.begin(), band->low.end()), true);
          hi->setData(QVector<double>(band->x.begin(), band->x.end()), QVector<double>(band->high.begin(), band->high.end()), true);
          lo->setPen(Qt::NoPen);
          hi->setPen(Qt::NoPen);
          hi->setBrush(QBrush(qcolor(band->fill)));
          hi->setChannelFillGraph(lo);
        } else if (const auto* line = std::get_if<pp::LineLayer>(&layer)) {
          QCPGraph* gl = plot_->addGraph(x, y);
          gl->setData(QVector<double>(line->x.begin(), line->x.end()), QVector<double>(line->y.begin(), line->y.end()), true);
          gl->setPen(QPen(qcolor(line->style.color), line->style.width, pen_style(line->style.dash)));
        } else if (const auto* pts = std::get_if<pp::PointLayer>(&layer)) {
          // Sorted by x so error bars stay aligned with QCPGraph's sorted data.
          std::vector<std::size_t> order(pts->x.size());
          std::iota(order.begin(), order.end(), 0);
          std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return pts->x[a] < pts->x[b]; });
          for (const bool excluded : {false, true}) {
            if (excluded && !pts->show_excluded) continue;
            QVector<double> gx, gy, ge, gxe;
            for (std::size_t i : order) {
              const bool ex = i < pts->excluded.size() && pts->excluded[i];
              if (ex != excluded) continue;
              gx << pts->x[i];
              gy << pts->y[i];
              if (!pts->y_err.empty()) ge << pts->y_err[i];
              if (!pts->x_err.empty()) gxe << pts->x_err[i];
              info.points.push_back({pts->x[i], pts->y[i], i < pts->refs.size() ? pts->refs[i].analysis : std::string(),
                                     i < pts->tooltips.size() ? pts->tooltips[i] : std::string()});
            }
            if (gx.isEmpty()) continue;
            QCPGraph* gp = plot_->addGraph(x, y);
            gp->setLineStyle(QCPGraph::lsNone);
            // Every point is clickable, and QCustomPlot 2.1.1's scatter thinning
            // divides by a zero pixel span when a column's points are off-range.
            gp->setAdaptiveSampling(false);
            gp->setScatterStyle(scatter(excluded ? pts->excluded_marker : pts->marker));
            gp->setData(gx, gy, true);
            gp->setName(QString::fromStdString(pts->label) + (excluded ? tr(" (excluded)") : QString()));
            if (!ge.isEmpty()) {
              auto* eb = new QCPErrorBars(x, y);
              eb->setDataPlottable(gp);
              eb->setData(ge);
              eb->setPen(QPen(excluded ? qcolor(pts->excluded_marker.color) : qcolor(pts->marker.color), 1));
              eb->setWhiskerWidth(0);
            }
            if (!gxe.isEmpty()) {
              auto* eb = new QCPErrorBars(x, y);
              eb->setErrorType(QCPErrorBars::etKeyError);
              eb->setDataPlottable(gp);
              eb->setData(gxe);
              eb->setPen(QPen(excluded ? qcolor(pts->excluded_marker.color) : qcolor(pts->marker.color), 1));
              eb->setWhiskerWidth(0);
            }
            if (legend && !excluded && !pts->label.empty()) legend->addItem(new QCPPlottableLegendItem(legend, gp));
          }
        } else if (const auto* steps = std::get_if<pp::StepLayer>(&layer)) {
          for (std::size_t i = 0; i < steps->x0.size(); ++i) {
            const double e = i < steps->y_err.size() ? steps->y_err[i] : 0.0;
            const bool ex = i < steps->excluded.size() && steps->excluded[i];
            const bool hl = i < steps->highlighted.size() && steps->highlighted[i];
            auto* box = new QCPItemRect(plot_);
            box->setClipAxisRect(rect);
            for (auto* pos : {box->topLeft, box->bottomRight}) {
              pos->setAxisRect(rect);
              pos->setAxes(x, y);
            }
            box->topLeft->setCoords(steps->x0[i], steps->y[i] + e);
            box->bottomRight->setCoords(steps->x1[i], steps->y[i] - e);
            QColor fill = qcolor(ex ? steps->excluded_fill : steps->fill);
            if (!ex && steps->dim_others && !hl) fill.setAlpha(fill.alpha() / 3);
            box->setBrush(QBrush(fill));
            box->setPen(QPen(ex ? qcolor(steps->excluded_fill).darker(150) : qcolor(steps->line), ex ? 0.8 : 1.0,
                             ex ? Qt::DashLine : Qt::SolidLine));
            HitBox hb{steps->x0[i], steps->x1[i], steps->y[i] - e, steps->y[i] + e,
                      {0.5 * (steps->x0[i] + steps->x1[i]), steps->y[i],
                       i < steps->refs.size() ? steps->refs[i].analysis : std::string(),
                       i < steps->tooltips.size() ? steps->tooltips[i] : std::string()}};
            info.boxes.push_back(std::move(hb));
            if (i < steps->labels.size() && !steps->labels[i].empty()) {
              auto* t = new QCPItemText(plot_);
              t->setClipAxisRect(rect);
              t->position->setAxisRect(rect);
              t->position->setAxes(x, y);
              t->position->setCoords(0.5 * (steps->x0[i] + steps->x1[i]), steps->y[i] + e);
              t->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
              t->setText(QString::fromStdString(steps->labels[i]));
              t->setFont(scene_font(s.style, s.style.fonts.annotation));
            }
          }
        } else if (const auto* el = std::get_if<pp::EllipseLayer>(&layer)) {
          constexpr int kSegments = 72;
          for (std::size_t i = 0; i < el->x.size(); ++i) {
            const bool ex = i < el->excluded.size() && el->excluded[i];
            const double rho = std::clamp(el->rho[i], -0.999999, 0.999999);
            QVector<double> ex_x, ex_y, t;
            for (int k = 0; k <= kSegments; ++k) {
              const double a = 2.0 * std::numbers::pi * k / kSegments;
              ex_x << el->x[i] + el->scale * el->sx[i] * std::cos(a);
              ex_y << el->y[i] + el->scale * el->sy[i] * (rho * std::cos(a) + std::sqrt(1 - rho * rho) * std::sin(a));
              t << k;
            }
            auto* curve = new QCPCurve(x, y);
            curve->setData(t, ex_x, ex_y, true);
            curve->setPen(QPen(ex ? theme().inactive : qcolor(el->line), 1.0, ex ? Qt::DashLine : Qt::SolidLine));
            if (el->filled && !ex) curve->setBrush(QBrush(qcolor(el->fill)));
          }
        } else if (const auto* text = std::get_if<pp::TextLayer>(&layer)) {
          for (const auto& l : text->lines) corner_text[text->corner] << QString::fromStdString(l);
        } else if (const auto* guide = std::get_if<pp::GuideLayer>(&layer)) {
          auto* l = new QCPItemStraightLine(plot_);
          l->setClipAxisRect(rect);
          for (auto* p : {l->point1, l->point2}) {
            p->setAxisRect(rect);
            p->setAxes(x, y);
          }
          if (guide->horizontal) {
            l->point1->setCoords(0, guide->value);
            l->point2->setCoords(1, guide->value);
          } else {
            l->point1->setCoords(guide->value, 0);
            l->point2->setCoords(guide->value, 1);
          }
          l->setPen(QPen(qcolor(guide->style.color), guide->style.width, pen_style(guide->style.dash)));
        }
      }
      for (const auto& [corner, lines] : corner_text) {
        auto* t = new QCPItemText(plot_);
        t->setClipAxisRect(rect);
        t->position->setType(QCPItemPosition::ptAxisRectRatio);
        t->position->setAxisRect(rect);
        t->position->setCoords(corner_ratio(corner));
        t->setPositionAlignment(corner_alignment(corner));
        t->setTextAlignment(Qt::AlignLeft);
        t->setText(lines.join(QLatin1Char('\n')));
        t->setFont(scene_font(s.style, s.style.fonts.annotation));
        t->setPadding(QMargins(4, 2, 4, 2));
        t->setBrush(QBrush(theme().overlay));
        for (const auto& line : lines) info.texts.push_back(line);
      }
      if (legend && legend->itemCount() == 0) legend->setVisible(false);
      xaxes.push_back(x);
      rects_.push_back(std::move(info));
    }
    // Shared x within a graph.
    for (QCPAxis* a : xaxes)
      for (QCPAxis* b : xaxes)
        if (a != b) connect(a, qOverload<const QCPRange&>(&QCPAxis::rangeChanged), b, qOverload<const QCPRange&>(&QCPAxis::setRange));
  }
  reset_view();
}

void SceneView::reset_view() {
  for (auto& info : rects_) {
    QCPAxis* x = info.rect->axis(QCPAxis::atBottom);
    QCPAxis* y = info.rect->axis(QCPAxis::atLeft);
    bool found = false;
    QCPRange yr;
    for (QCPAbstractPlottable* p : info.rect->plottables()) {
      bool ok = false;
      const QCPRange r = p->getValueRange(ok, QCP::sdBoth);
      if (!ok) continue;
      if (!found)
        yr = r;
      else
        yr.expand(r);
      found = true;
    }
    if (found) {
      if (info.log) {
        yr.lower = std::max(yr.lower, yr.upper * 1e-6);
        yr = QCPRange(yr.lower / 1.2, yr.upper * 1.2);
      } else {
        double pad = yr.size() * 0.08;
        if (pad == 0) pad = std::max(std::abs(yr.upper) * 0.05, 1e-12);
        yr = QCPRange(yr.lower - pad, yr.upper + pad);
      }
    } else {
      yr = info.log ? QCPRange(0.1, 10) : QCPRange(0, 1);
    }
    if (info.y_min) yr.lower = *info.y_min;
    if (info.y_max) yr.upper = *info.y_max;
    y->setRange(yr);
    if (info.x_min && info.x_max) x->setRange(*info.x_min, *info.x_max);
    else x->rescale(true);
  }
  plot_->replot();
}

bool SceneView::save_png(const QString& path, int width, int height) { return plot_->savePng(path, width, height); }

bool SceneView::save_pdf(const QString& path) { return plot_->savePdf(path); }

const SceneView::HitPoint* SceneView::hit(const QPoint& pos, const RectInfo** where) const {
  const HitPoint* best = nullptr;
  double best_d = kHitPixels * kHitPixels;
  for (const auto& info : rects_) {
    if (!info.rect->rect().contains(pos)) continue;
    QCPAxis* x = info.rect->axis(QCPAxis::atBottom);
    QCPAxis* y = info.rect->axis(QCPAxis::atLeft);
    for (const auto& p : info.points) {
      const double dx = x->coordToPixel(p.x) - pos.x(), dy = y->coordToPixel(p.y) - pos.y();
      const double d = dx * dx + dy * dy;
      if (d <= best_d) {
        best_d = d;
        best = &p;
        if (where) *where = &info;
      }
    }
    if (best) continue;
    // Steps: inside the box (at least a few pixels tall, so a tiny error still hits).
    const double px = x->pixelToCoord(pos.x());
    for (const auto& b : info.boxes) {
      if (px < b.x0 || px > b.x1) continue;
      const double top = std::min(y->coordToPixel(b.y1), y->coordToPixel(b.point.y) - 3);
      const double bottom = std::max(y->coordToPixel(b.y0), y->coordToPixel(b.point.y) + 3);
      if (pos.y() >= top && pos.y() <= bottom) {
        best = &b.point;
        if (where) *where = &info;
        break;
      }
    }
  }
  return best;
}

std::optional<QPoint> SceneView::point_position(const std::string& uuid, int panel) const {
  if (panel < 0 || panel >= static_cast<int>(rects_.size())) return std::nullopt;
  const auto& info = rects_[panel];
  std::vector<HitPoint> candidates = info.points;
  for (const auto& b : info.boxes) candidates.push_back(b.point);
  for (const auto& p : candidates)
    if (p.uuid == uuid) {
      const QPoint local(static_cast<int>(std::lround(info.rect->axis(QCPAxis::atBottom)->coordToPixel(p.x))),
                         static_cast<int>(std::lround(info.rect->axis(QCPAxis::atLeft)->coordToPixel(p.y))));
      if (!info.rect->rect().contains(local)) return std::nullopt;
      return plot_->mapTo(this, local);
    }
  return std::nullopt;
}

QStringList SceneView::points_in(const QRect& area) const {
  QStringList out;
  const QRect r = area.normalized();
  for (const auto& info : rects_) {
    QCPAxis* x = info.rect->axis(QCPAxis::atBottom);
    QCPAxis* y = info.rect->axis(QCPAxis::atLeft);
    std::vector<HitPoint> candidates = info.points;
    for (const auto& b : info.boxes) candidates.push_back(b.point);
    for (const auto& p : candidates) {
      const QPoint px(static_cast<int>(x->coordToPixel(p.x)), static_cast<int>(y->coordToPixel(p.y)));
      if (r.contains(px) && info.rect->rect().contains(px)) {
        const QString id = QString::fromStdString(p.uuid);
        if (!id.isEmpty() && !out.contains(id)) out << id;
      }
    }
  }
  return out;
}

QString SceneView::tooltip_at(const QPoint& pos) const {
  const HitPoint* p = hit(pos);
  return p ? QString::fromStdString(p->tooltip) : QString();
}

QStringList SceneView::texts(int panel) const {
  QStringList out;
  if (panel < 0 || panel >= static_cast<int>(rects_.size())) return out;
  for (const auto& t : rects_[panel].texts) out << t;
  return out;
}

bool SceneView::eventFilter(QObject* watched, QEvent* event) {
  if (watched != plot_) return QWidget::eventFilter(watched, event);
  switch (event->type()) {
    case QEvent::MouseButtonPress: {
      auto* e = static_cast<QMouseEvent*>(event);
      if (e->button() != Qt::LeftButton) break;
      press_pos_ = e->pos();
      pressed_ = true;
      if (e->modifiers() & Qt::ShiftModifier) {
        selecting_ = true;
        plot_->setInteraction(QCP::iRangeDrag, false);
        band_->setGeometry(QRect(press_pos_, QSize()));
        band_->show();
        return true;
      }
      break;
    }
    case QEvent::MouseMove: {
      auto* e = static_cast<QMouseEvent*>(event);
      if (selecting_) {
        band_->setGeometry(QRect(press_pos_, e->pos()).normalized());
        return true;
      }
      if (!(e->buttons() & Qt::LeftButton)) {
        const QString tip = tooltip_at(e->pos());
        if (!tip.isEmpty())
          QToolTip::showText(plot_->mapToGlobal(e->pos()), style::tip_text(tip), plot_);
        else
          QToolTip::hideText();
      }
      break;
    }
    case QEvent::MouseButtonRelease: {
      auto* e = static_cast<QMouseEvent*>(event);
      if (e->button() != Qt::LeftButton || !pressed_) break;
      pressed_ = false;
      if (selecting_) {
        selecting_ = false;
        band_->hide();
        plot_->setInteraction(QCP::iRangeDrag, true);
        const QStringList ids = points_in(QRect(press_pos_, e->pos()));
        if (!ids.isEmpty()) emit points_toggled(ids);
        return true;
      }
      if ((e->pos() - press_pos_).manhattanLength() <= 3) {
        if (const HitPoint* p = hit(e->pos()); p && !p->uuid.empty()) emit point_clicked(QString::fromStdString(p->uuid));
      }
      break;
    }
    case QEvent::MouseButtonDblClick:
      if (!hit(static_cast<QMouseEvent*>(event)->pos())) {
        reset_view();
        return true;
      }
      break;
    default:
      break;
  }
  return QWidget::eventFilter(watched, event);
}

void SceneView::show_context_menu(const QPoint& pos) {
  QMenu menu(this);
  const HitPoint* p = hit(pos);
  if (p && !p->uuid.empty()) {
    const QString id = QString::fromStdString(p->uuid);
    menu.addAction(tr("Include / exclude"), this, [this, id] { emit point_clicked(id); });
    menu.addAction(tr("Recall"), this, [this, id] { emit recall_requested(id); });
    menu.addSeparator();
  }
  menu.addAction(tr("Reset view"), this, &SceneView::reset_view);
  menu.addAction(tr("Copy image"), this, [this] { QApplication::clipboard()->setPixmap(plot_->toPixmap()); });
  menu.addAction(tr("Save as PNG..."), this, [this] {
    const QString path = QFileDialog::getSaveFileName(this, tr("Save figure"), QString(), tr("PNG (*.png)"));
    if (!path.isEmpty()) save_png(path);
  });
  menu.addAction(tr("Save as PDF..."), this, [this] {
    const QString path = QFileDialog::getSaveFileName(this, tr("Save figure"), QString(), tr("PDF (*.pdf)"));
    if (!path.isEmpty()) save_pdf(path);
  });
  menu.exec(plot_->mapToGlobal(pos));
}

}  // namespace pychron::ui
