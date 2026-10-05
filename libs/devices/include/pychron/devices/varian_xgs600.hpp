#pragma once

// Varian / Agilent XGS-600 gauge controller (config kind "varian_xgs600";
// legacy XGS600GaugeController, ldeo). Speaks codec::varian_xgs600.
//
// The controller reads a sensor by its user label, so the driver lists them:
// `labels = ["CNV1", "IMG1", "HFIG1"]`, and gauge channel n reads labels[n-1]
// (legacy's `channels=` held these labels). Several controllers may share an
// RS-485 bus by `address`.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pychron/codecs/varian_xgs600.hpp"
#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/channel_gauge.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

class VarianXgs600 final : public Device, public IPressureGauge, public IChannelPressureGauge, public IScannable {
 public:
  // `labels` non-empty and valid (create() checks).
  VarianXgs600(std::string name, Transport& transport, std::string address, std::vector<std::string> labels,
               const Clock* clock = nullptr);

  static DriverSchema schema();
  static Result<std::unique_ptr<VarianXgs600>> create(const DriverArgs& args);

  // Channel 1.
  Result<double> read_pressure() override;
  Result<double> read_pressure(int channel) override;
  std::vector<int> pressure_channels() const override;
  Result<Sample> sample() override;

  const std::vector<std::string>& labels() const noexcept { return labels_; }

 private:
  Transport& transport_;
  std::string address_;
  std::vector<std::string> labels_;
  const Clock* clock_;
};

// SimSystem hook contract: answers "#<address>02U<label>" with the pressure
// `pressure(label)` gives (OFF when it gives nullopt), stays silent for
// other addresses, and rejects anything else with "?FF".
struct Xgs600SimModel {
  std::string address = "00";
  std::function<std::optional<double>(const std::string& label)> pressure;
};
SimTransport::Hook xgs600_sim_hook(Xgs600SimModel model);

}  // namespace pychron
