#pragma once

// Gauge readbacks through Thermo Qtegra RemoteControl (config kind
// "qtegra_gauges"; legacy QtegraGaugeController, ldeo and usgsdenver).
// `parameters = ["Ion Gauge MS Readback", ...]` lists Qtegra parameter
// names; gauge channel n reads parameters[n-1] with GetParameter.
//
// Like qtegra_valves it shares thermo_qtegra's connection on a
// kind = "link" transport, or owns one on its own tcp or udp transport.
// A reply that is not a non-negative number, an ERROR among them, is a
// Protocol error; legacy kept its last value instead.

#include <memory>
#include <string>
#include <vector>

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/channel_gauge.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/qtegra_link.hpp"

namespace pychron::spectrometer {

class QtegraGauges final : public Device, public IPressureGauge, public IChannelPressureGauge, public IScannable {
 public:
  QtegraGauges(std::string name, QtegraLinkHandle link, std::vector<std::string> parameters,
               const Clock* clock = nullptr);

  static DriverSchema schema();
  static Result<std::unique_ptr<QtegraGauges>> create(const DriverArgs& args);

  Result<double> read_pressure() override;  // channel 1
  Result<double> read_pressure(int channel) override;
  std::vector<int> pressure_channels() const override;
  Result<Sample> sample() override;

 private:
  QtegraLinkHandle link_;
  std::vector<std::string> parameters_;
  const Clock* clock_;
};

}  // namespace pychron::spectrometer
