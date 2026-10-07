#include "pychron/sim/sim_system.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/sim/gas.hpp"
#include "pychron/sim/gas_network.hpp"
#include "pychron/sim/keyed_noise.hpp"

#include "pychron/devices/gp_microion.hpp"
#include "pychron/devices/pfeiffer_maxigauge.hpp"
#include "pychron/devices/varian_xgs600.hpp"
#include "pychron/devices/lakeshore.hpp"
#include "pychron/devices/modbus_device_sim.hpp"
#include "pychron/devices/plc2000_heater.hpp"
#include "pychron/devices/plc2000_valves.hpp"
#include "pychron/codecs/modbus.hpp"
#include "pychron/devices/spectrometer/ngx_sim.hpp"
#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"
#include "pychron/devices/types.hpp"

namespace pychron::sim {

namespace {

double seconds(Duration d) { return std::chrono::duration<double>(d).count(); }

bool is_plc_kind(std::string_view kind) {
  return kind == "plc2000_valves" || kind == "plc2000_gauges" || kind == "plc2000_heater";
}

// The canvas gives cc and the network holds litres: the one place they meet.
// A size the settings give goes first, then the topology's, then the role's.
double litres_of(const std::string& name, double cc, SimRole role, const SimSettings& settings) {
  if (auto own = settings.sizes.find(name); own != settings.sizes.end()) cc = own->second;
  if (!(cc > 0)) {
    switch (role) {
      case SimRole::Pipette: cc = settings.pipette_cc; break;
      case SimRole::Gauge: cc = settings.gauge_cc; break;
      case SimRole::Pipe: cc = settings.pipe_cc; break;
      default: cc = settings.default_volume_cc; break;
    }
  }
  return cc / 1000.0;
}

// `mbar` of air.
Composition air_at(double mbar) { return scaled(air_ratios(), mbar / total(air_ratios())); }

// What a volume holds at the start: its composition, else its pressure as
// air, else a tank's air, else the default pressure as air.
Composition initial_of(const std::string& name, SimRole role, const SimSettings& settings) {
  if (auto given = settings.compositions.find(name); given != settings.compositions.end()) return given->second;
  if (auto init = settings.initial_pressures.find(name); init != settings.initial_pressures.end()) {
    return air_at(init->second);
  }
  if (role == SimRole::Tank) return with_ar40(air_ratios(), settings.tank_argon40);
  return air_at(settings.default_pressure);
}

// One volume as the network has it. `walls`: it is part of the line and its
// walls give gas off; a gauge off the canvas is not.
GasVolume volume_of(const std::string& name, double cc, SimRole role, bool walls, const SimSettings& settings) {
  GasVolume volume;
  volume.name = name;
  volume.litres = litres_of(name, cc, role, settings);
  volume.initial = initial_of(name, role, settings);
  if (walls) {
    // Per litre, argon as in air and active gas at its own rate.
    volume.source_per_s = scaled(with_ar40(air_ratios(), settings.outgassing), volume.litres);
    volume.source_per_s[index(Species::Active)] = settings.outgassing_active * volume.litres;
  }
  if (auto leak = settings.leaks.find(name); leak != settings.leaks.end()) {
    const Composition air = with_ar40(air_ratios(), leak->second);
    for (std::size_t s = 0; s < kSpeciesCount; ++s) volume.source_per_s[s] += air[s];
  }
  if (role == SimRole::Spectrometer) {
    // The ion source uses each argon at the same rate, and its memory comes
    // back as Ar40: fA/s over fA/mbar is mbar/s, of this volume.
    for (const Species s : {Species::Ar36, Species::Ar37, Species::Ar38, Species::Ar39, Species::Ar40}) {
      volume.loss_per_s[index(s)] = settings.source.consumption;
    }
    volume.source_per_s[index(Species::Ar40)] +=
        settings.source.memory_fa_per_s / settings.source.sensitivity * volume.litres;
  }
  return volume;
}

// The pumps on a volume of `litres`: the settings' own for the name, else a
// pump stage's; and a getter's, where the stage or the settings make it one.
std::vector<GasPump> pumps_of(const std::string& name, SimRole role, double litres, const SimSettings& settings) {
  std::vector<GasPump> out;
  if (auto own = settings.pumps.find(name); own != settings.pumps.end()) {
    const SimPump& pump = own->second;
    // speed = V / tau with V the pump's own volume as described: what that
    // volume is merged with, or opened to, is not the pump's. A pump with no
    // time constant is as fast as the clock can tell. A speed given outright
    // goes first.
    const double tau = std::max(seconds(pump.tau), seconds(Duration{1}));
    const auto speed = settings.pump_speeds.find(name);
    out.push_back({name, speed == settings.pump_speeds.end() ? litres / tau : speed->second, pump.base, true, true});
  } else if (role == SimRole::Pump) {
    out.push_back({name, settings.pump_speed, settings.pump_base, true, true});
  }
  const auto listed = settings.getters.find(name);
  if (listed == settings.getters.end() ? role == SimRole::Getter : listed->second) {
    out.push_back({name, settings.getter_speed, 0.0, false, true});
  }
  return out;
}

// The network's description of a line. What a SimTopology may say loosely
// (a name twice, a valve with the name of a volume, settings for names that
// are not there) is settled here, so that the network refuses only numbers
// that cannot be: a negative or non-finite pressure, size, conductance, rate
// or pump base.
GasTopology describe(const SimTopology& topology, const SimSettings& settings) {
  GasTopology out;
  std::set<std::string, std::less<>> volumes;
  for (const auto& v : topology.volumes) {
    if (v.name.empty() || !volumes.insert(v.name).second) continue;
    GasVolume volume = volume_of(v.name, v.cc, v.role, true, settings);
    for (auto& pump : pumps_of(v.name, v.role, volume.litres, settings)) out.pumps.push_back(std::move(pump));
    out.volumes.push_back(std::move(volume));
  }

  std::set<std::string, std::less<>> valves;
  for (const auto& name : topology.valves) {
    if (name.empty() || volumes.contains(name) || !valves.insert(name).second) continue;
    auto own = settings.conductances.find(name);
    out.valves.push_back({name, own == settings.conductances.end() ? settings.valve_conductance : own->second});
  }

  out.edges = topology.edges;
  return out;
}

// The first spectrometer volume by name order, as the network will have it.
std::optional<std::string> spectrometer_of(const SimTopology& topology) {
  std::optional<std::string> first;
  std::set<std::string, std::less<>> seen;
  for (const auto& v : topology.volumes) {
    if (v.name.empty() || !seen.insert(v.name).second) continue;  // the first of a name is the volume
    if (v.role == SimRole::Spectrometer && (!first || v.name < *first)) first = v.name;
  }
  return first;
}

// The network of a line; an empty one, and why, if it is refused.
GasNetwork build(const SimTopology& topology, const SimSettings& settings, std::optional<Error>& error) {
  auto network = GasNetwork::make(describe(topology, settings));
  if (network) return std::move(*network);
  error = network.error();
  return std::move(*GasNetwork::make({}));  // nothing in it to refuse
}

Unexpected<Error> unknown_volume(std::string_view volume) {
  return fail(ErrorKind::Config, "unknown sim volume '" + std::string(volume) + "'");
}

}  // namespace

SimSystem::SimSystem(const Clock& clock, Topology topology, Settings settings)
    : clock_(clock),
      settings_(std::move(settings)),
      spectrometer_(spectrometer_of(topology)),
      network_(build(topology, settings_, build_error_)),
      start_(clock.now()),
      last_(start_) {
  for (const auto& v : topology.valves) valve_open_[v] = false;
}

SimSystem::~SimSystem() = default;

std::optional<Error> SimSystem::build_error() const {
  std::lock_guard lock(mutex_);
  return build_error_;
}

std::optional<std::string> SimSystem::spectrometer_volume() const {
  std::lock_guard lock(mutex_);
  return spectrometer_ && network_.has_volume(*spectrometer_) ? spectrometer_ : std::nullopt;
}

std::vector<std::pair<std::string, std::string>> SimSystem::valves_without_physics() const {
  std::lock_guard lock(mutex_);
  return network_.valves_without_physics();
}

void SimSystem::add_gauge_volume_locked(const std::string& name) {
  if (network_.has_volume(name)) return;
  // No part of the line, so no walls of it either: it holds what it is
  // given, and has the leak and the pumps the settings give its name.
  // Refused (the name of a valve, a pressure that cannot be), the gauge has
  // no volume and reads nothing, and the line says why.
  GasVolume volume = volume_of(name, 0.0, SimRole::Gauge, false, settings_);
  const std::vector<GasPump> pumps = pumps_of(name, SimRole::Gauge, volume.litres, settings_);
  if (auto added = network_.add_volume(std::move(volume), pumps); !added && !build_error_) {
    build_error_ = added.error();
  }
}

TimePoint SimSystem::advance_locked() const {
  const TimePoint now = clock_.now();
  if (now > last_) {
    network_.advance(seconds(now - last_));
    last_ = now;
  }
  return now;
}

void SimSystem::set_valve(std::string_view name, bool open) {
  std::lock_guard lock(mutex_);
  advance_locked();
  auto it = valve_open_.find(name);
  if (it == valve_open_.end()) {
    valve_open_.emplace(std::string(name), open);
  } else {
    it->second = open;
  }
  network_.set_valve(name, open);  // a name it does not have: no physics
}

bool SimSystem::valve_open(std::string_view name) const {
  std::lock_guard lock(mutex_);
  auto it = valve_open_.find(name);
  return it != valve_open_.end() && it->second;
}

bool SimSystem::has_volume(std::string_view name) const {
  std::lock_guard lock(mutex_);
  return network_.has_volume(name);
}

Result<double> SimSystem::pressure(std::string_view volume) const {
  std::lock_guard lock(mutex_);
  if (!network_.has_volume(volume)) return unknown_volume(volume);
  advance_locked();
  return network_.pressure(volume);
}

Result<Composition> SimSystem::partial_pressures(std::string_view volume) const {
  std::lock_guard lock(mutex_);
  if (!network_.has_volume(volume)) return unknown_volume(volume);
  advance_locked();
  return network_.partial_pressures(volume);
}

Result<void> SimSystem::set_pressure(std::string_view volume, double value) {
  std::lock_guard lock(mutex_);
  if (!network_.has_volume(volume)) return unknown_volume(volume);
  advance_locked();
  const auto held = network_.partial_pressures(volume);
  if (!held) return fail(held.error());
  const double sum = total(*held);
  return network_.set_partial_pressures(volume, sum > 0 ? scaled(*held, value / sum) : air_at(value));
}

Result<void> SimSystem::set_composition(std::string_view volume, const Composition& mbar) {
  std::lock_guard lock(mutex_);
  if (!network_.has_volume(volume)) return unknown_volume(volume);
  advance_locked();
  return network_.set_partial_pressures(volume, mbar);
}

Result<void> SimSystem::inject(std::string_view volume, const Composition& mbar_litres) {
  std::lock_guard lock(mutex_);
  if (!network_.has_volume(volume)) return unknown_volume(volume);
  advance_locked();
  return network_.inject(volume, mbar_litres);
}

Result<double> SimSystem::gauge_reading(std::string_view volume) const {
  std::lock_guard lock(mutex_);
  if (!network_.has_volume(volume)) return unknown_volume(volume);
  const TimePoint now = advance_locked();
  const auto p = network_.pressure(volume);
  // A noise that is negative or no number is no noise (sim.toml refuses one
  // before it gets here).
  if (!p || !(settings_.noise > 0) || !std::isfinite(settings_.noise)) return p;
  // Keyed by the volume and by the clock's time since this system was built.
  const auto tick = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_).count();
  return std::max(0.0, *p * (1.0 + settings_.noise * keyed_gauss(settings_.seed, volume, tick)));
}

