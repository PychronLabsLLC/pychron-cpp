#include "canvas_items.hpp"
#include "theme.hpp"

#include <algorithm>

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

QRectF StageItem::boundingRect() const { return rect_.adjusted(-1, -1, 1, 1); }

void StageItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
  // Corner radius scales with the smaller side so thin volumes stay pill-like
  // without swallowing the whole shape.
  const double radius = std::min(kCornerRadius, std::min(rect_.width(), rect_.height()) / 4);
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
  setPen(QPen(default_color(), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  setZValue(0);
}

QColor ConnectionItem::default_color() { return theme().outline; }

void ConnectionItem::set_region_color(QColor color) {
  if (color == pen().color()) {
    return;
  }
  QPen p = pen();
  p.setColor(color);
  setPen(p);
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
