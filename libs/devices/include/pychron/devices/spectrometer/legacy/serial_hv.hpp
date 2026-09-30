#pragma once

// Legacy ion-source HV supply, config kind "serial_hv": an IBeamSource that
// supports HV only (codec::hv_supply). read_hv() is the measured output;
// read_param(HV) reports {programmed setpoint, measured output}.

#include <functional>
#include <memory>
#include <vector>

#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/roles.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::spectrometer {

class SerialHv final : public Device, public IBeamSource {
 public:
  // `max_hv` > 0 (create() checks); setpoints outside 0..max_hv are Config.
  SerialHv(std::string name, Transport& transport, double max_hv = 10000.0, const Clock* clock = nullptr);

  static DriverSchema schema();
  static Result<std::unique_ptr<SerialHv>> create(const DriverArgs& args);

  Result<void> set_hv(double volts) override;
  Result<double> read_hv() override;
  std::span<const ParamSpec> params() const override { return params_; }
  Result<void> set_param(const ParamId& id, double value) override;
  Result<Readback> read_param(const ParamId& id) override;

 private:
  Transport& transport_;
  std::vector<ParamSpec> params_;
};

// SimTransport::hooked() hook for the supply side. The output follows the
// setpoint through `output` (identity when unset); `on_set` sees each new
// setpoint. Setpoints above `max_hv` answer "ERR RANGE".
struct HvSimModel {
  double initial = 0.0;
  double max_hv = 10000.0;
  std::function<void(double setpoint)> on_set;
  std::function<double(double setpoint)> output;
};

SimTransport::Hook hv_sim_hook(HvSimModel model);

}  // namespace pychron::spectrometer