std::optional<ModbusDeviceSim> SimSystem::plc_device(const config::DriverConfig& driver,
                                                     const config::SystemConfig& system) {
  if (driver.kind == "plc2000_valves") {
    // A coil bank: each valve's coil (address + coil_offset) reads and moves
    // that simulated valve, as does a state_source coil on this PLC; any
    // other coil is one the PLC does not have.
    const int offset = static_cast<int>(driver.options["coil_offset"].value_or(std::int64_t{-1}));
    struct CoilUse {
      std::string valve;
      bool inverted = false;
      bool drives = false;  // the valve's actuator coil, not only its read-back
    };
    std::map<std::uint16_t, CoilUse> coils;
    auto add = [&](const std::string& address, const std::string& valve, bool inverted, bool drives) {
      auto index = ValveAddress{address}.as_index();
      if (!index || *index + offset < 0 || *index + offset > 0xFFFF) return;
      coils.emplace(static_cast<std::uint16_t>(*index + offset), CoilUse{valve, inverted, drives});
    };
    auto add_switch = [&](const auto& v) {
      if (v.actuator == driver.name) add(v.address, v.name, v.inverted, true);
      if (v.state_source && v.state_source->driver == driver.name)
        add(v.state_source->address, v.name, v.state_source->inverted, false);
    };
    for (const auto& v : system.valves) add_switch(v);
    for (const auto& sw : system.switches) add_switch(sw);
    ModbusDeviceSim plc;
    plc.unit = static_cast<std::uint8_t>(driver.options["unit"].value_or(std::int64_t{1}));
    plc.read_coil = [this, coils](std::uint16_t a) -> std::optional<bool> {
      auto it = coils.find(a);
      if (it == coils.end()) return std::nullopt;
      return valve_open(it->second.valve) != it->second.inverted;
    };
    plc.write_coil = [this, coils](std::uint16_t a, bool on) {
      auto it = coils.find(a);
      if (it == coils.end()) return false;
      if (it->second.drives) set_valve(it->second.valve, on != it->second.inverted);
      return true;
    };
    return plc;
  }

  if (driver.kind == "plc2000_heater") {
    // A heater on this system's clock, at 25 (the PLC's units) and off.
    auto options = Plc2000Heater::parse_options(driver.options);
    if (!options) return std::nullopt;
    auto unit = std::make_unique<Plc2000HeaterSim>(clock_, std::move(*options));
    auto device = unit->device();
    std::lock_guard lock(mutex_);
    heaters_.insert_or_assign(driver.name, std::move(unit));
    return device;
  }

  if (driver.kind == "plc2000_gauges") {
    // Channel n's float lives at register n + register_offset (plc2000_gauges.hpp).
    const int offset = static_cast<int>(driver.options["register_offset"].value_or(std::int64_t{-1}));
    const auto order = codec::modbus::word_order_from_string(driver.options["word_order"].value_or(std::string("cdab")))
                           .value_or(codec::modbus::WordOrder::CDAB);
    std::map<int, std::string> by_register;  // first register -> gauge
    {
      std::lock_guard lock(mutex_);
      advance_locked();
      for (const auto& g : system.gauges) {
        if (g.driver != driver.name) continue;
        by_register[static_cast<int>(g.channel) + offset] = g.name;
        add_gauge_volume_locked(g.name);
      }
    }
    ModbusDeviceSim plc;
    plc.unit = static_cast<std::uint8_t>(driver.options["unit"].value_or(std::int64_t{1}));
    plc.read_holding = [this, order, by_register = std::move(by_register)](
                           std::uint16_t address) -> std::optional<std::uint16_t> {
      for (int half : {0, 1}) {
        auto it = by_register.find(static_cast<int>(address) - half);
        if (it == by_register.end()) continue;
        auto p = gauge_reading(it->second);
        const auto words = codec::modbus::encode_float(p ? static_cast<float>(*p) : 0.0F, order);
        return words[static_cast<std::size_t>(half)];
      }
      return std::nullopt;
    };
    return plc;
  }

  return std::nullopt;
}

