#include "tray_view.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>

#include "theme.hpp"

namespace pychron::ui {

namespace {

constexpr double kPadding = 14;  // pixels around the tray

// A hole with no size of its own is still something to click.
double size_of(const laser::Hole& hole) { return hole.dimension > 0 ? hole.dimension : 1.0; }

}  // namespace

TrayView::TrayView(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("tray"));
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void TrayView::set_tray(const laser::TrayMap* map) {
  holes_.clear();
  name_.clear();
  if (map != nullptr) {
    name_ = QString::fromStdString(map->name());
    shape_ = map->shape();
    holes_ = map->holes();
  }
  update();
}

void TrayView::set_transform(std::optional<laser::Transform> transform) {
  transform_ = transform;
  update();
}

void TrayView::set_stage(std::optional<laser::StageXY> stage) {
  stage_ = stage;
  update();
}

void TrayView::set_calibration_holes(const QStringList& holes) {
  calibration_ = holes;
  update();
}

void TrayView::set_corrected_holes(const QStringList& holes) {
  corrected_ = holes;
  update();
}

void TrayView::set_current_hole(const QString& hole) {
  if (current_ == hole) return;
  current_ = hole;
  update();
}

TrayView::Fit TrayView::fit() const {
  Fit f;
  if (holes_.empty()) return f;
  double x0 = std::numeric_limits<double>::infinity(), x1 = -x0, y0 = x0, y1 = -x0;
  for (const auto& hole : holes_) {
    const double r = size_of(hole) / 2;
    x0 = std::min(x0, hole.x - r);
    x1 = std::max(x1, hole.x + r);
    y0 = std::min(y0, hole.y - r);
    y1 = std::max(y1, hole.y + r);
  }
  const double w = std::max(x1 - x0, 1e-6), h = std::max(y1 - y0, 1e-6);
  const double room_w = std::max(width() - 2 * kPadding, 10.0), room_h = std::max(height() - 2 * kPadding, 10.0);
  f.scale = std::min(room_w / w, room_h / h);
  // centred; the tray's y runs up, the widget's down
  f.origin = QPointF(width() / 2.0 - f.scale * (x0 + x1) / 2, height() / 2.0 + f.scale * (y0 + y1) / 2);
  return f;
}

QPointF TrayView::to_widget(double x, double y) const {
  const Fit f = fit();
  return {f.origin.x() + f.scale * x, f.origin.y() - f.scale * y};
}

const laser::Hole* TrayView::find(const QString& id) const {
  const std::string name = id.toStdString();
  for (const auto& hole : holes_) {
    if (hole.id == name) return &hole;
  }
  return nullptr;
}

QPointF TrayView::hole_center(const QString& id) const {
  const laser::Hole* hole = find(id);
  return hole != nullptr ? to_widget(hole->x, hole->y) : QPointF();
}

double TrayView::hole_radius(const QString& id) const {
  const laser::Hole* hole = find(id);
  return hole != nullptr ? fit().scale * size_of(*hole) / 2 : 0.0;
}

QString TrayView::hole_at(const QPointF& point) const {
  const Fit f = fit();
  const laser::Hole* best = nullptr;
  double nearest = std::numeric_limits<double>::infinity();
  for (const auto& hole : holes_) {
    const QPointF c = to_widget(hole.x, hole.y);
    const double d = std::hypot(point.x() - c.x(), point.y() - c.y());
    // a small hole is still given a few pixels to be hit in
    const double reach = std::max(f.scale * size_of(hole) / 2, 4.0);
    if (d <= reach && d < nearest) {
      nearest = d;
      best = &hole;
    }
  }
  return best != nullptr ? QString::fromStdString(best->id) : QString();
}

std::optional<QPointF> TrayView::stage_point() const {
  if (!transform_ || !stage_ || holes_.empty()) return std::nullopt;
  const laser::StageXY on_tray = transform_->to_map(stage_->x, stage_->y);
  return to_widget(on_tray.x, on_tray.y);
}

void TrayView::paintEvent(QPaintEvent*) {
  const Theme& t = theme();
  QPainter p(this);
  p.fillRect(rect(), t.base);
  if (holes_.empty()) {
    p.setPen(t.muted_text);
    p.drawText(rect(), Qt::AlignCenter, name_.isEmpty() ? tr("no tray") : tr("%1: no holes").arg(name_));
    return;
  }
  p.setRenderHint(QPainter::Antialiasing, true);
  const Fit f = fit();
  QFont small = font();
  small.setPointSizeF(std::max(small.pointSizeF() * 0.8, 6.0));
  p.setFont(small);
  for (const auto& hole : holes_) {
    const QString id = QString::fromStdString(hole.id);
    const QPointF c = to_widget(hole.x, hole.y);
    const double r = f.scale * size_of(hole) / 2;
    const QRectF box(c.x() - r, c.y() - r, 2 * r, 2 * r);
    const bool current = id == current_;
    p.setBrush(current ? t.accent_soft : t.window);
    p.setPen(QPen(current ? t.accent_strong : t.strong_border, 1));
    if (shape_ == laser::HoleShape::Square) p.drawRect(box);
    else p.drawEllipse(box);
    if (calibration_.contains(id)) {
      // ringed: a hole the calibration was taken at
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(t.accent, 2));
      p.drawEllipse(box.adjusted(-3, -3, 3, 3));
    }
    if (corrected_.contains(id)) {
      // dotted: a camera has found this hole
      p.setPen(Qt::NoPen);
      p.setBrush(t.ok);
      const double d = std::clamp(r * 0.35, 1.5, 4.0);
      p.drawEllipse(QPointF(c.x() + r * 0.6, c.y() - r * 0.6), d, d);
    }
    if (r >= 9) {
      p.setPen(current ? t.accent_strong : t.muted_text);
      p.drawText(box, Qt::AlignCenter, id);
    }
  }
  if (const auto at = stage_point()) {
    // the stage: a crosshair, and a mark on the edge when it is off the picture
    p.setRenderHint(QPainter::Antialiasing, false);
    const QPointF c(std::clamp(at->x(), 0.0, width() - 1.0), std::clamp(at->y(), 0.0, height() - 1.0));
    p.setPen(QPen(t.error, 1));
    p.drawLine(QPointF(c.x() - 9, c.y()), QPointF(c.x() + 9, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - 9), QPointF(c.x(), c.y() + 9));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, 4, 4);
  } else if (!transform_) {
    p.setPen(t.warning_text);
    p.drawText(rect().adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft, tr("not calibrated: the stage is not shown"));
  }
}

void TrayView::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    if (const QString id = hole_at(event->position()); !id.isEmpty()) emit holeClicked(id);
  }
  QWidget::mousePressEvent(event);
}

void TrayView::contextMenuEvent(QContextMenuEvent* event) {
  if (const QString id = hole_at(event->pos()); !id.isEmpty()) emit holeMenu(id, event->globalPos());
}

}  // namespace pychron::ui
