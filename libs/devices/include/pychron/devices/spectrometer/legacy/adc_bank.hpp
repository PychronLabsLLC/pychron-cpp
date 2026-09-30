#pragma once

// Legacy Faraday ADC bank, config kind "adc_bank": raw volt samples from a
// Modbus TCP ADC (codec::modbus_adc), one float channel per detector, polled
// at `sample_hz`; the host integrates. Values are multiplied by `scale`.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "pychron/devices/spectrometer/legacy/polled_acquirer.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::spectrometer {

class AdcBank final : public PolledAcquirer {
 public:
  struct Options {
    double sample_hz = 100.0;
    std::uint8_t unit = 1;
    std::uint16_t start_register = 0;
    double scale = 1.0;
  };

  AdcBank(std::string name, Transport& transport, std::vector<ChannelId> channels, Options options,
          const Clock* clock = nullptr);

  static DriverSchema schema();
  static Result<std::unique_ptr<AdcBank>> create(const DriverArgs& args);

 protected:
  Result<std::vector<double>> sample() override;

 private:
  Transport& transport_;
  Options options_;
  std::atomic<std::uint16_t> tid_{0};
};

// SimTransport::hooked() hook for the ADC side at `unit`. Channel i (float
// registers start_register + 2i, +1) reads `volts(i)`; registers without a
// value answer Modbus exception 02. Other units and malformed frames get no
// reply.
struct AdcSimModel {
  std::uint8_t unit = 1;
  std::uint16_t start_register = 0;
  std::function<std::optional<double>(std::size_t channel)> volts;
};

SimTransport::Hook adc_sim_hook(AdcSimModel model);

}  // namespace pychron::spectrometer
