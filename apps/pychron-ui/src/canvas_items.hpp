#pragma once

// QGraphicsItems for the M1 canvas (spec section 10.3). Items are passive:
// they paint whatever state CanvasView pushes into them from the CoreBridge
// snapshot and report clicks through a callback. Positions are element
// centers in canvas.toml coordinates.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsSimpleTextItem>
#include <QPen>
#include <QPointF>
#include <QTimer>

#include "pychron/core/events.hpp"
#include "pychron/systems/canvas/canvas.hpp"

#include "theme.hpp"

namespace pychron::ui {

QPointF to_qpoint(const canvas::Point& p);
QColor valve_color(ValveState state);

// Click -> actuate; colour by state; thick blue border when locked (context
// menu locks/unlocks); pending indicator; flashes on rejection. The tooltip
// is the name, then what CanvasView knows of it (set_details), then the last
// rejection's Error.what. A manual valve wears a handwheel on its face
// (nothing sticks out of the body, so pipes can join any side); a label too
// long for the body is cut short (the tooltip has the name whole).
class ValveItem : public QGraphicsObject {
  Q_OBJECT

 public:
  static constexpr double kSize = 30.0;
  static constexpr double kCornerRadius = 5.0;
  static constexpr double kLockBorderWidth = 5.0;
  // A manual valve's mark: a handwheel on its face, filling a blank face
  // and tucked into the top-right corner of a labelled one.
  static constexpr double kWheelRadius = 9.0;
  static constexpr double kCornerWheelRadius = 4.5;

  // Border colour of a software-locked valve.
  static QColor lock_color() { return theme().lock; }

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
  // Region colour an open valve shows instead of green (CanvasView sets it
  // when canvas.toml says open_valve_color = "inherit"); nullopt = state colour.
  void set_inherited_color(std::optional<QColor> color);
  void set_pending(bool pending);
  // Rejection feedback: blink for ~1 s and keep `what` in the tooltip.
  void flash(const QString& what);
  // The tooltip's lines under the name: description, state, history.
  void set_details(const QStringList& lines);
  // Called as the pointer comes over the valve, before its tooltip shows:
  // the details hold durations, which are only right when asked for.
  void set_on_hover(std::function<void()> on_hover) { on_hover_ = std::move(on_hover); }

  void set_on_click(std::function<void(const std::string&)> on_click) { on_click_ = std::move(on_click); }
  // Called with the requested state when the user picks the context-menu
  // lock/unlock action. Manual valves have no menu.
  void set_on_lock_request(std::function<void(const std::string&, bool)> cb) { on_lock_request_ = std::move(cb); }

  // What is written on the face: the name unless set otherwise; may be empty.
  const QString& label() const noexcept { return label_; }
  void set_label(QString label);
  // The label as drawn: whole, or cut short with an ellipsis to fit the body.
  QString shown_name(const QFontMetricsF& metrics) const;
  // Where a manual valve's handwheel is drawn, in item coordinates; empty
  // for other kinds.
  QRectF wheel_rect() const;

  QRectF boundingRect() const override;
  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

 protected:
  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
  void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;
  void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;

 private:
  std::string name_;
  QString label_;
  canvas::ValveKind kind_;
  ValveState state_ = ValveState::Unknown;
  bool locked_ = false;
  std::optional<QColor> inherited_;
  bool pending_ = false;
  void update_tip();

  std::function<void()> on_hover_;

  QStringList details_;
  QString last_failure_;
  int flash_ticks_ = 0;
  QTimer flash_timer_;
  std::function<void(const std::string&)> on_click_;
  std::function<void(const std::string&, bool)> on_lock_request_;
};

// Where a polyline first crosses into `box`, walking from its first point:
// the crossing on the box edge and the unit direction of travel there. A
// pipe need not stop inside the box (a legacy offset can put its end on the
// far edge). Nothing when it starts inside the box or never reaches it.
struct BoxEntry {
  QPointF edge;
  QPointF inward;
};
std::optional<BoxEntry> box_entry(const std::vector<QPointF>& points, const QRectF& box);

// A stage or pipette volume; filled with its network region colour.
class StageItem : public QGraphicsItem {
 public:
  static constexpr double kCornerRadius = 8.0;

  StageItem(std::string name, QString label, canvas::Size size, QColor base, QGraphicsItem* parent = nullptr);

