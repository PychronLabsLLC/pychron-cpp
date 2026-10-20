#pragma once

// ICanvasState: what CanvasView reads to paint itself. The main window's
// canvas reads the CoreBridge's mirror of the line (CoreBridgeCanvasState); a
// rehearsal's canvas reads state a rehearsal keeps, with no bridge and so no
// way to actuate anything.

#include <string>

#include <QDateTime>
#include <QObject>

#include "core_bridge.hpp"

namespace pychron::ui {

class ICanvasState : public QObject {
  Q_OBJECT

 public:
  using QObject::QObject;
  [[nodiscard]] virtual const CoreBridge::State& state() const = 0;
  [[nodiscard]] virtual bool pending(const std::string& name) const = 0;
  [[nodiscard]] virtual QDateTime wall_now() const = 0;
  [[nodiscard]] virtual const canvas::Canvas* canvas() const = 0;
  // The line's network, which colours regions; none and the view leaves
  // valves and pipes uncoloured.
  [[nodiscard]] virtual const systems::NetworkGraph* network() const { return nullptr; }
  // Whether the line defines a gauge of this name. A defined gauge shows its
  // value on the canvas; one that is not is drawn as its name only.
  [[nodiscard]] virtual bool gauge_defined(const std::string& name) const = 0;

 signals:
  // Something the painting reads has changed.
  void changed();
};

// The main canvas's state: the bridge's, re-announced as changed().
class CoreBridgeCanvasState final : public ICanvasState {
  Q_OBJECT

 public:
  explicit CoreBridgeCanvasState(CoreBridge& bridge, QObject* parent = nullptr);

  [[nodiscard]] const CoreBridge::State& state() const override { return bridge_.state(); }
  [[nodiscard]] bool pending(const std::string& name) const override { return bridge_.pending(name); }
  [[nodiscard]] QDateTime wall_now() const override { return bridge_.wall_now(); }
  [[nodiscard]] const canvas::Canvas* canvas() const override { return bridge_.canvas(); }
  [[nodiscard]] const systems::NetworkGraph* network() const override { return bridge_.network(); }
  [[nodiscard]] bool gauge_defined(const std::string& name) const override;

 private:
  CoreBridge& bridge_;
};

}  // namespace pychron::ui
