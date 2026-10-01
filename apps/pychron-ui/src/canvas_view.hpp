#pragma once

// CanvasView: builds the scene from the loaded canvas.toml and keeps it in
// step with the CoreBridge snapshot. Clicking a valve toggles it through
// CoreBridge::actuate; rejections flash the valve. Stages and pipettes are
// coloured by NetworkGraph region (volumes sharing gas share a colour).

#include <functional>
#include <map>
#include <string>

#include <QColor>
#include <QGraphicsScene>
#include <QGraphicsView>

#include "canvas_items.hpp"
#include "core_bridge.hpp"

namespace pychron::ui {

class CanvasView : public QGraphicsView {
  Q_OBJECT

 public:
  explicit CanvasView(CoreBridge& bridge, QWidget* parent = nullptr);

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

  // Colour of an isolated volume.
  static QColor isolated_color();

 private:
  void build(const canvas::Canvas& canvas);
  void add_path(const std::vector<std::string>& names, double width);
  ConnectionItem* add_pipe(const std::vector<QPointF>& points, double width, std::vector<std::string> endpoints);
  bool position(const std::string& name, QPointF& out) const;

  void on_click(const std::string& name);
  void apply_state();
  void apply_regions();
  void on_pressure(const PressureSample& sample);
  void on_alarm(const Alarm& alarm);
  void on_finished(const QString& name, const Result<void>& result);

  CoreBridge& bridge_;
  std::function<bool(const QString&)> confirm_unlock_;
  QGraphicsScene scene_;
  std::map<std::string, QPointF> positions_;
  std::map<std::string, ValveItem*> valves_;
  std::map<std::string, StageItem*> stages_;
  std::map<std::string, GaugeLabelItem*> gauges_;
  std::vector<ConnectionItem*> pipes_;
  int connections_ = 0;
};

}  // namespace pychron::ui
