#include "pychron/sim/sim_system.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

#include "pychron/devices/pfeiffer_maxigauge.hpp"
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
    std::map<std::int64_t, std::string> by_index;
    auto map_address = [&](const std::string& actuator, const std::string& address, const std::string& name) {
      if (actuator != driver.name) return;
      if (auto index = ValveAddress{address}.as_index()) by_index[*index] = name;
    };
    for (const auto& v : system.valves) map_address(v.actuator, v.address, v.name);
    for (const auto& s : system.switches) map_address(s.actuator, s.address, s.name);

    auto board = std::make_unique<ProxrBoardSim>([this, by_index = std::move(by_index)](std::int64_t index, bool on) {
      if (auto it = by_index.find(index); it != by_index.end()) set_valve(it->second, on);
    });
    auto hook = board->hook();
    std::lock_guard lock(mutex_);
    boards_.push_back(std::move(board));
    return hook;
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

  return [](const Bytes&) { return Bytes{}; };
}

}  // namespace pychron::sim
