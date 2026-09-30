#pragma once

// Legacy ion-counting front end, config kind "pulse_counter": raw count
// frames from a serial counter (codec::pulse_counter) polled at `sample_hz`.
// Each frame carries the pulses counted since the previous read; start()
// discards whatever accumulated while stopped. Rate and dead-time correction
// are the host integrator's job.

#include <functional>
#include <memory>

#include "pychron/devices/spectrometer/legacy/polled_acquirer.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::spectrometer {

class PulseCounter final : public PolledAcquirer {
 public:
  PulseCounter(std::string name, Transport& transport, std::vector<ChannelId> channels, double sample_hz = 10.0,
               const Clock* clock = nullptr);

  static DriverSchema schema();
  static Result<std::unique_ptr<PulseCounter>> create(const DriverArgs& args);

 protected:
  Result<std::vector<double>> sample() override;
  Result<void> prime() override;

 private:
  Transport& transport_;
};

// SimTransport::hooked() hook for the counter side: each read answers
// `counts(i)` for channels 0..channels-1; anything else answers "E SYNTAX".
struct CounterSimModel {
  std::size_t channels = 1;
  std::function<std::uint64_t(std::size_t channel)> counts;
};

SimTransport::Hook counter_sim_hook(CounterSimModel model);

}  // namespace pychron::spectrometer
