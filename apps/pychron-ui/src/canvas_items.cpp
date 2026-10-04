#include "canvas_items.hpp"
#include "theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <QCursor>
#include <QAction>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QStringList>

namespace pychron::ui {

namespace {

constexpr int kFlashTicks = 6;
constexpr int kFlashIntervalMs = 150;

}  // namespace

QPointF to_qpoint(const canvas::Point& p) { return {p.x, p.y}; }

QColor valve_color(ValveState state) {
  switch (state) {
    case ValveState::Open:
      return theme().ok;
    case ValveState::Closed:
      return theme().error;
    case ValveState::Unknown:
      break;
  }
  return theme().inactive;
}

// ---- ValveItem --------------------------------------------------------------

ValveItem::ValveItem(std::string name, canvas::ValveKind kind, QGraphicsItem* parent)
    : QGraphicsObject(parent), name_(std::move(name)), kind_(kind) {
  setZValue(2);
  setCursor(Qt::PointingHandCursor);
  setToolTip(QString::fromStdString(name_));
  flash_timer_.setInterval(kFlashIntervalMs);
  connect(&flash_timer_, &QTimer::timeout, this, [this] {
    if (--flash_ticks_ <= 0) {
      flash_ticks_ = 0;
      flash_timer_.stop();
    }
    update();
  });
}

QColor ValveItem::fill_color() const {
  if (is_flashing() && flash_ticks_ % 2 == 0) {
    return theme().flash;
  }
  if (state_ == ValveState::Open && inherited_) {
    return *inherited_;
  }
  return valve_color(state_);
}

void ValveItem::set_inherited_color(std::optional<QColor> color) {
  if (inherited_ != color) {
    inherited_ = color;
    update();
  }
}

void ValveItem::set_state(ValveState state) {
  state_ = state;
  update();
}

void ValveItem::set_locked(bool locked) {
  locked_ = locked;
  update();
}

void ValveItem::set_pending(bool pending) {
  pending_ = pending;
  update();
}

void ValveItem::flash(const QString& what) {
  setToolTip(QStringLiteral("%1: %2").arg(QString::fromStdString(name_), what));
  flash_ticks_ = kFlashTicks;
  flash_timer_.start();
  update();
}

QRectF ValveItem::boundingRect() const {
  const double half = kSize / 2 + 4;  // room for the pending outline and badge
  return {-half, -half, 2 * half, 2 * half};
}

void ValveItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
  const QRectF body(-kSize / 2, -kSize / 2, kSize, kSize);
  painter->setRenderHint(QPainter::Antialiasing, true);
  painter->setPen(QPen(theme().text, 1));
  painter->setBrush(fill_color());
  if (kind_ == canvas::ValveKind::Switch) {
    painter->drawEllipse(body);
  } else {
    painter->drawRoundedRect(body, kCornerRadius, kCornerRadius);
  }
  if (kind_ == canvas::ValveKind::Manual) {
    painter->drawLine(body.topLeft() + QPointF(3, 3), body.bottomRight() - QPointF(3, 3));
  }
  painter->drawText(body, Qt::AlignCenter, QString::fromStdString(name_));

  if (pending_) {
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(theme().text, 2, Qt::DashLine));
    painter->drawRoundedRect(body.adjusted(-3, -3, 3, 3), kCornerRadius + 2, kCornerRadius + 2);
  }
  if (locked_) {
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(lock_color(), kLockBorderWidth));
    if (kind_ == canvas::ValveKind::Switch) {
      painter->drawEllipse(body);
    } else {
      painter->drawRoundedRect(body, kCornerRadius, kCornerRadius);
    }
  }
}

void ValveItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event) {
  if (kind_ == canvas::ValveKind::Manual || !on_lock_request_) {
    event->ignore();
    return;
  }
  QMenu menu;
  QAction* toggle = menu.addAction(locked_ ? tr("Unlock valve") : tr("Lock valve"));
  if (menu.exec(event->screenPos()) == toggle) {
    on_lock_request_(name_, !locked_);
  }
  event->accept();
}

void ValveItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
  if (event->button() == Qt::LeftButton && on_click_) {
    on_click_(name_);
    event->accept();
    return;
  }
  QGraphicsObject::mousePressEvent(event);
}

// ---- StageItem --------------------------------------------------------------

StageItem::StageItem(std::string name, QString label, canvas::Size size, QColor base, QGraphicsItem* parent)
    : QGraphicsItem(parent),
      name_(std::move(name)),
      label_(std::move(label)),
      rect_(-size.width / 2, -size.height / 2, size.width, size.height),
      region_(base) {
  setZValue(1);
  setToolTip(QString::fromStdString(name_));
}

void StageItem::set_region_color(QColor color) {
  if (color != region_) {
    region_ = color;
    update();
  }
}

double StageItem::corner_radius(QSizeF size) {
  return std::min(kCornerRadius, std::min(size.width(), size.height()) / 4);
}

double StageItem::border_inset(const QRectF& box, const BoxEntry& entry, double width) {
  const double r = corner_radius(box.size());
  // Distance along the crossed edge from the pipe's nearer side to the
  // nearer corner of the box.
  const bool through_side = std::abs(entry.inward.x()) >= std::abs(entry.inward.y());
  const double along = through_side ? std::min(entry.edge.y() - box.top(), box.bottom() - entry.edge.y())
                                    : std::min(entry.edge.x() - box.left(), box.right() - entry.edge.x());
  const double d = std::max(0.0, along - width / 2);
  if (d >= r) {
    return 0;
  }
  return r - std::sqrt(r * r - (r - d) * (r - d));  // the arc, d along from the corner
}

