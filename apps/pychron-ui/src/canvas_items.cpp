#include "canvas_items.hpp"
#include "theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <QCursor>
#include <QFontMetricsF>
#include <QAction>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
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
    : QGraphicsObject(parent), name_(std::move(name)), label_(QString::fromStdString(name_)), kind_(kind) {
  setZValue(2);
  setCursor(Qt::PointingHandCursor);
  setAcceptHoverEvents(true);
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

void ValveItem::hoverEnterEvent(QGraphicsSceneHoverEvent* event) {
  if (on_hover_) on_hover_();
  QGraphicsObject::hoverEnterEvent(event);
}

void ValveItem::set_details(const QStringList& lines) {
  if (lines == details_) return;
  details_ = lines;
  update_tip();
}

void ValveItem::update_tip() {
  QStringList lines{QString::fromStdString(name_)};
  lines += details_;
  if (!last_failure_.isEmpty()) lines += tr("Last failure: %1").arg(last_failure_);
  setToolTip(style::tip_text(lines.join(QLatin1Char('\n'))));
}

void ValveItem::flash(const QString& what) {
  last_failure_ = what;
  update_tip();
  flash_ticks_ = kFlashTicks;
  flash_timer_.start();
  update();
}

void ValveItem::set_label(QString label) {
  label_ = std::move(label);
  update();
}

QString ValveItem::shown_name(const QFontMetricsF& metrics) const {
  return metrics.elidedText(label_, Qt::ElideRight, kSize - 4);
}

QRectF ValveItem::wheel_rect() const {
  if (kind_ != canvas::ValveKind::Manual) {
    return {};
  }
  if (label_.isEmpty()) {
    return {-kWheelRadius, -kWheelRadius, 2 * kWheelRadius, 2 * kWheelRadius};
  }
  const double r = kCornerWheelRadius;
  const QPointF c(kSize / 2 - r - 2.5, -kSize / 2 + r + 2.5);
  return {c.x() - r, c.y() - r, 2 * r, 2 * r};
}

QRectF ValveItem::boundingRect() const {
  const double half = kSize / 2 + 4;  // room for the pending outline and the lock border
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
    // A handwheel: rim, hub and four spokes, set on the diagonal so it does
    // not read as a crosshair.
    const QRectF wheel = wheel_rect();
    const QPointF c = wheel.center();
    const double r = wheel.width() / 2;
    const bool small = r < kWheelRadius;
    painter->save();
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(theme().text, small ? 1.2 : 2.0));
    painter->drawEllipse(c, r, r);
    painter->setPen(QPen(theme().text, small ? 1.0 : 1.6, Qt::SolidLine, Qt::RoundCap));
    const double d = r * 0.7071;
    for (const QPointF& spoke : {QPointF(d, d), QPointF(d, -d)}) {
      painter->drawLine(c - spoke, c + spoke);
    }
    if (!small) {
      painter->setPen(Qt::NoPen);
      painter->setBrush(theme().text);
      painter->drawEllipse(c, 2.6, 2.6);
    }
    painter->restore();
  }
  // A labelled manual valve keeps its label clear of the wheel in the corner.
  const bool under_wheel = kind_ == canvas::ValveKind::Manual && !label_.isEmpty();
  painter->drawText(body.adjusted(0, 0, 0, -1), under_wheel ? Qt::AlignHCenter | Qt::AlignBottom : Qt::AlignCenter,
                    shown_name(QFontMetricsF(painter->font())));

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

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDialGap = 4.0;  // between a gauge's dial and its reading
constexpr double kSymbolGap = 3.0;   // between a glyph and the name beside it
constexpr double kSymbolPad = 4.0;   // between the glyph or name and the border
constexpr double kSymbolMin = 14.0;  // a glyph smaller than this is a smudge

