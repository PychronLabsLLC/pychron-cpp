#pragma once

// CanvasView: builds the scene from the loaded canvas.toml and keeps it in
// step with the CoreBridge snapshot. Clicking a valve toggles it through
// CoreBridge::actuate; rejections flash the valve. Clicking a gauge the line
// defines, or resting the pointer on it, is said by a signal; what follows is
// the window's. Stages and pipettes are
// coloured by NetworkGraph region (volumes sharing gas share a colour).
// The drawing never overflows the view: it is scaled, either way, to fit the
// view whatever its size, with no scroll bars and nothing to pan.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QColor>
#include <QDateTime>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QRectF>
#include <QStringList>
#include <QTimer>

#include "canvas_items.hpp"
#include "canvas_state.hpp"
#include "core_bridge.hpp"

namespace pychron::ui {

class CanvasView : public QGraphicsView {
  Q_OBJECT

 public:
  // How long the pointer stays on a gauge before gauge_rested: Qt's own
  // tooltip wake-up delay (QStyle::SH_ToolTip_WakeUpDelay), long enough that
  // a pointer passing over a chip opens nothing.
  static constexpr int kGaugeRestMs = 700;
  // Scene units left around the drawn items when the drawing is fitted to the view.
  static constexpr double kContentMargin = 20;

  // Over the bridge: its own adapter, owned by the view.
  explicit CanvasView(CoreBridge& bridge, QWidget* parent = nullptr);
  // Paints from `state` (which must outlive the view). With a null `bridge`
  // the view is read-only: clicks and lock requests do nothing, and nothing
  // here has a path to an actuator.
  CanvasView(ICanvasState& state, CoreBridge* bridge, QWidget* parent = nullptr);

  [[nodiscard]] bool read_only() const noexcept { return bridge_ == nullptr; }

  void set_gauge_rest_ms(int ms);  // tests: 0
  [[nodiscard]] int gauge_rest_ms() const noexcept { return gauge_rest_timer_.interval(); }

  ValveItem* valve(const std::string& name) const;
  StageItem* stage(const std::string& name) const;  // stages and pipettes
  GaugeLabelItem* gauge(const std::string& name) const;
  int connection_count() const noexcept { return connections_; }
  const std::vector<ConnectionItem*>& pipes() const noexcept { return pipes_; }

  // Locks or unlocks a valve or switch through the bridge. Unlocking asks the
  // confirmation callback first (default: a Yes/No dialog); declining, a
  // manual valve or an unknown name returns false without touching the core.
  // A core error flashes the valve with its reason.
  bool request_lock(const std::string& name, bool locked);
  void set_confirm_unlock(std::function<bool(const QString& name)> confirm) { confirm_unlock_ = std::move(confirm); }

  // Initialised from canvas.toml's open_valve_color; changing it repaints.
  canvas::OpenValveColor open_valve_color() const noexcept { return open_valve_color_; }
  void set_open_valve_color(canvas::OpenValveColor mode);

  // The lines of a valve's tooltip under its name, as of `now`: description,
  // state and how long it has held, last actuation, counts and time open.
  static QStringList valve_details(const CoreBridge::State& state, const std::string& name, const QDateTime& now);

  // Colour of an isolated volume.
  static QColor isolated_color();
  // The canvas file's `[colors] background`, if it names a colour; without
  // one the view keeps the theme's own.
  static std::optional<QColor> background_color(const canvas::Canvas& c);
  // The theme's colour for a source (a region takes its source's). Tanks
  // differ by `ordinal`, their place among the canvas's tanks; the other
  // kinds have one colour each.
  static QColor source_color(canvas::SourceKind kind, int ordinal = 0);

 signals:
  void gauge_clicked(const QString& name);
  // The pointer stayed on the gauge for the delay; `global_chip` is its chip
  // on the screen.
  void gauge_rested(const QString& name, const QRect& global_chip);
  // After a rest: the pointer moved off, the view scrolled or was fitted
  // again (a resize), or a button went down or a wheel turned over the view.
  void gauge_left();

 protected:
  void scrollContentsBy(int dx, int dy) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void build(const canvas::Canvas& canvas);
  // Scales the drawing to fill the view (while shown); ends a rest.
  void fit();
  // Widens the scene to the drawn items, plus the margin, when they have
  // grown past it (a gauge's chip widens with its first reading); never
  // narrows it, so the drawing does not jump as readings change.
  void grow_content();
  void on_gauge_hover(const std::string& name, bool entered);
  void on_gauge_rest();
  // Stops a pending rest; after one that was said, says gauge_left.
  void end_gauge_rest();
  void add_path(const std::vector<std::string>& names, double width);
  ConnectionItem* add_pipe(const std::vector<QPointF>& points, double width, std::vector<std::string> endpoints);
  bool position(const std::string& name, QPointF& out) const;
  // A connection with an orientation: straight into a box it lines up with,
  // else one corner (vertical first for "v", horizontal first for "h").
  std::vector<QPointF> oriented(QPointF a, const std::string& a_name, QPointF b, const std::string& b_name,
                                bool vertical) const;

  void on_click(const std::string& name);
  void apply_state();
  void apply_regions();
  void on_pressure(const PressureSample& sample);
  void on_alarm(const Alarm& alarm);
  void on_finished(const QString& name, const Result<void>& result);

  std::unique_ptr<ICanvasState> own_state_;  // set when built over a bridge alone
  ICanvasState& state_;
  CoreBridge* bridge_;  // null: read-only
  std::function<bool(const QString&)> confirm_unlock_;
  canvas::OpenValveColor open_valve_color_ = canvas::OpenValveColor::Green;
  QGraphicsScene scene_;
  std::map<std::string, QPointF> positions_;
  std::map<std::string, ValveItem*> valves_;
  std::map<std::string, StageItem*> stages_;
  std::map<std::string, QRectF> boxes_;  // stages and pipettes, scene coordinates
  std::map<std::string, GaugeLabelItem*> gauges_;
  std::map<std::string, canvas::Source, std::less<>> sources_;  // what colours the region it is in
  std::vector<ConnectionItem*> pipes_;
  int connections_ = 0;
  QTimer gauge_rest_timer_;
  std::string gauge_under_;  // the wired gauge the pointer is on, if any
  bool gauge_rested_ = false;
};

}  // namespace pychron::ui
