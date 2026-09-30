#pragma once

// Legacy magnet positioner, config kind "dac_positioner": a write-only DAC
// whose read() is the cached last-written (quantized) value. The DAC powers
// up at 0 V, so before the first set() read() reports the limit nearest 0.

#include <atomic>
#include <memory>
#include <string>

#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/legacy/dac_output.hpp"
#include "pychron/devices/spectrometer/roles.hpp"

namespace pychron::spectrometer {

class DacPositioner final : public Device, public IMassPositioner {
 public:
  // `limits` must be a non-empty range inside output->span() (create()
  // checks).
  DacPositioner(std::string name, std::unique_ptr<IDacOutput> output, Limits limits, const Clock* clock = nullptr);

  static DriverSchema schema();
  static Result<std::unique_ptr<DacPositioner>> create(const DriverArgs& args);

  Axis native_axis() const override { return Axis::Dac; }
  Result<void> set(double value) override;
  Result<double> read() override;
  Limits limits() const override { return limits_; }

 private:
  std::unique_ptr<IDacOutput> output_;
  Limits limits_;
  std::atomic<double> cached_;
};

}  // namespace pychron::spectrometer
