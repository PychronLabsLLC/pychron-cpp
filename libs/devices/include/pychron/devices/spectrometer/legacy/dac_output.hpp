#pragma once

// Generic DAC transport interface for legacy write-only magnet DACs. A
// DacPositioner drives any IDacOutput; each hardware protocol (MAP-215 serial
// today) is one small implementation over a Transport.

#include <functional>
#include <memory>

#include "pychron/core/error.hpp"
#include "pychron/devices/spectrometer/types.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::spectrometer {

struct IDacOutput {
  virtual ~IDacOutput() = default;

  // Volts the output can produce.
  virtual Range span() const = 0;
  // Programs `volts` (inside span(), else Config) and returns the voltage
  // actually output after quantization. Write-only hardware: nothing is read.
  virtual Result<double> write(double volts) = 0;
};

// MAP-215 serial DAC (codec::map215): every write selects `range` then writes
// the code, as one bus transaction.
class Map215Dac final : public IDacOutput {
 public:
  // `range` in 0..9 and `full_scale` > 0 (checked by DacPositioner::create).
  Map215Dac(Transport& transport, int range, double full_scale);

  Range span() const override { return {0.0, full_scale_}; }
  Result<double> write(double volts) override;

 private:
  Transport& transport_;
  int range_;
  double full_scale_;
};

// SimTransport::hooked() hook for a MAP-215 DAC: never replies (the hardware
// is write-only) and reports each output change as volts on a
// 0..`full_scale` output. Unreadable bytes are ignored, as the DAC does.
struct Map215SimModel {
  double full_scale = 10.0;
  std::function<void(int range, double volts)> on_output;
};

SimTransport::Hook map215_sim_hook(Map215SimModel model);

}  // namespace pychron::spectrometer
