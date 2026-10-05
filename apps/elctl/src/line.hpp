#pragma once

// Assembles one extraction line from its config: transports -> drivers ->
// SwitchManager + GaugeScanner, sharing one Clock, SignalBus and Scheduler.
//
// This is elctl's stand-in for the ExtractionLine facade (spec 9.2, work
// step 10), which has not landed yet; once it does, elctl should build that
// instead and this file goes away.
//
// `kind = "sim"` transports (or every transport, with force_sim) are wired to
// minimal stateless-per-process device models chosen by the kind of driver
// on the transport: a ProxrBoardSim for `proxr_relay`, a fixed-pressure
// MaxiGauge for `pfeiffer_maxigauge`, a Plc2000HeaterSim for
// `plc2000_heater`. SimSystem replaces these later.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/system_config.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/heater.hpp"
#include "pychron/systems/gauge_scanner.hpp"
#include "pychron/systems/switch_manager.hpp"
#include "pychron/transport/transport.hpp"
#include "trace_settings.hpp"

namespace elctl {

struct LineOptions {
  bool force_sim = false;  // treat every transport as kind = "sim"
  TraceSettings trace;     // transports to record, on top of config `trace = true`
  std::filesystem::path trace_dir = "traces";
};

class Line {
 public:
  static pychron::Result<std::unique_ptr<Line>> build(pychron::config::SystemConfig config, LineOptions options = {});

  ~Line();
  Line(const Line&) = delete;
  Line& operator=(const Line&) = delete;

  struct Opened {
    std::string transport;
    pychron::Result<void> result;
  };
  // Opens every transport, in config order, and reports each outcome.
  std::vector<Opened> open_all();
  void close_all();

  const pychron::config::SystemConfig& config() const noexcept { return config_; }
  pychron::SignalBus& bus() noexcept { return bus_; }
  const pychron::Clock& clock() const noexcept { return clock_; }

  pychron::Transport* transport(const std::string& name) const;
  pychron::Device* device(const std::string& name) const;
  pychron::systems::SwitchManager& switches() noexcept { return *switches_; }

  const pychron::config::GaugeConfig* gauge(const std::string& name) const;
  // One reading, in the gauge's configured units.
  pychron::Result<double> read_gauge(const std::string& name);

  const pychron::config::HeaterConfig* heater_config(const std::string& name) const;
  // Config error for an unknown heater or a driver that is not one.
  pychron::Result<pychron::IHeater*> heater(const std::string& name) const;

  // Registers every gauge with the scanner at `interval` and starts the
  // scheduler; PressureSample/Alarm arrive on bus(). stop_scan() undoes it.
  pychron::Result<void> start_scan(pychron::Duration interval);
  void stop_scan();

 private:
  struct Sims;
  Line(pychron::config::SystemConfig config);

  pychron::config::SystemConfig config_;
  // Declaration order is teardown order in reverse: the scanner and
  // scheduler stop before the drivers they call, drivers go before their
  // transports, and transports before the sim models their hooks borrow.
  pychron::SteadyClock clock_;
  pychron::SignalBus bus_;
  // After the bus and before the transports: destroyed after the transports
  // that log through it. No [logging].dir means no file sink.
  std::shared_ptr<pychron::LogHub> log_hub_;
  std::unique_ptr<Sims> sims_;
  std::vector<std::pair<std::string, std::unique_ptr<pychron::Transport>>> transports_;
  std::vector<std::pair<std::string, std::unique_ptr<pychron::Device>>> devices_;
  std::unique_ptr<pychron::systems::SwitchManager> switches_;
  std::unique_ptr<pychron::Scheduler> scheduler_;
  std::unique_ptr<pychron::GaugeScanner> scanner_;
};

}  // namespace elctl
