#include "canvas_state.hpp"

#include <algorithm>

namespace pychron::ui {

CoreBridgeCanvasState::CoreBridgeCanvasState(CoreBridge& bridge, QObject* parent)
    : ICanvasState(parent), bridge_(bridge) {
  connect(&bridge_, &CoreBridge::snapshot, this, [this](const Snapshot&) { emit changed(); });
  connect(&bridge_, &CoreBridge::valveChanged, this, [this](const ValveChanged&) { emit changed(); });
  connect(&bridge_, &CoreBridge::lockChanged, this, [this](const QString&, bool) { emit changed(); });
  connect(&bridge_, &CoreBridge::actuationFailed, this, [this](const ActuationFailed&) { emit changed(); });
}

bool CoreBridgeCanvasState::gauge_defined(const std::string& name) const {
  const auto& defined = bridge_.config().gauges;
  return std::any_of(defined.begin(), defined.end(), [&](const auto& d) { return d.name == name; });
}

}  // namespace pychron::ui
