#pragma once

// SimSystem: stateful model of the physical lab behind `kind = "sim"`
// transports (spec section 7).
//
// One instance serves every driver of a config, so cross-device causality
// emerges naturally: a relay command reaching the modelled ProXR board opens
// a valve here, gas equilibrates across the now-connected volumes, and the
// modelled MaxiGauge reports the new pressure.
//
// Physics: a `GasNetwork` (gas_network.hpp; lab simulator spec sections 3
// and 7), which this class describes, keeps the time of, and guards.
//   - Volumes and valves form a graph (edges as on the canvas). Each volume
//     holds gas by species; a volume's pressure is the sum. A pressure given
//     with no composition is that much air.
//   - An open valve between two volumes carries gas at its conductance, so
//     the two equilibrate to their volume-weighted mean with the time
//     constant V1 V2 / ((V1 + V2) C), lighter gas first. A closed valve
//     carries nothing; every valve starts closed. Volumes joined by an edge
//     with no valve between them are one volume.
//   - A pumped volume decays towards its pump's base pressure:
//       p(t + dt) = base + (p(t) - base) * exp(-dt / tau)
//     and whatever is open to it follows through the valves between.
//   - Walls give off gas (`Settings::outgassing`), so a volume valved off
//     from every pump rises; a leak adds air, a getter takes active gas.
//   - What a volume is to the gas is its role, which the line takes from
//     the canvas (lab simulator spec section 6.1): a pump stage pumps, a
//     getter takes active gas, a tank starts full of air, a pipette is
//     small, and the spectrometer's source uses its argon up slowly and
//     gives a little back (ion consumption and memory, spec section 5.1).
//   - Gauge readings add Gaussian noise (relative, `Settings::noise`) that
//     is a function of the seed, the volume and the time of the reading
//     (keyed_noise.hpp), not of how many readings came before.
// The network is solved exactly between valve events, so advancing lazily to
// clock.now() on every query is exact, however often anything polls.
//
// Sizes are given in cc, as on the canvas, and are litres in the network;
// pressures are mbar, amounts mbar L, conductances L/s.
//
// This library depends on transport/devices/core only, so the topology is a
// plain description; the ExtractionLine facade fills it from its
// NetworkGraph. Time comes from the injected Clock; tests advance a
// ManualClock. All methods are thread-safe: hooks run on transport workers.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/system_config.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/agilent_unit_sim.hpp"
#include "pychron/devices/lakeshore.hpp"
#include "pychron/devices/modbus_device_sim.hpp"
#include "pychron/devices/plc2000_heater.hpp"
#include "pychron/devices/extraction/chromium_sim.hpp"
#include "pychron/devices/pychron_valve_server_sim.hpp"
#include "pychron/devices/proxr_board_sim.hpp"
#include "pychron/sim/gas.hpp"
#include "pychron/sim/gas_network.hpp"
#include "pychron/transport/sim_transport.hpp"

namespace pychron::sim {

// What a volume is to the gas, from the canvas stage's kind.
enum class SimRole { Plain, Pump, Getter, Tank, Pipette, Spectrometer };

struct SimVolume {
  std::string name;
  // Size. Zero or less is unset: SimSettings::sizes, else pipette_cc for a
  // pipette, else default_volume_cc.
  double cc = 0.0;
  SimRole role = SimRole::Plain;
};

struct SimTopology {
  // A name given twice is the first of them; a valve with the name of a
  // volume, or a second valve of one name, is not there.
  std::vector<SimVolume> volumes;
  std::vector<std::string> valves;
  // Undirected; endpoints name volumes or valves. Edges naming unknown
  // nodes are ignored. Volume to volume: the two are one volume. A valve
  // stands between the two volumes it has edges to; one with any other
  // number of them is tracked with no physics.
  std::vector<std::pair<std::string, std::string>> edges;
};

struct SimPump {
  double base = 1e-9;  // ultimate pressure
  // Time constant of the pump on its own volume, valved off from the rest:
  // a pump of speed V / tau, V being that volume's size. With more of the
  // line open to it, it has more to empty and takes longer.
  Duration tau = std::chrono::seconds(5);
};

// Names are the topology's. An entry for a name that is not there does
// nothing.
struct SimSettings {
  double default_pressure = 1e-8;  // volumes not listed below
  std::map<std::string, double> initial_pressures;  // totals, mbar, as air
  // Keyed by volume name. A pump stage with no entry here has a pump of
  // `pump_speed` and `pump_base`.
  std::map<std::string, SimPump> pumps;
  // By volume, for a pump in `pumps`: its speed, L/s, as sim.toml gives it.
  // Goes before the pump's `tau`.
  std::map<std::string, double> pump_speeds;
  double noise = 0.01;                   // relative 1-sigma gauge noise
  std::uint64_t seed = 0x5eed;

  double default_volume_cc = 50.0;  // volumes with no size
  double valve_conductance = 0.1;   // L/s for Ar40; valves not listed below
  // What walls give off, per litre of volume: argon in the ratios of air,
  // `outgassing` mbar L / s of it Ar40, and `outgassing_active` mbar L / s
  // of active gas.
  double outgassing = 5e-13;
  double outgassing_active = 1e-10;
  // By volume: initial partial pressures, mbar. Goes before initial_pressures.
  std::map<std::string, Composition> compositions;
  std::map<std::string, double> conductances;  // by valve: L/s for Ar40
  // By volume: a leak of air, as mbar L / s of Ar40; the rest follows.
  std::map<std::string, double> leaks;
  // By volume: a pump of active gas only, of `getter_speed`. A getter stage
  // is one unless it is false here.
  std::map<std::string, bool> getters;

