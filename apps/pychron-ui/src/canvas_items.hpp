#pragma once

// QGraphicsItems for the M1 canvas (spec section 10.3). Items are passive:
// they paint whatever state CanvasView pushes into them from the CoreBridge
// snapshot and report clicks through a callback. Positions are element
// centres in canvas.toml coordinates.

#include <functional>
#include <string>
#include <vector>

#include <QColor>
#include <QFont>
#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsSimpleTextItem>
#include <QPen>
#include <QPointF>
#include <QTimer>

#include "pychron/core/events.hpp"
#include "pychron/systems/canvas/canvas.hpp"

namespace pychron::ui {

QPointF to_qpoint(const canvas::Point& p);
QColor valve_color(ValveState state);

// Click -> actuate; colour by state; blue border when locked (context menu
// locks/unlocks); pending indicator; flashes
// and shows Error.what in its tooltip on rejection.
class ValveItem : public QGraphicsObject {
  Q_OBJECT

 public:
  static constexpr double kSize = 30.0;
  static constexpr double kCornerRadius = 5.0;
  static constexpr double kLockBorderWidth = 3.0;

  // Border colour of a software-locked valve.
  static QColor lock_color() { return QColor(0x1e, 0x6f, 0xe8); }

  ValveItem(std::string name, canvas::ValveKind kind, QGraphicsItem* parent = nullptr);

  const std::string& name() const noexcept { return name_; }
  canvas::ValveKind kind() const noexcept { return kind_; }
  ValveState state() const noexcept { return state_; }
  bool locked() const noexcept { return locked_; }
  bool is_pending() const noexcept { return pending_; }
  bool is_flashing() const noexcept { return flash_ticks_ > 0; }
  QColor fill_color() const;

  void set_state(ValveState state);
  void set_locked(bool locked);
  void set_pending(bool pending);
  // Rejection feedback: blink for ~1 s and show `what` as the tooltip.
  void flash(const QString& what);

  void set_on_click(std::function<void(const std::string&)> on_click) { on_click_ = std::move(on_click); }
  // Called with the requested state when the user picks the context-menu
  // lock/unlock action. Manual valves have no menu.
  void set_on_lock_request(std::function<void(const std::string&, bool)> cb) { on_lock_request_ = std::move(cb); }

  QRectF boundingRect() const override;
  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

 protected:
  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
  void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;

 private:
  std::string name_;
  canvas::ValveKind kind_;
  ValveState state_ = ValveState::Unknown;
  bool locked_ = false;
  bool pending_ = false;
  int flash_ticks_ = 0;
  QTimer flash_timer_;
  std::function<void(const std::string&)> on_click_;
  std::function<void(const std::string&, bool)> on_lock_request_;
};

// A stage or pipette volume; filled with its network region colour.
class StageItem : public QGraphicsItem {
 public:
  static constexpr double kCornerRadius = 8.0;

  StageItem(std::string name, QString label, canvas::Size size, QColor base, QGraphicsItem* parent = nullptr);

  const std::string& name() const noexcept { return name_; }
  QColor region_color() const noexcept { return region_; }
  void set_region_color(QColor color);

  QRectF boundingRect() const override;
  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

 private:
  std::string name_;
  QString label_;
  QRectF rect_;
  QColor region_;
};

// Plumbing drawn as a polyline through element centres. Remembers the names
// of the elements it joins so the view can paint it in the colour of the
// network region it belongs to.
class ConnectionItem : public QGraphicsPathItem {
 public:
  ConnectionItem(const std::vector<QPointF>& points, double width, std::vector<std::string> endpoints,
                 QGraphicsItem* parent = nullptr);

  static QColor default_color();

  const std::vector<std::string>& endpoints() const noexcept { return endpoints_; }
  QColor region_color() const { return pen().color(); }
  void set_region_color(QColor color);

 private:
  std::vector<std::string> endpoints_;
};

class LabelItem : public QGraphicsSimpleTextItem {
 public:
  LabelItem(const QString& text, const QString& font_spec, QGraphicsItem* parent = nullptr);
  // "Arial 12" -> family Arial, 12 pt; a bare family or size is accepted.
  static QFont parse_font(const QString& spec);
};

// Gauge value + units; red while in alarm.
class GaugeLabelItem : public QGraphicsSimpleTextItem {
 public:
  explicit GaugeLabelItem(std::string name, QGraphicsItem* parent = nullptr);

  const std::string& name() const noexcept { return name_; }
  bool in_alarm() const noexcept { return alarm_; }
  void set_value(double value, const std::string& units);
  void set_alarm(bool alarm);

 private:
  void refresh();

  std::string name_;
  QString value_ = QStringLiteral("--");
  bool alarm_ = false;
};

}  // namespace pychron::ui