QRectF StageItem::boundingRect() const { return rect_.adjusted(-1, -1, 1, 1); }

void StageItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
  const double radius = corner_radius(rect_.size());
  painter->setRenderHint(QPainter::Antialiasing, true);
  painter->setPen(QPen(theme().text, 1));
  painter->setBrush(region_);
  painter->drawRoundedRect(rect_, radius, radius);
  painter->drawText(rect_, Qt::AlignCenter, label_);
}

// ---- ConnectionItem ---------------------------------------------------------

ConnectionItem::ConnectionItem(const std::vector<QPointF>& points, double width, std::vector<std::string> endpoints,
                               QGraphicsItem* parent)
    : QGraphicsPathItem(parent), endpoints_(std::move(endpoints)) {
  QPainterPath path;
  if (!points.empty()) {
    path.moveTo(points.front());
    for (std::size_t i = 1; i < points.size(); ++i) {
      path.lineTo(points[i]);
    }
  }
  setPath(path);
  // Flat caps: a pipe ends at an element centre or on another pipe's centre
  // line, both covered. Mitred joins keep an elbow's corner square.
  setPen(QPen(default_color(), width, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
  setZValue(kFillZ);
  outline_ = new QGraphicsPathItem(path);
  outline_->setPen(QPen(theme().text, width + 2 * kBorderWidth, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
  outline_->setZValue(kOutlineZ);
}

// An isolated pipe is filled like an isolated volume; the border tells them apart.
QColor ConnectionItem::default_color() { return theme().neutral_fill; }

void ConnectionItem::set_region_color(QColor color) {
  if (color == pen().color()) {
    return;
  }
  QPen p = pen();
  p.setColor(color);
  setPen(p);
  for (QGraphicsPathItem* gap : gaps_) {
    QPen g = gap->pen();
    g.setColor(color);
    gap->setPen(g);
  }
}

QGraphicsPathItem* ConnectionItem::add_gap(const BoxEntry& entry, double depth) {
  // Just long enough to cover the volume's border: centred on the edge, or
  // up to `depth` inside it round a corner.
  QPainterPath path;
  path.moveTo(entry.edge - entry.inward * kBorderWidth);
  path.lineTo(entry.edge + entry.inward * (depth + kBorderWidth));
  auto* gap = new QGraphicsPathItem(path);
  gap->setPen(QPen(pen().color(), pen().widthF(), Qt::SolidLine, Qt::FlatCap));
  gap->setZValue(kGapZ);
  gaps_.push_back(gap);
  return gap;
}

std::optional<BoxEntry> box_entry(const std::vector<QPointF>& points, const QRectF& box) {
  for (std::size_t i = 0; i + 1 < points.size(); ++i) {
    const QPointF a = points[i];
    const QPointF d = points[i + 1] - a;
    if (box.contains(a) || d.isNull()) {
      continue;
    }
    // Clip the segment to the box (Liang-Barsky): it runs inside for t0..t1.
    double t0 = 0;
    double t1 = 1;
    bool crosses = true;
    const std::array<std::pair<double, double>, 4> slabs{{{-d.x(), a.x() - box.left()},
                                                          {d.x(), box.right() - a.x()},
                                                          {-d.y(), a.y() - box.top()},
                                                          {d.y(), box.bottom() - a.y()}}};
    for (const auto& [p, q] : slabs) {
      if (p == 0) {
        crosses = crosses && q >= 0;
      } else if (p < 0) {
        t0 = std::max(t0, q / p);
      } else {
        t1 = std::min(t1, q / p);
      }
    }
    if (crosses && t0 <= t1) {
      return BoxEntry{a + t0 * d, d / std::hypot(d.x(), d.y())};
    }
  }
  return std::nullopt;
}

// ---- LabelItem --------------------------------------------------------------

LabelItem::LabelItem(const QString& text, const QString& font_spec, QGraphicsItem* parent)
    : QGraphicsSimpleTextItem(text, parent) {
  setFont(parse_font(font_spec));
  setZValue(3);
}

QFont LabelItem::parse_font(const QString& spec) {
  QFont font;
  QStringList family;
  for (const QString& part : spec.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
    bool is_size = false;
    const int size = part.toInt(&is_size);
    if (is_size && size > 0) {
      font.setPointSize(size);
    } else {
      family << part;
    }
  }
  if (!family.isEmpty()) {
    font.setFamily(family.join(QLatin1Char(' ')));
  }
  return font;
}

// ---- GaugeLabelItem ---------------------------------------------------------

GaugeLabelItem::GaugeLabelItem(std::string name, QGraphicsItem* parent)
    : QGraphicsSimpleTextItem(parent), name_(std::move(name)) {
  setZValue(3);
  refresh();
}

void GaugeLabelItem::set_value(double value, const std::string& units) {
  value_ = QString::number(value, 'e', 2);
  if (!units.empty()) {
    value_ += QLatin1Char(' ') + QString::fromStdString(units);
  }
  refresh();
}

void GaugeLabelItem::set_alarm(bool alarm) {
  alarm_ = alarm;
  refresh();
}

void GaugeLabelItem::refresh() {
  setText(QStringLiteral("%1: %2").arg(QString::fromStdString(name_), value_));
  setBrush(alarm_ ? theme().error_text : theme().text);
  // Centre on the element position the view assigns with setPos().
  const QRectF r = QGraphicsSimpleTextItem::boundingRect();
  setTransform(QTransform::fromTranslate(-r.width() / 2, -r.height() / 2));
}

}  // namespace pychron::ui