// Glyphs, each drawn on its own design grid and scaled to fit `area`.
// `fill` is the box's own colour, for the parts gas reaches.
void paint_symbol(QPainter& painter, canvas::StageSymbol symbol, const QRectF& area, const QColor& fill) {
  const bool spectrometer = symbol == canvas::StageSymbol::Spectrometer;
  const bool quadrupole = symbol == canvas::StageSymbol::Quadrupole;
  const bool turbo = symbol == canvas::StageSymbol::Turbo;
  const bool getter = symbol == canvas::StageSymbol::Getter;
  const bool ion_pump = symbol == canvas::StageSymbol::IonPump;
  const bool square = quadrupole || turbo || getter || ion_pump;
  const QSizeF grid = spectrometer ? QSizeF(66, 62) : square ? QSizeF(40, 40) : QSizeF(67, 34);
  const double scale =
      std::min({area.width() / grid.width(), area.height() / grid.height(), spectrometer ? 0.8 : square ? 0.9 : 0.85});
  painter.save();
  painter.translate(area.center().x() - grid.width() * scale / 2, area.center().y() - grid.height() * scale / 2);
  painter.scale(scale, scale);
  const QPen line(theme().text, 1.2 / scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  if (spectrometer) {
    // A magnetic sector instrument seen from above: the source, the flight
    // tube turning a quarter circle through the magnet's poles, and the
    // collector block the masses fan out onto.
    const QColor metal = theme().inactive;
    const QPointF center(46, 48);
    auto ring = [&](double r) { return QRectF(center.x() - r, center.y() - r, 2 * r, 2 * r); };
    QPainterPath tube(QPointF(12, 56));
    tube.lineTo(12, 48);
    tube.arcTo(ring(34), 180, -90);
    tube.lineTo(52, 14);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(theme().text, 9, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
    painter.drawPath(tube);
    painter.setPen(QPen(fill, 9 - 2.4 / scale, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
    painter.drawPath(tube);

    QPainterPath magnet;  // the pole piece: a wedge of the turn, wider than the tube
    magnet.arcMoveTo(ring(46), 162);
    magnet.arcTo(ring(46), 162, -54);
    magnet.arcTo(ring(22), 108, 54);
    magnet.closeSubpath();
    painter.setPen(line);
    painter.setBrush(metal);
    painter.drawPath(magnet);

    painter.drawRoundedRect(QRectF(3, 50, 18, 11), 2, 2);  // source
    const QRectF collector(50, 1, 15, 26);
    painter.drawRoundedRect(collector, 2, 2);
    for (const double y : {7.5, 14.0, 20.5}) {  // collector slits
      painter.drawLine(QPointF(collector.left() + 4, y), QPointF(collector.right() - 3, y));
    }
  } else if (quadrupole) {
    // A quadrupole mass filter seen end on: four rods, opposite pairs
    // wired together (one pair shaded), the ion beam down the middle.
    painter.setPen(line);
    for (const QPointF& rod : {QPointF(10, 10), QPointF(30, 30)}) {
      painter.setBrush(theme().inactive);
      painter.drawEllipse(rod, 8.5, 8.5);
    }
    for (const QPointF& rod : {QPointF(30, 10), QPointF(10, 30)}) {
      painter.setBrush(fill);
      painter.drawEllipse(rod, 8.5, 8.5);
    }
    painter.setBrush(theme().text);
    painter.drawEllipse(QPointF(20, 20), 2.5, 2.5);
  } else if (turbo) {
    // A turbomolecular pump from above: the housing and the rotor's swept
    // blades round the hub.
    painter.setPen(line);
    painter.setBrush(theme().inactive);
    painter.drawEllipse(QPointF(20, 20), 18.5, 18.5);
    painter.setBrush(Qt::NoBrush);
    for (int i = 0; i < 8; ++i) {
      const double a = i * kPi / 4;
      auto at = [](double angle, double r) { return QPointF(20 + r * std::cos(angle), 20 + r * std::sin(angle)); };
      QPainterPath blade(at(a, 5));
      blade.quadTo(at(a + 0.25, 11), at(a + 0.8, 15.5));
      painter.drawPath(blade);
    }
    painter.setBrush(fill);
    painter.drawEllipse(QPointF(20, 20), 5, 5);
  } else if (getter) {
    // A getter pump cartridge from the side: its flange, and the stack of
    // getter discs on the heater rod.
    painter.setPen(line);
    painter.setBrush(fill);
    painter.drawRect(QRectF(18, 7, 4, 31));
    painter.setBrush(theme().inactive);
    painter.drawRoundedRect(QRectF(3, 2, 34, 6), 1.5, 1.5);
    for (int i = 0; i < 4; ++i) {
      painter.drawRoundedRect(QRectF(8, 12 + i * 6.5, 24, 4.2), 1.5, 1.5);
    }
  } else if (ion_pump) {
    // A sputter ion pump from the side: the flange, the body with its anode
    // cells, and the magnets either side of it.
    painter.setPen(line);
    painter.setBrush(theme().inactive);
    painter.drawRoundedRect(QRectF(9, 1.5, 22, 5), 1.5, 1.5);
    painter.drawRect(QRectF(1.5, 14, 6.5, 21));
    painter.drawRect(QRectF(32, 14, 6.5, 21));
    painter.setBrush(fill);
    painter.drawRect(QRectF(16.5, 6.5, 7, 5.5));
    painter.drawRoundedRect(QRectF(8, 12, 24, 25), 2, 2);
    for (const QPointF& cell : {QPointF(15, 19.5), QPointF(25, 19.5), QPointF(15, 29.5), QPointF(25, 29.5)}) {
      painter.drawEllipse(cell, 3.6, 3.6);
    }
  } else {
    // A laser from the side: the head with its cooling fins, the beam out of
    // the aperture, a lens, and the beam brought to a focus on the sample.
    const QColor beam = theme().error;
    painter.setPen(Qt::NoPen);
    painter.setBrush(beam);
    painter.drawRect(QRectF(29, 13, 15, 8));
    painter.drawPolygon(QPolygonF({QPointF(44, 13), QPointF(44, 21), QPointF(60, 17)}));

    painter.setPen(line);
    painter.setBrush(theme().inactive);
    painter.drawRoundedRect(QRectF(0.5, 7, 25, 20), 2.5, 2.5);  // head
    painter.drawRect(QRectF(25.5, 11.5, 3.5, 11));              // aperture
    for (const double x : {5.5, 9.5, 13.5}) {
      painter.drawLine(QPointF(x, 11), QPointF(x, 23));
    }
    QPainterPath lens(QPointF(44, 4));
    lens.quadTo(QPointF(49.5, 17), QPointF(44, 30));
    lens.quadTo(QPointF(38.5, 17), QPointF(44, 4));
    painter.setBrush(fill);
    painter.drawPath(lens);

    const QPointF focus(60, 17);  // where it lands: the hazard starburst
    for (int i = 0; i < 8; ++i) {
      const double angle = i * kPi / 4;
      const double reach = i % 2 == 0 ? 6.0 : 4.2;
      painter.drawLine(focus, focus + reach * QPointF(std::cos(angle), std::sin(angle)));
    }
  }
  painter.restore();
}

}  // namespace

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
  const QSizeF label = painter->fontMetrics().size(Qt::TextSingleLine, label_);
  const QRectF glyph = symbol_rect(label);
  if (glyph.isEmpty()) {
    painter->drawText(rect_, Qt::AlignCenter, label_);
    return;
  }
  const QRectF inner = rect_.adjusted(kSymbolPad, kSymbolPad, -kSymbolPad, -kSymbolPad);
  if (glyph.width() >= inner.width()) {  // above the name
    painter->drawText(QRectF(inner.left(), glyph.bottom(), inner.width(), inner.bottom() - glyph.bottom()),
                      Qt::AlignCenter, label_);
  } else {  // beside it, the font shrunk if the name is wider than the room left
    const double left = glyph.right() + kSymbolGap;
    const QRectF room(left, inner.top(), inner.right() - left, inner.height());
    painter->save();
    QFont font = painter->font();
    // Font sizes round, so step down until the name fits; elided past that.
    // NOLINTNEXTLINE(bugprone-float-loop-counter): a ratio shrunk until the name fits, not a count
    for (double shrink = room.width() / label.width(); shrink < 1 && shrink > 0.3; shrink *= 0.95) {
      if (painter->font().pointSizeF() > 0) {
        font.setPointSizeF(painter->font().pointSizeF() * shrink);
      } else {
        font.setPixelSize(std::max(1, int(painter->font().pixelSize() * shrink)));
      }
      if (QFontMetricsF(font).horizontalAdvance(label_) <= room.width()) {
        break;
      }
    }
    painter->setFont(font);
    painter->drawText(room, Qt::AlignCenter,
                      painter->fontMetrics().elidedText(label_, Qt::ElideRight, int(std::ceil(room.width()))));
    painter->restore();
  }
  paint_symbol(*painter, symbol_, glyph, region_);
}

void StageItem::set_symbol(canvas::StageSymbol symbol) {
  if (symbol != symbol_) {
    symbol_ = symbol;
    update();
  }
}

QRectF StageItem::symbol_rect(QSizeF label) const {
  if (symbol_ == canvas::StageSymbol::None) {
    return {};
  }
  const QRectF inner = rect_.adjusted(kSymbolPad, kSymbolPad, -kSymbolPad, -kSymbolPad);
  if (inner.height() - label.height() >= kSymbolMin) {
    return {inner.left(), inner.top(), inner.width(), inner.height() - label.height()};
  }
  // Beside the name, in the width it leaves; a name too wide for that gives
  // way to the smallest glyph and is shrunk to fit what is left.
  const double beside = std::max(kSymbolMin, inner.width() - label.width() - kSymbolGap);
  if (inner.height() >= kSymbolMin && inner.width() - kSymbolMin - kSymbolGap >= kSymbolMin) {
    return {inner.left(), inner.top(), std::min(inner.height(), beside), inner.height()};
  }
  return {};
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
  // Flat caps: a pipe ends at an element center or on another pipe's center
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
  // Just long enough to cover the volume's border: centered on the edge, or
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
  // A little smaller than the rest: a gauge is a fitting on a volume, and
  // its chip should not crowd the volume it sits beside.
  QFont small = font();
  small.setPointSizeF(small.pointSizeF() * 0.85);
  setFont(small);
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

// The dial sits left of the reading, as tall as the text.
QRectF GaugeLabelItem::dial_rect() const {
  const QRectF text = QGraphicsSimpleTextItem::boundingRect();
  const double side = text.height();
  return {text.left() - side - kDialGap, text.top(), side, side};
}

// The chip behind the dial and the reading: a gauge is a bordered component
// like the rest, and a pipe drawn to it ends under the chip.
QRectF GaugeLabelItem::chip_rect() const {
  return QGraphicsSimpleTextItem::boundingRect().united(dial_rect()).adjusted(-5, -3, 5, 3);
}

QRectF GaugeLabelItem::boundingRect() const { return chip_rect().adjusted(-1, -1, 1, 1); }

void GaugeLabelItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
  const QColor ink = brush().color();  // red with the reading while in alarm
  painter->setRenderHint(QPainter::Antialiasing, true);
  const QRectF chip = chip_rect();
  painter->setPen(QPen(ink, 1));
  painter->setBrush(theme().base);
  painter->drawRoundedRect(chip, chip.height() / 2, chip.height() / 2);
  QGraphicsSimpleTextItem::paint(painter, option, widget);

  // A pressure gauge's dial: face, scale ticks round the top, a needle.
  const QRectF dial = dial_rect();
  const QPointF c = dial.center();
  const double r = dial.width() / 2;
  painter->setPen(QPen(ink, 1.2));
  painter->setBrush(theme().base);
  painter->drawEllipse(c, r, r);
  painter->setPen(QPen(ink, 1.0));
  for (int i = 0; i < 5; ++i) {
    const double a = kPi * (1.0 + i / 4.0);  // left, round the top, to right
    const QPointF dir(std::cos(a), std::sin(a));
    painter->drawLine(c + dir * (r * 0.62), c + dir * (r * 0.88));
  }
  const double needle = kPi * 1.68;
  painter->setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap));
  painter->drawLine(c, c + QPointF(std::cos(needle), std::sin(needle)) * (r * 0.7));
  painter->setPen(Qt::NoPen);
  painter->setBrush(ink);
  painter->drawEllipse(c, 1.5, 1.5);
}

void GaugeLabelItem::set_wired(bool wired) {
  if (wired != wired_) {
    wired_ = wired;
    refresh();
  }
}

void GaugeLabelItem::refresh() {
  prepareGeometryChange();
  // A gauge the line does not define is there for illustration: its name only.
  setText(wired_ ? QStringLiteral("%1: %2").arg(QString::fromStdString(name_), value_) : QString::fromStdString(name_));
  setBrush(alarm_ ? theme().error_text : theme().text);
  // Center the chip (dial and text) on the element position the view assigns
  // with setPos(); the text itself starts at x = 0.
  const QRectF r = QGraphicsSimpleTextItem::boundingRect();
  const double dial = r.height() + kDialGap;
  setTransform(QTransform::fromTranslate(-(r.width() - dial) / 2, -r.height() / 2));
}

}  // namespace pychron::ui
