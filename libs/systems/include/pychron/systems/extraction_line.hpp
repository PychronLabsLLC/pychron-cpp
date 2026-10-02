#pragma once

// ExtractionLine facade (spec section 9.2): the only entry point for elctl
// and the UI. Loads `extraction_line.toml` and, optionally, `canvas.toml`;
// builds transports -> drivers -> managers sharing one Clock, SignalBus and
// Scheduler; start()/stop() the line.
//
// `kind = "sim"` transports (or every transport, with force_sim) are hooked
// to one SimSystem whose topology comes from the canvas NetworkGraph, so the
// identical code path runs with no hardware. The hook on a sim transport is
// chosen by the first driver configured on it.
//
// start():
//   1. opens every transport (all-or-nothing: on any failure all are closed
//      again and an Io error lists each failure)
//   2. reads every actuated switch back (SwitchManager::refresh); read
//      errors are published as Log warnings, not fatal
//   3. reads every gauge once and registers it with the GaugeScanner at
//      [system].scan_interval_ms
//   4. publishes a full Snapshot so subscribers paint before the first scan
//   5. starts the scheduler dispatcher (unless Options::run_scheduler is
//      false, in which case the caller drives scheduler().run_pending())
//
// Events leave only through bus(). Commands (actuate, read_gauge) block the
// caller for the transport round trips and valve settle time.

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/diagnostic.hpp"
#include "pychron/core/config/system_config.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/events.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/systems/canvas/canvas.hpp"
#include "pychron/systems/gauge_scanner.hpp"
#include "pychron/systems/network_graph.hpp"
#include "pychron/systems/switch_manager.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::systems {

struct ExtractionLineOptions {
  const Clock* clock = nullptr;  // SteadyClock if null; must outlive the line
  bool force_sim = false;        // treat every transport as kind = "sim"
  std::set<std::string> trace;   // transports to record, on top of config `trace = true`
  std::filesystem::path trace_dir = "traces";
  sim::SimSettings sim;          // initial pressures, pumps, noise for the SimSystem
  Scheduler::Options scheduler;
  bool run_scheduler = true;     // false: caller drives scheduler().run_pending()
  // Optional override. When null the line creates one from [logging]. Every
  // transport mirrors its bytes to "<name>.wire" on it, and the line's own
  // records go to "extraction_line" and "scheduler"; the line shares
  // ownership, so the hub outlives its transports.
  std::shared_ptr<LogHub> log_hub;
};

class ExtractionLine {
 public:
  using Options = ExtractionLineOptions;

  // Loads both files (the canvas is optional) and create()s the line.
  static Result<std::unique_ptr<ExtractionLine>> load(const std::filesystem::path& system_file,
                                                      const std::optional<std::filesystem::path>& canvas_file,
                                                      Options options = {});

  // Cross-validates the canvas against the config (errors fail, warnings
  // are kept in warnings()) and builds everything, closed and idle.
  static Result<std::unique_ptr<ExtractionLine>> create(config::SystemConfig config,
                                                        std::optional<canvas::Canvas> canvas,
                                                        Options options = {});

  ~ExtractionLine();
  ExtractionLine(const ExtractionLine&) = delete;
  ExtractionLine& operator=(const ExtractionLine&) = delete;

  // See the header comment. Starting a running line is a no-op.
  Result<void> start();
  // Stops scans, drains in-flight work and closes every transport. Idempotent.
  void stop();
  bool running() const;

  Result<void> actuate(std::string_view name, SwitchOp op, std::string_view actor = {});
  // One reading, in the gauge's configured units. Config error if unknown.
  Result<double> read_gauge(std::string_view name);
  // Recorded valve states and the latest pressure of every gauge read so far.
  Snapshot snapshot() const;

  SignalBus& bus() noexcept { return bus_; }
  // Options::log_hub, or the hub built from [logging]; null only if that failed.
  std::shared_ptr<LogHub> log_hub() const noexcept { return log_hub_; }
  const Clock& clock() const noexcept { return *clock_; }
  Scheduler& scheduler() noexcept { return *scheduler_; }
  SwitchManager& switches() noexcept { return *switches_; }
  const config::SystemConfig& config() const noexcept { return config_; }
  const canvas::Canvas* canvas() const noexcept { return canvas_ ? &*canvas_ : nullptr; }
  const NetworkGraph* network() const noexcept { return network_ ? &*network_ : nullptr; }
  // Canvas cross-validation warnings (system valves missing from the canvas).
  const std::vector<config::Diagnostic>& warnings() const noexcept { return warnings_; }
  // The lab model behind sim transports; null when no transport is sim.
  sim::SimSystem* sim() noexcept { return sim_.get(); }

  Transport* transport(std::string_view name) const;
  Device* device(std::string_view name) const;

 private:
  ExtractionLine(config::SystemConfig config, std::optional<canvas::Canvas> canvas, Options options);

  Result<void> build();
  void read_all_gauges();
  void record_pressure(const std::string& gauge, double value);
  void log(LogLevel level, std::string message);

  config::SystemConfig config_;
  std::optional<canvas::Canvas> canvas_;
  std::optional<NetworkGraph> network_;
  std::vector<config::Diagnostic> warnings_;
  Options options_;

  // Declaration order is construction order; teardown runs in reverse, so
  // the scanner and scheduler stop before the drivers they call, drivers go
  // before their transports, and transports before the sim models their
  // hooks borrow.
  SteadyClock steady_clock_;
  const Clock* clock_;
  SignalBus bus_;
  // After the bus, before the transports: destroyed after them.
  std::shared_ptr<LogHub> log_hub_;
  // "extraction_line" on log_hub_; null without a hub, when log() publishes
  // straight on bus_.
  std::optional<Logger> logger_;
  std::unique_ptr<sim::SimSystem> sim_;
  std::vector<std::pair<std::string, std::unique_ptr<Transport>>> transports_;
  std::vector<std::pair<std::string, std::unique_ptr<Device>>> devices_;
  std::unique_ptr<SwitchManager> switches_;
  std::unique_ptr<Scheduler> scheduler_;
  std::unique_ptr<GaugeScanner> scanner_;
  std::vector<SignalBus::Subscription> subscriptions_;

  mutable std::mutex lifecycle_;  // start()/stop()
  bool running_ = false;
  mutable std::mutex pressures_mutex_;
  std::map<std::string, double> pressures_;
};

}  // namespace pychron::systems