  // A glyph drawn with the name, saying what the volume is: above the name
  // when the box is tall enough, else beside it, at its smallest with the
  // name shrunk to the width left, and left out only when the box is too
  // small for even that. The box itself, and so every pipe joining it, is
  // unchanged.
  canvas::StageSymbol symbol() const noexcept { return symbol_; }
  void set_symbol(canvas::StageSymbol symbol);
  // Where paint() puts the glyph for a label this size; empty when it does not fit.
  QRectF symbol_rect(QSizeF label) const;

  const std::string& name() const noexcept { return name_; }
  // The corner radius of a volume this size: scaled down with the smaller
  // side so thin volumes stay pill-like without swallowing the whole shape.
  static double corner_radius(QSizeF size);
  // How far inside `box`'s straight edge its rounded border runs where a
  // pipe `width` wide crosses at `entry`: 0 along the straight part, up to
  // the corner radius inside a corner.
  static double border_inset(const QRectF& box, const BoxEntry& entry, double width);
  QColor region_color() const noexcept { return region_; }
  void set_region_color(QColor color);

  QRectF boundingRect() const override;
  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

 private:
  std::string name_;
  QString label_;
  QRectF rect_;
  QColor region_;
  canvas::StageSymbol symbol_ = canvas::StageSymbol::None;
};

// Plumbing drawn as a polyline through element centers. Remembers the names
// of the elements it joins so the view can paint it in the colour of the
// network region it belongs to.
//
// A pipe is bordered like every other component, as in legacy pychron. The
// item itself is the fill; the border is outline(), the same path a border
// wider on each side, which the view stacks beneath every pipe's fill so
// pipes that meet (a tee, an elbow's corner) merge with no line between them.
// Where a pipe enters a volume, a gap (add_gap) paints the pipe's fill over
// the volume's border: the border is broken there.
class ConnectionItem : public QGraphicsPathItem {
 public:
  static constexpr double kBorderWidth = 1.0;
  // Stacking: borders, then fills, then (above the volumes at z 1) gaps.
  static constexpr double kOutlineZ = -1.0;
  static constexpr double kFillZ = 0.0;
  static constexpr double kGapZ = 1.5;

  ConnectionItem(const std::vector<QPointF>& points, double width, std::vector<std::string> endpoints,
                 QGraphicsItem* parent = nullptr);

  static QColor default_color();

  const std::vector<std::string>& endpoints() const noexcept { return endpoints_; }
  QColor region_color() const { return pen().color(); }
  void set_region_color(QColor color);

  // Scene-level items the view adds beside this one (the scene owns them).
  QGraphicsPathItem* outline() const noexcept { return outline_; }
  const std::vector<QGraphicsPathItem*>& gaps() const noexcept { return gaps_; }
  // `depth`: how far in from the edge the volume's border lies at its
  // deepest across the pipe's width (more than the border itself only
  // inside a rounded corner; see StageItem::border_inset).
  QGraphicsPathItem* add_gap(const BoxEntry& entry, double depth = 0);

 private:
  std::vector<std::string> endpoints_;
  QGraphicsPathItem* outline_ = nullptr;
  std::vector<QGraphicsPathItem*> gaps_;
};

class LabelItem : public QGraphicsSimpleTextItem {
 public:
  LabelItem(const QString& text, const QString& font_spec, QGraphicsItem* parent = nullptr);
  // "Arial 12" -> family Arial, 12 pt; a bare family or size is accepted.
  static QFont parse_font(const QString& spec);
};

// A gauge: a chip holding a dial, the name and the value + units; red while in alarm.
class GaugeLabelItem : public QGraphicsSimpleTextItem {
 public:
  explicit GaugeLabelItem(std::string name, QGraphicsItem* parent = nullptr);

  const std::string& name() const noexcept { return name_; }
  bool in_alarm() const noexcept { return alarm_; }
  void set_value(double value, const std::string& units);
  void set_alarm(bool alarm);

  // False for a gauge the line does not define: drawn for illustration, its
  // name with no reading.
  bool wired() const noexcept { return wired_; }
  void set_wired(bool wired);

  // A small dial left of the reading marks it as a gauge; both sit on a chip.
  QRectF dial_rect() const;
  QRectF chip_rect() const;
  QRectF boundingRect() const override;
  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

 private:
  void refresh();

  std::string name_;
  QString value_ = QStringLiteral("--");
  bool alarm_ = false;
  bool wired_ = true;
};

}  // namespace pychron::ui
