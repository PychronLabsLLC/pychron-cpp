#pragma once

// SpectrometerAssembler (spectrometer spec sections 2.2, 7.3): config ->
// transports -> drivers -> role bindings -> Spectrometer.
//
// The config rules of config_validate.hpp run first; binding then checks what
// only live drivers can answer (does the named driver implement the role,
// does the positioner's native axis match, does every configured channel
// exist on the acquirer). All problems are collected into one Config error.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/systems/spectrometer/config.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::spectrometer {

using TransportMaker =
    std::function<Result<std::unique_ptr<Transport>>(const cfg::TransportConfig&, const SpectrometerContext&)>;
using DriverMaker =
    std::function<Result<std::unique_ptr<Device>>(const cfg::DriverConfig&, Transport&, const SpectrometerContext&)>;

struct AssemblerOptions {
  TransportMaker make_transport;  // default: pychron::make_transport
  DriverMaker make_driver;        // default: DriverRegistry::global()
  Spectrometer::Options spectrometer;
};

class SpectrometerAssembler {
 public:
  using Options = AssemblerOptions;

  // load_spectrometer(config_file) then assemble().
  static Result<std::unique_ptr<Spectrometer>> load(const std::filesystem::path& config_file,
                                                    SpectrometerContext context, Options options = {});

  static Result<std::unique_ptr<Spectrometer>> assemble(cfg::SpectrometerData data, SpectrometerContext context,
                                                        Options options = {});

  // Resolves role -> driver bindings over already-built devices (by driver
  // name). Devices are not taken; the caller moves them into the result.
  static Result<SpectrometerRoles> bind(const cfg::SpectrometerConfig& config,
                                        const std::vector<std::pair<std::string, Device*>>& devices);

  static Result<std::unique_ptr<Transport>> default_transport(const cfg::TransportConfig& config,
                                                              const SpectrometerContext& context,
                                                              const std::filesystem::path& trace_dir = "traces");
  static Result<std::unique_ptr<Device>> default_driver(const cfg::DriverConfig& config, Transport& transport,
                                                        const SpectrometerContext& context);
};

}  // namespace pychron::spectrometer
