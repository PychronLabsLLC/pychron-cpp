#pragma once

// SimSystem: stateful model of the physical lab behind `kind = "sim"`
// transports (spec section 7).
//
// One instance serves every driver of a config, so cross-device causality
// emerges naturally: a relay command reaching the modelled ProXR board opens
// a valve here, gas equilibrates across the now-connected volumes, and the
// modelled MaxiGauge reports the new pressure.
//
// Physics, deliberately simple:
//   - Volumes and valves form a graph (edges as on the canvas). A valve
//     conducts only while open. Volumes joined through open valves or direct
//     edges form a region that equilibrates instantly to its
//     volume-weighted mean pressure.
//   - A region containing a pumped volume decays toward the lowest pump base
//     pressure with the shortest pump time constant:
//       p(t + dt) = base + (p(t) - base) * exp(-dt / tau)
//   - Gauge readings add Gaussian noise (relative, `Settings::noise`) from a
//     seeded generator.
// Topology is constant between valve events, so advancing lazily to
// clock.now() on every query is exact.
//
// This library depends on transport/devices/core only, so the topology is a
// plain description; the ExtractionLine facade fills it from its
// NetworkGraph. Time comes from the injected Clock; tests advance a
// ManualClock. All methods are thread-safe: hooks run on transport workers.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/system_config.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/agilent_unit_sim.hpp"
#include "pychron/devices/lakeshore.hpp"
#include "pychron/devices/extraction/chromium_sim.hpp"
#include "pychron/devices/pychron_valve_server_sim.hpp"
#include "pychron/devices/proxr_board_sim.hpp"
#include "pychron/transport/sim_transport.hpp"

namespace pychron::sim {

struct SimVolume {
  std::string name;
  double cc = 1.0;  // relative size; only ratios matter
};

struct SimTopology {
  std::vector<SimVolume> volumes;
  std::vector<std::string> valves;
  // Undirected; endpoints name volumes or valves. Edges naming unknown
  // nodes are ignored.
  std::vector<std::pair<std::string, std::string>> edges;
};

struct SimPump {
  double base = 1e-9;  // ultimate pressure
  Duration tau = std::chrono::seconds(5);
};

struct SimSettings {
  double default_pressure = 1e-8;  // volumes not listed below
  std::map<std::string, double> initial_pressures;
  std::map<std::string, SimPump> pumps;  // keyed by volume name
  double noise = 0.01;                   // relative 1-sigma gauge noise
  std::uint64_t seed = 0x5eed;
};

class SimSystem {
 public:
  using Volume = SimVolume;
  using Topology = SimTopology;
  using Pump = SimPump;
  using Settings = SimSettings;

  SimSystem(const Clock& clock, Topology topology, Settings settings = {});
  ~SimSystem();
  SimSystem(const SimSystem&) = delete;
  SimSystem& operator=(const SimSystem&) = delete;

  // Advances the model to now, then changes the valve. Names outside the
  // topology (switches, unmodelled valves) are tracked with no physics.
  void set_valve(std::string_view name, bool open);
  bool valve_open(std::string_view name) const;

  // Model pressure of a volume, advanced to now. Config error if unknown.
  Result<double> pressure(std::string_view volume) const;
  // Sets one volume's pressure (e.g. gas released into a stage). The region
  // re-equilibrates on the next advance.
  Result<void> set_pressure(std::string_view volume, double value);
  // pressure() with gauge noise applied.
  Result<double> gauge_reading(std::string_view volume) const;

  bool has_volume(std::string_view name) const;

  // Hook for a SimTransport answering as `driver` would, given the system
  // config it belongs to:
  //   proxr_relay         a ProxrBoardSim; relay commands for addresses of
  //                       valves/switches on this actuator drive set_valve().
  //   agilent_switch      an AgilentUnitSim; route commands for channels of
  //                       valves/switches on this unit drive set_valve(),
  //                       honouring the unit's `invert` and each valve's.
  //   qtegra_valves       a Qtegra answering valve commands; Open/Close of a
  //                       valve's Qtegra name drive set_valve().
  //   qtegra_gauges       a Qtegra whose parameters[n-1] reads the volume of
  //                       the gauge on channel n.
  //   pychron_valves      another Pychron's valve service serving the line's
  //                       valve addresses; Open/Close drive set_valve().
  //   pfeiffer_maxigauge  channel n reads the volume named after the gauge
  //                       configured on that channel (added as an isolated
  //                       volume if the topology lacks it); other channels
  //                       have no sensor.
  //   lakeshore           a LakeshoreSim on this system's clock.
  //   plc2000_gauges      a Modbus PLC whose float at channel n's registers
  //                       is the volume of the gauge on that channel.
  //   varian_xgs600       label n of the driver's `labels` reads the volume
  //                       of the gauge on channel n, as for maxigauge.
  //   chromium            a ChromiumSim on this system's clock: the laser
  //                       PC, with its stage, output and interlocks.
  //   anything else       a silent wire (the driver sees timeouts).
  // Models built here live as long as this SimSystem, which must outlive
  // the transport.
  SimTransport::Hook hook_for(const config::DriverConfig& driver, const config::SystemConfig& system);

  // The Chromium simulator hook_for built for the driver named `driver`;
  // null if there is none. For tests and tools that look at what the
  // simulated laser was told.
  extraction::ChromiumSim* chromium(std::string_view driver) const;

 private:
  struct Node {
    bool valve = false;
    std::set<std::string> edges;
  };

  // Moves the model to clock.now(); with no elapsed time it only equilibrates.
  void advance_locked() const;
  // Volume sets joined through open valves.
  std::vector<std::vector<std::string>> regions_locked() const;
  void add_volume_locked(const std::string& name, double cc);

  const Clock& clock_;
  Settings settings_;
  mutable std::mutex mutex_;
  std::map<std::string, Node, std::less<>> nodes_;
  std::map<std::string, double, std::less<>> cc_;
  mutable std::map<std::string, double, std::less<>> pressure_;
  std::map<std::string, bool, std::less<>> valve_open_;
  mutable TimePoint last_{};
  mutable std::mt19937_64 rng_;
  std::vector<std::unique_ptr<ProxrBoardSim>> boards_;
  std::vector<std::unique_ptr<AgilentUnitSim>> units_;
  std::vector<std::unique_ptr<LakeshoreSim>> cryostats_;
  std::vector<std::unique_ptr<PychronValveServerSim>> valve_servers_;
  std::map<std::string, std::unique_ptr<extraction::ChromiumSim>, std::less<>> lasers_;  // by driver name
};

}  // namespace pychron::sim
