#include "pychron/devices/spectrometer/roles.hpp"

namespace pychron::spectrometer {

Error unsupported(DetectorCap cap) {
  return Error{ErrorKind::Config, to_string(cap) + " not supported", {}};
}

Result<void> IDetectorControl::set_deflection(const ChannelId&, double) {
  return fail(unsupported(DetectorCap::Deflection));
}

Result<double> IDetectorControl::read_deflection(const ChannelId&) {
  return fail(unsupported(DetectorCap::Deflection));
}

Result<void> IDetectorControl::set_gain(const ChannelId&, double) {
  return fail(unsupported(DetectorCap::Gain));
}

Result<double> IDetectorControl::read_gain(const ChannelId&) {
  return fail(unsupported(DetectorCap::Gain));
}

Result<void> IDetectorControl::set_cdd_voltage(const ChannelId&, double) {
  return fail(unsupported(DetectorCap::CddVoltage));
}

std::string_view to_string(IMassPositioner::Axis axis) noexcept {
  switch (axis) {
    case IMassPositioner::Axis::Dac: return "dac";
    case IMassPositioner::Axis::Field: return "field";
    case IMassPositioner::Axis::Mass: return "mass";
  }
  return "dac";
}

Result<IMassPositioner::Axis> parse_axis(std::string_view text) {
  for (auto axis : {IMassPositioner::Axis::Dac, IMassPositioner::Axis::Field,
                    IMassPositioner::Axis::Mass}) {
    if (to_string(axis) == text) return axis;
  }
  return fail(ErrorKind::Config, "unknown positioner axis '" + std::string(text) + "'");
}

}  // namespace pychron::spectrometer
