#include "canvas_state.hpp"

namespace pychron::ui {

CoreBridgeCanvasState::CoreBridgeCanvasState(CoreBridge& bridge, QObject* parent)
    : ICanvasState(parent), bridge_(bridge) {
  connect(&bridge_, &CoreBridge::snapshot, this, [this](const Snapshot&) { emit changed(); });
  connect(&bridge_, &CoreBridge::valveChanged, this, [this](const ValveChanged&) { emit changed(); });
  connect(&bridge_, &CoreBridge::lockChanged, this, [this](const QString&, bool) { emit changed(); });
  connect(&bridge_, &CoreBridge::actuationFailed, this, [this](const ActuationFailed&) { emit changed(); });
}

}  // namespace pychron::ui
