#pragma once

// Stateful simulated Qtegra RemoteControlServer for SimTransport::hooked(),
// so tests run the real QtegraSpectrometer against a small in-memory model.

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "pychron/core/clock.hpp"
#include "pychron/transport/sim_transport.hpp"

namespace pychron::spectrometer {

// The hook locks `mutex` for every command; tests lock it to read or change
// the model while a driver may be running.
struct QtegraSimModel {
  std::mutex mutex;

  double dac = 0.0;
  // SetMagnetDAC sets moving_until = clock->now() + move_time; GetMagnetMoving
  // is true until the clock reaches it (never, without a clock).
  TimePoint moving_until{};
  Duration move_time{};
  bool blank = false;
  std::map<std::string, bool> protect;  // by detector name
  std::map<std::string, double> deflection, gain;
  double hv = 0.0;
  std::map<std::string, double> params;  // by hardware name
  double integration_s = 1.048576;
  std::map<std::string, double> intensities;  // by detector name
  std::string data_override;                  // when non-empty, GetData replies with this verbatim
  const Clock* clock = nullptr;
};

// Commands it does not know, or cannot parse, answer "ERROR: ...".
SimTransport::Hook qtegra_sim_hook(std::shared_ptr<QtegraSimModel> model);

}  // namespace pychron::spectrometer
