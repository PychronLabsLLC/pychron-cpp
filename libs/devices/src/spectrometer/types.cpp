#include "pychron/devices/spectrometer/types.hpp"

namespace pychron::spectrometer {

std::optional<double> Frame::value(const ChannelId& channel) const {
  for (const auto& [id, v] : values) {
    if (id == channel) return v;
  }
  return std::nullopt;
}

std::string to_string(DetectorCap cap) {
  switch (cap) {
    case DetectorCap::Gain: return "gain";
    case DetectorCap::Deflection: return "deflection";
    case DetectorCap::Protect: return "protect";
    case DetectorCap::CddVoltage: return "cdd_voltage";
  }
  return "unknown";
}

std::string to_string(Caps caps) {
  std::string out;
  for (auto cap : {DetectorCap::Gain, DetectorCap::Deflection, DetectorCap::Protect,
                   DetectorCap::CddVoltage}) {
    if (!caps.has(cap)) continue;
    if (!out.empty()) out += '|';
    out += to_string(cap);
  }
  return out;
}

}  // namespace pychron::spectrometer