  // By volume: its size, cc. Goes before the topology's.
  std::map<std::string, double> sizes;
  // mbar of Ar40 a tank starts with, the rest in the ratios of air, unless
  // `compositions` or `initial_pressures` says what it holds.
  double tank_argon40 = 3e-5;
  double pipette_cc = 0.1;    // a pipette with no size
  double pump_speed = 50.0;   // L/s: a pump stage's
  double pump_base = 1e-9;    // mbar: its ultimate pressure
  double getter_speed = 1.0;  // L/s for active gas
  // The spectrometer's source volume (spec section 5.1).
  struct Source {
    double sensitivity = 1e12;      // fA per mbar of an isotope in the source
    double consumption = 2e-5;      // 1/s: what the ion source uses of each argon
    double memory_fa_per_s = 0.01;  // what the source gives back, as Ar40 signal
  } source;
  // By detector: what it reads with no beam on it, and how that drifts. Not
  // the line's: whoever builds the simulated spectrometer reads them.
  struct DetectorBaseline {
    double baseline = 0.0;     // fA (or cps on a counting detector)
    double drift_per_h = 0.0;  // the same, per hour
  };
  std::map<std::string, DetectorBaseline> detectors;
  // Compositions by name, as ratios to Ar36 (`[compositions.<name>]` of
  // sim.toml); `air` and `cocktail` are there without being listed.
  std::map<std::string, Composition> named;
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

  // Model pressure of a volume (the total over species, mbar), advanced to
  // now. Config error if unknown.
  Result<double> pressure(std::string_view volume) const;
  // Its partial pressures, mbar.
  Result<Composition> partial_pressures(std::string_view volume) const;
  // Sets one volume's pressure, keeping the proportions of what it holds
  // (those of air if it is empty). Config error on an unknown volume or a
  // value that is negative or not finite. This and the two below act on the
  // whole of a volume joined to others with no valve between.
  Result<void> set_pressure(std::string_view volume, double value);
  // Sets its partial pressures, mbar.
  Result<void> set_composition(std::string_view volume, const Composition& mbar);
  // Adds gas to it, mbar L per species: what a heated sample releases.
  Result<void> inject(std::string_view volume, const Composition& mbar_litres);
  // pressure() with gauge noise applied: the same reading for the same
  // volume at the same instant, whoever asks and in whatever order.
  Result<double> gauge_reading(std::string_view volume) const;

  bool has_volume(std::string_view name) const;

  // The spectrometer's source: the first volume of that role by name order;
  // nothing when the line has none.
  std::optional<std::string> spectrometer_volume() const;
  // The valves that have a state and no physics, each with why (a valve
  // joined to another valve, a tee on a valve, a dangling valve): what the
  // line warns of when it builds this.
  std::vector<std::pair<std::string, std::string>> valves_without_physics() const;

  // Set when the topology and settings do not describe a network (a
  // negative pressure, a size that is not a number): the line is then empty,
  // no volume answers, and valves are still tracked. Set as well, if not
  // already, when hook_for cannot give a gauge off the canvas its own volume
  // (its name is a valve's, its pressure cannot be): that gauge reads
  // nothing. The first refusal stands; read it once the hooks are made.
  std::optional<Error> build_error() const;

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
  //   plc2000_valves      a coil bank: each valve's coil reads and moves that
  //                       simulated valve.
  //   plc2000_heater      a Plc2000HeaterSim on this system's clock.
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
  // The hook for a sim transport: hook_for its first driver (by name), or,
  // when every driver on it is a PLC kind (plc2000_valves, plc2000_gauges,
  // plc2000_heater) and there are several, one PLC answering for all of
  // them (modbus_bus_hook). A heater on a shared PLC cannot be set offline.
  SimTransport::Hook hook_for_transport(std::string_view transport, const config::SystemConfig& system);

  // The Chromium simulator hook_for built for the driver named `driver`;
  // null if there is none. For tests and tools that look at what the
  // simulated laser was told.
  extraction::ChromiumSim* chromium(std::string_view driver) const;
  // Likewise the PLC heater simulator of a plc2000_heater driver.
  Plc2000HeaterSim* heater(std::string_view driver) const;

 private:
  // The coils and registers a PLC driver's sim serves; nullopt for other
  // kinds (or a heater whose options do not parse).
  std::optional<ModbusDeviceSim> plc_device(const config::DriverConfig& driver, const config::SystemConfig& system);

  // Moves the network to clock.now(), and says when that is.
  TimePoint advance_locked() const;
  // An isolated volume for a gauge the topology does not have, with what
  // the settings say of that name (size, gas, leak, pump, getter); nothing
  // if a volume has the name already.
  void add_volume_locked(const std::string& name, double cc);

  const Clock& clock_;
  Settings settings_;
  mutable std::mutex mutex_;
  std::optional<Error> build_error_;         // the first refusal; under mutex_
  std::optional<std::string> spectrometer_;  // see spectrometer_volume()
  mutable GasNetwork network_;        // only under mutex_
  // What every name was last told, the network's valves and the names it
  // does not have (switches, unmodelled valves) alike.
  std::map<std::string, bool, std::less<>> valve_open_;
  const TimePoint start_;     // gauge noise counts time from here
  mutable TimePoint last_{};  // where the network is
  std::vector<std::unique_ptr<ProxrBoardSim>> boards_;
  std::vector<std::unique_ptr<AgilentUnitSim>> units_;
  std::vector<std::unique_ptr<LakeshoreSim>> cryostats_;
  std::map<std::string, std::unique_ptr<Plc2000HeaterSim>, std::less<>> heaters_;  // by driver name
  std::vector<std::unique_ptr<PychronValveServerSim>> valve_servers_;
  std::map<std::string, std::unique_ptr<extraction::ChromiumSim>, std::less<>> lasers_;  // by driver name
};

}  // namespace pychron::sim