SimTransport::Hook SimSystem::hook_for_transport(std::string_view transport, const config::SystemConfig& system) {
  std::vector<const config::DriverConfig*> drivers;
  for (const auto& [name, d] : system.drivers)
    if (d.transport == transport) drivers.push_back(&d);
  if (drivers.empty()) return {};
  if (drivers.size() > 1) {
    // One PLC serving several drivers answers for all of them.
    std::vector<ModbusDeviceSim> parts;
    for (const auto* d : drivers) {
      if (!is_plc_kind(d->kind)) {
        parts.clear();
        break;
      }
      if (auto part = plc_device(*d, system)) parts.push_back(std::move(*part));
    }
    if (!parts.empty()) return modbus_bus_hook(std::move(parts));
  }
  return hook_for(*drivers.front(), system);
}

SimTransport::Hook SimSystem::hook_for(const config::DriverConfig& driver, const config::SystemConfig& system) {
  if (driver.kind == "proxr_relay") {
    // A relay drives its valve; an inverted valve is open while its relay is off.
    std::map<std::int64_t, std::pair<std::string, bool>> by_index;
    auto map_address = [&](const auto& v) {
      if (v.actuator != driver.name) return;
      if (auto index = ValveAddress{v.address}.as_index()) by_index[*index] = {v.name, v.inverted};
    };
    for (const auto& v : system.valves) map_address(v);
    for (const auto& s : system.switches) map_address(s);

    auto board = std::make_unique<ProxrBoardSim>([this, by_index = std::move(by_index)](std::int64_t index, bool on) {
      if (auto it = by_index.find(index); it != by_index.end()) set_valve(it->second.first, on != it->second.second);
    });
    auto hook = board->hook();
    std::lock_guard lock(mutex_);
    boards_.push_back(std::move(board));
    return hook;
  }

  if (driver.kind == "agilent_switch") {
    // A valve is open while its relay is open, or closed with invert = true
    // (agilent_switch.hpp), and the other way round for an inverted valve.
    const bool unit_invert = driver.options["invert"].value_or(false);
    std::map<std::string, std::pair<std::string, bool>> by_channel;  // name, inverted
    auto map_channel = [&](const auto& v) {
      if (v.actuator != driver.name) return;
      if (auto ch = codec::agilent::channel(v.address)) by_channel[*ch] = {v.name, v.inverted};
    };
    for (const auto& v : system.valves) map_channel(v);
    for (const auto& s : system.switches) map_channel(s);

    auto unit = std::make_unique<AgilentUnitSim>(
        [this, unit_invert, by_channel = std::move(by_channel)](const std::string& channel, bool relay_closed) {
          auto it = by_channel.find(channel);
          if (it == by_channel.end()) return;
          const bool open = (relay_closed == unit_invert) != it->second.second;
          set_valve(it->second.first, open);
        },
        /*relays_start_closed=*/!unit_invert);
    auto hook = unit->hook();
    std::lock_guard lock(mutex_);
    units_.push_back(std::move(unit));
    return hook;
  }

  if (driver.kind == "pychron_valves") {
    // Another Pychron's valve service, serving the names this line uses.
    std::map<std::string, std::pair<std::string, bool>> by_name;  // valve, inverted
    for (const auto& v : system.valves)
      if (v.actuator == driver.name) by_name[v.address] = {v.name, v.inverted};
    for (const auto& s : system.switches)
      if (s.actuator == driver.name) by_name[s.address] = {s.name, s.inverted};
    std::set<std::string> names;
    for (const auto& [name, _] : by_name) names.insert(name);
    auto server = std::make_unique<PychronValveServerSim>(
        [this, by_name = std::move(by_name)](const std::string& name, bool open) {
          if (auto it = by_name.find(name); it != by_name.end()) set_valve(it->second.first, open != it->second.second);
        });
    if (!names.empty()) server->declare(std::move(names));
    auto hook = server->hook();
    std::lock_guard lock(mutex_);
    valve_servers_.push_back(std::move(server));
    return hook;
  }

  if (driver.kind == "qtegra_gauges") {
    // A Qtegra whose parameters[n-1] reads the gauge on channel n.
    std::vector<std::string> parameters;
    if (const auto* array = driver.options["parameters"].as_array())
      for (const auto& p : *array) parameters.push_back(p.value_or(std::string{}));
    std::map<std::string, std::string> by_parameter;
    {
      std::lock_guard lock(mutex_);
      advance_locked();
      for (const auto& g : system.gauges) {
        if (g.driver != driver.name || g.channel < 1 || g.channel > static_cast<std::int64_t>(parameters.size()))
          continue;
        by_parameter[parameters[static_cast<std::size_t>(g.channel - 1)]] = g.name;
        add_gauge_volume_locked(g.name);
      }
    }
    auto model = std::make_shared<spectrometer::QtegraSimModel>();
    model->parameter_source = [this, by_parameter = std::move(by_parameter)](
                                  const std::string& name) -> std::optional<double> {
      auto it = by_parameter.find(name);
      if (it == by_parameter.end()) return std::nullopt;
      auto p = gauge_reading(it->second);
      return p ? std::optional<double>(*p) : std::nullopt;
    };
    return spectrometer::qtegra_sim_hook(std::move(model));
  }

  if (driver.kind == "qtegra_valves") {
    // A Qtegra RemoteControl answering valve commands only; its Open/Close
    // move the line's valves (by Qtegra name). On a kind = "link" transport
    // the spectrometer's simulator answers instead, and the line's model
    // does not follow.
    std::map<std::string, std::pair<std::string, bool>> by_name;  // valve, inverted
    for (const auto& v : system.valves)
      if (v.actuator == driver.name) by_name[v.address] = {v.name, v.inverted};
    for (const auto& s : system.switches)
      if (s.actuator == driver.name) by_name[s.address] = {s.name, s.inverted};
    auto model = std::make_shared<spectrometer::QtegraSimModel>();
    model->on_valve = [this, by_name = std::move(by_name)](const std::string& name, bool open) {
      if (auto it = by_name.find(name); it != by_name.end()) set_valve(it->second.first, open != it->second.second);
    };
    return spectrometer::qtegra_sim_hook(std::move(model));
  }

  if (driver.kind == "chromium") {
    auto laser = std::make_unique<extraction::ChromiumSim>(clock_);
    auto hook = laser->hook();
    std::lock_guard lock(mutex_);
    lasers_.insert_or_assign(driver.name, std::move(laser));
    return hook;
  }

  if (driver.kind == "ngx_valves") {
    // The NGX simulator answers (Login, SAB, valves); actuations move the
    // simulated line's valves.
    std::map<std::string, std::pair<std::string, bool>> by_address;  // name, inverted
    for (const auto& v : system.valves)
      if (v.actuator == driver.name) by_address[v.address] = {v.name, v.inverted};
    for (const auto& s : system.switches)
      if (s.actuator == driver.name) by_address[s.address] = {s.name, s.inverted};
    auto model = std::make_shared<spectrometer::NgxSimModel>();
    model->banner_pending = false;  // no event source on a line transport
    auto inner = spectrometer::ngx_sim_hook(model);
    return [this, model, inner = std::move(inner), by_address = std::move(by_address)](const Bytes& tx) {
      Bytes reply = inner(tx);
      std::string cmd = to_string(tx);
      while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n' || cmd.back() == '#')) cmd.pop_back();
      for (const auto* verb : {"OpenValve ", "CloseValve "}) {
        if (!cmd.starts_with(verb)) continue;
        if (auto it = by_address.find(cmd.substr(std::string(verb).size())); it != by_address.end())
          set_valve(it->second.first, cmd.starts_with("OpenValve ") != it->second.second);
      }
      return reply;
    };
  }

  if (driver.kind == "pfeiffer_maxigauge") {
    std::map<int, std::string> by_channel;
    {
      std::lock_guard lock(mutex_);
      advance_locked();
      for (const auto& g : system.gauges) {
        if (g.driver != driver.name) continue;
        by_channel[static_cast<int>(g.channel)] = g.name;
        add_gauge_volume_locked(g.name);
      }
    }
    MaxiGaugeSimModel model;
    model.pressure = [this, by_channel = std::move(by_channel)](int channel) -> std::optional<double> {
      auto it = by_channel.find(channel);
      if (it == by_channel.end()) return std::nullopt;
      auto p = gauge_reading(it->second);
      return p ? std::optional<double>(*p) : std::nullopt;
    };
    return maxigauge_sim_hook(std::move(model));
  }

  if (driver.kind == "lakeshore") {
    // A cryostat on this system's clock: room temperature at start, each
    // input following its output's setpoint while that heater is on.
    auto unit = std::make_unique<LakeshoreSim>(clock_, "MODEL" + driver.options["model"].value_or(std::string("335")));
    auto hook = unit->hook();
    std::lock_guard lock(mutex_);
    cryostats_.push_back(std::move(unit));
    return hook;
  }

  if (driver.kind == "plc2000_heater") {
    // Alone on its transport it can be taken off the network (set_offline).
    if (!plc_device(driver, system)) return {};
    return heater(driver.name)->hook();
  }
  if (auto plc = plc_device(driver, system)) return plc->hook();

  if (driver.kind == "varian_xgs600") {
    // Gauge channel n is labels[n-1] (varian_xgs600.hpp).
    std::vector<std::string> labels;
    if (const auto* array = driver.options["labels"].as_array())
      for (const auto& l : *array) labels.push_back(l.value_or(std::string{}));
    std::map<std::string, std::string> by_label;
    {
      std::lock_guard lock(mutex_);
      advance_locked();
      for (const auto& g : system.gauges) {
        if (g.driver != driver.name || g.channel < 1 || g.channel > static_cast<std::int64_t>(labels.size())) continue;
        by_label[labels[static_cast<std::size_t>(g.channel - 1)]] = g.name;
        add_gauge_volume_locked(g.name);
      }
    }
    Xgs600SimModel model;
    model.address = driver.options["address"].value_or(std::string("00"));
    model.pressure = [this, by_label = std::move(by_label)](const std::string& label) -> std::optional<double> {
      auto it = by_label.find(label);
      if (it == by_label.end()) return std::nullopt;
      auto p = gauge_reading(it->second);
      return p ? std::optional<double>(*p) : std::nullopt;
    };
    return xgs600_sim_hook(std::move(model));
  }

  if (driver.kind == "gp_microion") {
    std::map<int, std::string> by_channel;
    {
      std::lock_guard lock(mutex_);
      advance_locked();
      for (const auto& g : system.gauges) {
        if (g.driver != driver.name) continue;
        by_channel[static_cast<int>(g.channel)] = g.name;
        add_gauge_volume_locked(g.name);
      }
    }
    MicroIonSimModel model;
    // Same key the driver reads (see GpMicroIon::schema); default 1.
    model.address = static_cast<int>(driver.options["address"].value<std::int64_t>().value_or(1));
    model.pressure = [this, by_channel = std::move(by_channel)](int channel) -> std::optional<double> {
      auto it = by_channel.find(channel);
      if (it == by_channel.end()) return std::nullopt;
      auto p = gauge_reading(it->second);
      return p ? std::optional<double>(*p) : std::nullopt;
    };
    return microion_sim_hook(std::move(model));
  }

  return [](const Bytes&) { return Bytes{}; };
}

extraction::ChromiumSim* SimSystem::chromium(std::string_view driver) const {
  std::lock_guard lock(mutex_);
  const auto it = lasers_.find(driver);
  return it == lasers_.end() ? nullptr : it->second.get();
}

Plc2000HeaterSim* SimSystem::heater(std::string_view driver) const {
  std::lock_guard lock(mutex_);
  const auto it = heaters_.find(driver);
  return it == heaters_.end() ? nullptr : it->second.get();
}

}  // namespace pychron::sim
