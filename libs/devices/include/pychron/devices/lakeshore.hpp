#pragma once

// Lake Shore 325/331/335/336 temperature controller (config kind
// "lakeshore"; legacy Model335TemperatureController and siblings, ldeo).
// Speaks codec::lakeshore.
//
// set_setpoint(output, K): when [[drivers.<name>.ranges]] gives bands for
// that output, the band holding the setpoint picks the heater range (bands
// are [min, max), the highest one closed; a setpoint in no band is a Config
// error and nothing is sent; range 0, heater off, may be a band). With no
// bands the range is left as it is. Then SETP, then SETP? until it reads back
// within `setpoint_tolerance`, resending at most `verify_retries` times.
// Legacy compared floats exactly, skipped a repeat of the same setpoint, and
// left gaps at 10 K and 30 K in its default bands; none of that here.
//
// connect() clears the status and checks *IDN? names a Lake Shore of the
// configured model.

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pychron/codecs/lakeshore.hpp"
#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/connectable.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/temperature_controller.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

struct LakeshoreBand {
  int output = 1;
  int range = 0;
  double min_k = 0.0;
  double max_k = 0.0;
};

struct LakeshoreOptions {
  std::string model = "335";  // 325, 331, 335 or 336
  std::vector<std::string> inputs{"A", "B"};
  codec::lakeshore::Units units = codec::lakeshore::Units::Kelvin;
  std::vector<LakeshoreBand> bands;
  double setpoint_tolerance = 0.01;  // K
  int verify_retries = 3;
};

class Lakeshore final : public Device, public IConnectable, public ITemperatureController, public IScannable {
 public:
  Lakeshore(std::string name, Transport& transport, LakeshoreOptions options, const Clock* clock = nullptr);

  static DriverSchema schema();
  static Result<std::unique_ptr<Lakeshore>> create(const DriverArgs& args);

  Result<void> connect() override;

  std::vector<std::string> inputs() const override { return options_.inputs; }
  Result<double> read_temperature(std::string_view input) override;
  int outputs() const override;
  Result<void> set_setpoint(int output, double kelvin) override;
  Result<double> setpoint(int output) override;

  // The first input, for the generic scan.
  Result<Sample> sample() override;

  // The band's heater range for `kelvin` on `output`; nullopt when the output
  // has no bands; Config error when it has bands and none holds the value.
  Result<std::optional<int>> range_for(int output, double kelvin) const;

 private:
  Result<void> check_output(int output) const;
  double to_wire(double kelvin) const;
  double from_wire(double value) const;
  Result<Bytes> ask(const codec::Command& command);

  Transport& transport_;
  LakeshoreOptions options_;
  const Clock* clock_;
};

// A simulated Lake Shore for SimSystem and tests: each input i follows output
// i's setpoint while that output's heater range is above 0, and falls back to
// `base_k` while it is 0, as a first-order lag with time constant `tau` on the
// injected clock. Answers *IDN?, *CLS, KRDG?/CRDG?, SETP, SETP?, RANGE,
// RANGE?; anything else is ignored, as the unit does.
class LakeshoreSim {
 public:
  LakeshoreSim(const Clock& clock, std::string model = "MODEL335", double start_k = 293.15, double base_k = 10.0,
               Duration tau = std::chrono::seconds(60));
  LakeshoreSim(const LakeshoreSim&) = delete;
  LakeshoreSim& operator=(const LakeshoreSim&) = delete;

  Bytes respond(const Bytes& tx);
  SimTransport::Hook hook();  // the sim must outlive the transport

  double temperature(char input) const;
  void set_temperature(char input, double kelvin);
  std::optional<double> setpoint(int output) const;
  int range(int output) const;
  // When set, SETP stores this much off what was asked (a unit that rounds).
  void set_setpoint_error(double kelvin);

 private:
  void advance_locked();

  const Clock& clock_;
  std::string model_;
  double base_k_;
  Duration tau_;
  mutable std::mutex mutex_;
  TimePoint last_{};
  std::map<char, double> temps_;
  std::map<int, double> setpoints_;
  std::map<int, int> ranges_;
  double setpoint_error_ = 0.0;
};

}  // namespace pychron
