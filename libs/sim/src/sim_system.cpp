#include "pychron/sim/sim_system.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

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
double litres_of(double cc, const SimSettings& settings) {
  return (cc > 0 ? cc : settings.default_volume_cc) / 1000.0;
}

// `mbar` of air.
Composition air_at(double mbar) { return scaled(air_ratios(), mbar / total(air_ratios())); }

// What a volume holds at the start: its composition, else its pressure (or
// the default) as air.
Composition initial_of(const std::string& name, const SimSettings& settings) {
  if (auto given = settings.compositions.find(name); given != settings.compositions.end()) return given->second;
  auto init = settings.initial_pressures.find(name);
  return air_at(init == settings.initial_pressures.end() ? settings.default_pressure : init->second);
}

// A getter's pumping speed for active gas, L/s.
constexpr double kGetterSpeed = 1.0;

// The network's description of a line. What a SimTopology may say loosely
// (a name twice, a valve with the name of a volume, settings for names that
// are not there) is settled here, so that the network refuses only numbers
// that cannot be: a negative or non-finite pressure, size, conductance, rate
// or pump base.
GasTopology describe(const SimTopology& topology, const SimSettings& settings) {
  GasTopology out;
  std::map<std::string, double, std::less<>> litres;  // by volume
  for (const auto& v : topology.volumes) {
    if (v.name.empty() || litres.contains(v.name)) continue;
    GasVolume volume;
    volume.name = v.name;
    volume.litres = litres_of(v.cc, settings);
    volume.initial = initial_of(v.name, settings);
    // Walls: per litre, argon as in air and active gas at its own rate.
    volume.source_per_s = scaled(with_ar40(air_ratios(), settings.outgassing), volume.litres);
    volume.source_per_s[index(Species::Active)] = settings.outgassing_active * volume.litres;
    if (auto leak = settings.leaks.find(v.name); leak != settings.leaks.end()) {
      const Composition air = with_ar40(air_ratios(), leak->second);
      for (std::size_t s = 0; s < kSpeciesCount; ++s) volume.source_per_s[s] += air[s];
    }
    litres.emplace(v.name, volume.litres);
    out.volumes.push_back(std::move(volume));
  }

  std::set<std::string, std::less<>> valves;
  for (const auto& name : topology.valves) {
    if (name.empty() || litres.contains(name) || !valves.insert(name).second) continue;
    auto own = settings.conductances.find(name);
    out.valves.push_back({name, own == settings.conductances.end() ? settings.valve_conductance : own->second});
  }

  for (const auto& [name, pump] : settings.pumps) {
    auto volume = litres.find(name);
    if (volume == litres.end()) continue;
    // speed = V / tau with V the pump's own volume as described: what that
    // volume is merged with, or opened to, is not the pump's. A pump with no
    // time constant is as fast as the clock can tell.
    const double tau = std::max(seconds(pump.tau), seconds(Duration{1}));
    out.pumps.push_back({name, volume->second / tau, pump.base, true, true});
  }
  for (const auto& [name, getter] : settings.getters) {
    if (getter && litres.contains(name)) out.pumps.push_back({name, kGetterSpeed, 0.0, false, true});
  }

  out.edges = topology.edges;
  return out;
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
      network_(build(topology, settings_, build_error_)),
      start_(clock.now()),
      last_(start_) {
  for (const auto& v : topology.valves) valve_open_[v] = false;
}

SimSystem::~SimSystem() = default;

const std::optional<Error>& SimSystem::build_error() const { return build_error_; }

void SimSystem::add_volume_locked(const std::string& name, double cc) {
  if (network_.has_volume(name)) return;
  // No part of the line, so no walls of it either: it holds what it is
  // given. Refused (the name of a valve, a pressure that cannot be), the
  // gauge has no volume and reads nothing.
  GasVolume volume;
  volume.name = name;
  volume.litres = litres_of(cc, settings_);
  volume.initial = initial_of(name, settings_);
  (void)network_.add_volume(std::move(volume));
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
  if (!p || settings_.noise <= 0) return p;
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
        add_volume_locked(g.name, 1.0);
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
        add_volume_locked(g.name, 1.0);
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
        add_volume_locked(g.name, 1.0);
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
        add_volume_locked(g.name, 1.0);
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
        add_volume_locked(g.name, 1.0);
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
