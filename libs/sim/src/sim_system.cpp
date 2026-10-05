#include "pychron/sim/sim_system.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

#include "pychron/devices/gp_microion.hpp"
#include "pychron/devices/pfeiffer_maxigauge.hpp"
#include "pychron/devices/varian_xgs600.hpp"
#include "pychron/devices/modbus_device_sim.hpp"
#include "pychron/codecs/modbus.hpp"
#include "pychron/devices/spectrometer/ngx_sim.hpp"
#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"
#include "pychron/devices/types.hpp"

namespace pychron::sim {

namespace {

double seconds(Duration d) { return std::chrono::duration<double>(d).count(); }

}  // namespace

SimSystem::SimSystem(const Clock& clock, Topology topology, Settings settings)
    : clock_(clock), settings_(std::move(settings)), last_(clock.now()), rng_(settings_.seed) {
  for (const auto& v : topology.volumes) add_volume_locked(v.name, v.cc);
  for (const auto& v : topology.valves) {
    nodes_[v].valve = true;
    valve_open_[v] = false;
  }
  for (const auto& [a, b] : topology.edges) {
    auto ia = nodes_.find(a);
    auto ib = nodes_.find(b);
    if (ia == nodes_.end() || ib == nodes_.end() || a == b) continue;
    ia->second.edges.insert(b);
    ib->second.edges.insert(a);
  }
  advance_locked();
}

SimSystem::~SimSystem() = default;

void SimSystem::add_volume_locked(const std::string& name, double cc) {
  if (nodes_.contains(name)) return;
  nodes_[name];
  cc_[name] = cc > 0 ? cc : 1.0;
  auto init = settings_.initial_pressures.find(name);
  pressure_[name] = init == settings_.initial_pressures.end() ? settings_.default_pressure : init->second;
}

std::vector<std::vector<std::string>> SimSystem::regions_locked() const {
  std::vector<std::vector<std::string>> out;
  std::set<std::string> seen;
  for (const auto& [start, node] : nodes_) {
    if (node.valve || seen.contains(start)) continue;
    std::vector<std::string> region;
    std::deque<std::string> frontier{start};
    std::set<std::string> visited{start};
    while (!frontier.empty()) {
      std::string name = std::move(frontier.front());
      frontier.pop_front();
      const Node& n = nodes_.find(name)->second;
      if (n.valve) {
        auto open = valve_open_.find(name);
        if (open == valve_open_.end() || !open->second) continue;
      } else {
        region.push_back(name);
        seen.insert(name);
      }
      for (const auto& next : n.edges) {
        if (visited.insert(next).second) frontier.push_back(next);
      }
    }
    out.push_back(std::move(region));
  }
  return out;
}

void SimSystem::advance_locked() const {
  const TimePoint now = clock_.now();
  const double dt = now > last_ ? seconds(now - last_) : 0.0;
  last_ = std::max(now, last_);

  for (const auto& region : regions_locked()) {
    double total_cc = 0;
    double amount = 0;
    double base = std::numeric_limits<double>::infinity();
    double tau = std::numeric_limits<double>::infinity();
    for (const auto& v : region) {
      const double cc = cc_.find(v)->second;
      total_cc += cc;
      amount += cc * pressure_.find(v)->second;
      if (auto pump = settings_.pumps.find(v); pump != settings_.pumps.end()) {
        base = std::min(base, pump->second.base);
        tau = std::min(tau, seconds(pump->second.tau));
      }
    }
    double p = total_cc > 0 ? amount / total_cc : 0.0;
    if (std::isfinite(base) && dt > 0) {
      p = tau > 0 ? base + (p - base) * std::exp(-dt / tau) : base;
    }
    for (const auto& v : region) pressure_.find(v)->second = p;
  }
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
  advance_locked();
}

bool SimSystem::valve_open(std::string_view name) const {
  std::lock_guard lock(mutex_);
  auto it = valve_open_.find(name);
  return it != valve_open_.end() && it->second;
}

bool SimSystem::has_volume(std::string_view name) const {
  std::lock_guard lock(mutex_);
  return pressure_.contains(name);
}

Result<double> SimSystem::pressure(std::string_view volume) const {
  std::lock_guard lock(mutex_);
  auto it = pressure_.find(volume);
  if (it == pressure_.end()) return fail(ErrorKind::Config, "unknown sim volume '" + std::string(volume) + "'");
  advance_locked();
  return it->second;
}

Result<void> SimSystem::set_pressure(std::string_view volume, double value) {
  std::lock_guard lock(mutex_);
  auto it = pressure_.find(volume);
  if (it == pressure_.end()) return fail(ErrorKind::Config, "unknown sim volume '" + std::string(volume) + "'");
  advance_locked();
  it->second = value;
  return {};
}

Result<double> SimSystem::gauge_reading(std::string_view volume) const {
  auto p = pressure(volume);
  if (!p || settings_.noise <= 0) return p;
  std::lock_guard lock(mutex_);
  std::normal_distribution<double> gauss(0.0, settings_.noise);
  return std::max(0.0, *p * (1.0 + gauss(rng_)));
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
    return plc.hook();
  }

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

}  // namespace pychron::sim
