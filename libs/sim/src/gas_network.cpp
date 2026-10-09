#include "pychron/sim/gas_network.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/sim/gas.hpp"
#include "pychron/sim/linear_flow.hpp"

namespace pychron::sim {
namespace {

bool amount(double value) { return std::isfinite(value) && value >= 0.0; }

// Molecular flow: a valve's conductance for a species, as a factor of its
// conductance for Ar40. Exactly 1 for Ar40.
double flow_factor(std::size_t species) {
  return std::sqrt(kSpeciesMass[index(Species::Ar40)] / kSpeciesMass[species]);
}

bool pumps(const GasPump& pump, std::size_t species) {
  return species == index(Species::Active) ? pump.active : pump.nobles;
}

// The pump's base pressure per species: `base` over the species it pumps,
// in the proportions air has them. Sums to `base`.
Composition base_shares(const GasPump& pump) {
  const Composition air = air_ratios();
  double pumped = 0.0;
  for (std::size_t s = 0; s < kSpeciesCount; ++s) pumped += pumps(pump, s) ? air[s] : 0.0;
  Composition shares{};
  if (!(pumped > 0.0)) return shares;
  for (std::size_t s = 0; s < kSpeciesCount; ++s) {
    if (pumps(pump, s)) shares[s] = pump.base * (air[s] / pumped);
  }
  return shares;
}

std::string quoted(std::string_view name) { return "'" + std::string(name) + "'"; }

// What a volume says of itself, whoever it is joined to.
Result<void> check(const GasVolume& volume) {
  if (volume.name.empty()) return fail(ErrorKind::Config, "gas network: a volume has no name");
  const std::string where = "gas network: volume " + quoted(volume.name);
  if (!(std::isfinite(volume.litres) && volume.litres > 0.0)) {
    return fail(ErrorKind::Config, where + " is not a size above zero");
  }
  for (std::size_t s = 0; s < kSpeciesCount; ++s) {
    const std::string species(kSpeciesName[s]);
    if (!amount(volume.initial[s]) || !std::isfinite(volume.initial[s] * volume.litres)) {
      return fail(ErrorKind::Config, where + " has a negative or non-finite pressure of " + species);
    }
    if (!amount(volume.source_per_s[s])) {
      return fail(ErrorKind::Config, where + " has a negative or non-finite source of " + species);
    }
    if (!amount(volume.loss_per_s[s]) || !std::isfinite(volume.loss_per_s[s] * volume.litres)) {
      return fail(ErrorKind::Config, where + " has a negative or non-finite loss of " + species);
    }
  }
  return {};
}

// What a pump says of itself.
Result<void> check(const GasPump& pump, std::string_view volume) {
  const std::string where = "gas network: the pump on " + quoted(volume);
  if (!amount(pump.speed)) return fail(ErrorKind::Config, where + " has a negative or non-finite speed");
  if (!amount(pump.base)) return fail(ErrorKind::Config, where + " has a negative or non-finite base pressure");
  return {};
}

// "'a'", "'a' and 'b'", "'a', 'b' and 'c'".
std::string listed(const std::vector<std::string>& names) {
  std::string out;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out += i + 1 == names.size() ? " and " : ", ";
    out += quoted(names[i]);
  }
  return out;
}

}  // namespace

void GasNetwork::fit(Volume& whole, const GasPump& pump) {
  const Composition shares = base_shares(pump);
  for (std::size_t s = 0; s < kSpeciesCount; ++s) {
    if (!pumps(pump, s)) continue;
    whole.loss[s] += pump.speed / whole.litres;
    whole.source[s] += shares[s] * pump.speed;
  }
}

Result<GasNetwork> GasNetwork::make(GasTopology topology) {
  GasNetwork network;

  // Names first: a volume or a valve, once.
  std::map<std::string, std::size_t, std::less<>> described;  // volume name -> index in topology.volumes
  for (std::size_t i = 0; i < topology.volumes.size(); ++i) {
    const GasVolume& volume = topology.volumes[i];
    if (auto checked = check(volume); !checked) return fail(checked.error());
    if (!described.emplace(volume.name, i).second) {
      return fail(ErrorKind::Config, "gas network: two volumes are named " + quoted(volume.name));
    }
  }
  for (const GasValve& valve : topology.valves) {
    if (valve.name.empty()) return fail(ErrorKind::Config, "gas network: a valve has no name");
    const std::string where = "gas network: valve " + quoted(valve.name);
    if (described.contains(valve.name)) {
      return fail(ErrorKind::Config, where + " has the name of a volume");
    }
    // For the lightest species too: that is the largest it is used at.
    if (!amount(valve.conductance) || !std::isfinite(valve.conductance * flow_factor(index(Species::Active)))) {
      return fail(ErrorKind::Config, where + " has a negative or non-finite conductance");
    }
    Valve made;
    made.conductance = valve.conductance;
    if (!network.valve_of_.emplace(valve.name, network.valves_.size()).second) {
      return fail(ErrorKind::Config, "gas network: two valves are named " + quoted(valve.name));
    }
    network.valves_.push_back(made);
  }
  for (const GasPump& pump : topology.pumps) {
    const std::string where = "gas network: the pump on " + quoted(pump.volume);
    if (!described.contains(pump.volume)) {
      return fail(ErrorKind::Config, where + " is on a volume that is not there");
    }
    if (auto checked = check(pump, pump.volume); !checked) return fail(checked.error());
  }

  // Volumes joined by an edge with no valve between them are one. `joined`
  // is, per described volume, the first described volume of its group.
  const std::size_t count = topology.volumes.size();
  std::vector<std::size_t> joined(count);
  for (std::size_t i = 0; i < count; ++i) joined[i] = i;
  const auto first_of = [&joined](std::size_t i) {
    while (joined[i] != i) i = joined[i] = joined[joined[i]];
    return i;
  };
  for (const auto& [from, to] : topology.edges) {
    const auto a = described.find(from);
    const auto b = described.find(to);
    if (a == described.end() || b == described.end()) continue;
    const std::size_t x = first_of(a->second);
    const std::size_t y = first_of(b->second);
    joined[std::max(x, y)] = std::min(x, y);
  }

  // One internal volume per group, in the order the groups were described.
  // Sizes, amounts and sources add. A loss acts on the gas in its own part,
  // V_i / V of the gas of the whole: it is gathered as loss * V_i and
  // divided by V below.
  std::vector<std::size_t> merged(count, 0);  // described volume -> internal volume
  for (std::size_t i = 0; i < count; ++i) {
    const GasVolume& volume = topology.volumes[i];
    const std::size_t first = first_of(i);
    if (first == i) {
      merged[i] = network.volumes_.size();
      network.volumes_.emplace_back();
      for (auto& amounts : network.amount_) amounts.push_back(0.0);
    }
    merged[i] = merged[first];  // the first of a group comes first
    Volume& whole = network.volumes_[merged[i]];
    whole.litres += volume.litres;
    for (std::size_t s = 0; s < kSpeciesCount; ++s) {
      network.amount_[s][merged[i]] += volume.initial[s] * volume.litres;
      whole.source[s] += volume.source_per_s[s];
      whole.loss[s] += volume.loss_per_s[s] * volume.litres;
    }
    network.volume_of_.emplace(volume.name, merged[i]);
  }
  for (Volume& whole : network.volumes_) {
    for (double& loss : whole.loss) loss /= whole.litres;
  }

  for (const GasPump& pump : topology.pumps) {
    fit(network.volumes_[merged[described.find(pump.volume)->second]], pump);
  }

  // Each number was finite; sums and quotients of them need not be.
  for (std::size_t i = 0; i < count; ++i) {
    if (first_of(i) != i) continue;
    const Volume& whole = network.volumes_[merged[i]];
    bool finite = std::isfinite(whole.litres);
    for (std::size_t s = 0; s < kSpeciesCount; ++s) {
      finite = finite && std::isfinite(whole.source[s]) && std::isfinite(whole.loss[s]) &&
               std::isfinite(network.amount_[s][merged[i]]);
    }
    if (!finite) {
      return fail(ErrorKind::Config, "gas network: volume " + quoted(topology.volumes[i].name) +
                                         ", with what is joined to it and pumps on it, is beyond what a number holds");
    }
  }

  // A valve's neighbours are the merged volumes it has an edge to. Exactly
  // two make it a link; any other number leaves it a valve with no physics,
  // and what it was joined to instead says why.
  struct Beside {
    std::vector<std::size_t> wholes;   // merged volumes, each once
    std::vector<std::string> volumes;  // as named in the edges, each once
    std::vector<std::string> valves;   // other valves it has an edge to
  };
  std::vector<Beside> neighbours(network.valves_.size());
  const auto once = [](std::vector<std::string>& names, const std::string& name) {
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  };
  for (const auto& [from, to] : topology.edges) {
    for (const auto& [valve_name, other] : {std::pair{from, to}, std::pair{to, from}}) {
      const auto valve = network.valve_of_.find(valve_name);
      if (valve == network.valve_of_.end()) continue;
      Beside& beside = neighbours[valve->second];
      if (network.valve_of_.contains(other)) {
        if (other != valve_name) once(beside.valves, other);
        continue;
      }
      const auto volume = described.find(other);
      if (volume == described.end()) continue;
      const std::size_t whole = merged[volume->second];
      if (std::find(beside.wholes.begin(), beside.wholes.end(), whole) == beside.wholes.end()) {
        beside.wholes.push_back(whole);
      }
      once(beside.volumes, other);
    }
  }
  for (const auto& [name, v] : network.valve_of_) {
    const Beside& beside = neighbours[v];
    if (beside.wholes.size() == 2) {
      network.valves_[v].linked = true;
      network.valves_[v].a = beside.wholes[0];
      network.valves_[v].b = beside.wholes[1];
      continue;
    }
    std::string why;
    if (beside.wholes.empty()) {
      why = "it is joined to no volume";
    } else if (beside.wholes.size() > 2) {
      why = "it is joined to " + std::to_string(beside.wholes.size()) + " volumes (" + listed(beside.volumes) +
            "), and a valve stands between two";
    } else if (beside.volumes.size() > 1) {
      why = "both its sides are one volume (" + listed(beside.volumes) + " are joined with no valve between)";
    } else {
      why = "it is joined to one volume only (" + listed(beside.volumes) + ")";
    }
    if (!beside.valves.empty()) {
      why += "; it is joined to valve " + listed(beside.valves) + " with no volume between";
    }
    network.unlinked_.emplace(name, std::move(why));
  }

  if (auto built = network.rebuild(); !built) return fail(built.error());
  return network;
}

Result<void> GasNetwork::rebuild() {
  FlowTerms terms;
  terms.volume.reserve(volumes_.size());
  for (const Volume& volume : volumes_) terms.volume.push_back(volume.litres);
  terms.loss.resize(volumes_.size());
  terms.source.resize(volumes_.size());

  std::vector<LinearFlow> flows;
  flows.reserve(kSpeciesCount);
  for (std::size_t s = 0; s < kSpeciesCount; ++s) {
    terms.links.clear();
    for (const Valve& valve : valves_) {
      if (valve.open && valve.linked) terms.links.push_back({valve.a, valve.b, valve.conductance * flow_factor(s)});
    }
    for (std::size_t i = 0; i < volumes_.size(); ++i) {
      terms.loss[i] = volumes_[i].loss[s];
      terms.source[i] = volumes_[i].source[s];
    }
    auto flow = LinearFlow::make(terms);
    if (!flow) return fail(ErrorKind::Config, "gas network: " + flow.error().what);
    flows.push_back(std::move(*flow));
  }
  flows_ = std::move(flows);
  return {};
}

void GasNetwork::advance(double seconds) {
  if (!std::isfinite(seconds) || !(seconds > 0.0)) return;
  for (std::size_t s = 0; s < flows_.size(); ++s) flows_[s].advance(amount_[s], seconds);
}

void GasNetwork::set_valve(std::string_view name, bool open) {
  const auto found = valve_of_.find(name);
  if (found == valve_of_.end()) return;
  Valve& valve = valves_[found->second];
  if (valve.open == open) return;
  valve.open = open;
  if (!valve.linked) return;  // no physics: nothing flows differently
  // Everything the flows are made of was checked when it came in, so this
  // cannot be refused; were it, the valve stays as the flows have it.
  const auto built = rebuild();
  assert(built.has_value());
  if (!built) valve.open = !open;
}

bool GasNetwork::valve_open(std::string_view name) const {
  const auto found = valve_of_.find(name);
  return found != valve_of_.end() && valves_[found->second].open;
}

std::size_t GasNetwork::find_volume(std::string_view name) const {
  const auto found = volume_of_.find(name);
  return found == volume_of_.end() ? volumes_.size() : found->second;
}

bool GasNetwork::has_volume(std::string_view name) const { return find_volume(name) != volumes_.size(); }

Result<Composition> GasNetwork::partial_pressures(std::string_view volume) const {
  const std::size_t i = find_volume(volume);
  if (i == volumes_.size()) return fail(ErrorKind::Config, "gas network: no volume " + quoted(volume));
  Composition pressures{};
  for (std::size_t s = 0; s < kSpeciesCount; ++s) pressures[s] = amount_[s][i] / volumes_[i].litres;
  return pressures;
}

Result<double> GasNetwork::pressure(std::string_view volume) const {
  const auto pressures = partial_pressures(volume);
  if (!pressures) return fail(pressures.error());
  return total(*pressures);
}

Result<void> GasNetwork::set_partial_pressures(std::string_view volume, const Composition& mbar) {
  const std::size_t i = find_volume(volume);
  if (i == volumes_.size()) return fail(ErrorKind::Config, "gas network: no volume " + quoted(volume));
  for (std::size_t s = 0; s < kSpeciesCount; ++s) {
    if (!amount(mbar[s]) || !std::isfinite(mbar[s] * volumes_[i].litres)) {
      return fail(ErrorKind::Config, "gas network: a negative or non-finite pressure of " +
                                         std::string(kSpeciesName[s]) + " for volume " + quoted(volume));
    }
  }
  for (std::size_t s = 0; s < kSpeciesCount; ++s) amount_[s][i] = mbar[s] * volumes_[i].litres;
  return {};
}

Result<void> GasNetwork::inject(std::string_view volume, const Composition& mbar_litres) {
  const std::size_t i = find_volume(volume);
  if (i == volumes_.size()) return fail(ErrorKind::Config, "gas network: no volume " + quoted(volume));
  for (std::size_t s = 0; s < kSpeciesCount; ++s) {
    if (!amount(mbar_litres[s]) || !std::isfinite(amount_[s][i] + mbar_litres[s])) {
      return fail(ErrorKind::Config, "gas network: a negative or non-finite amount of " +
                                         std::string(kSpeciesName[s]) + " for volume " + quoted(volume));
    }
  }
  for (std::size_t s = 0; s < kSpeciesCount; ++s) amount_[s][i] += mbar_litres[s];
  return {};
}

std::vector<std::pair<std::string, std::string>> GasNetwork::valves_without_physics() const {
  return {unlinked_.begin(), unlinked_.end()};
}

Result<void> GasNetwork::add_volume(GasVolume volume, const std::vector<GasPump>& pumps) {
  if (auto checked = check(volume); !checked) return fail(checked.error());
  for (const GasPump& pump : pumps) {
    if (auto checked = check(pump, volume.name); !checked) return fail(checked.error());
  }
  if (volume_of_.contains(volume.name)) {
    return fail(ErrorKind::Config, "gas network: there is a volume " + quoted(volume.name) + " already");
  }
  if (valve_of_.contains(volume.name)) {
    return fail(ErrorKind::Config, "gas network: volume " + quoted(volume.name) + " has the name of a valve");
  }
  // On a copy, so that a refusal leaves this network as it was.
  GasNetwork next = *this;
  Volume made;
  made.litres = volume.litres;
  made.source = volume.source_per_s;
  made.loss = volume.loss_per_s;
  for (const GasPump& pump : pumps) fit(made, pump);
  for (std::size_t s = 0; s < kSpeciesCount; ++s) {
    if (!std::isfinite(made.source[s]) || !std::isfinite(made.loss[s])) {
      return fail(ErrorKind::Config, "gas network: volume " + quoted(volume.name) +
                                         ", with the pumps on it, is beyond what a number holds");
    }
  }
  next.volume_of_.emplace(volume.name, next.volumes_.size());
  next.volumes_.push_back(made);
  for (std::size_t s = 0; s < kSpeciesCount; ++s) next.amount_[s].push_back(volume.initial[s] * volume.litres);
  if (auto built = next.rebuild(); !built) return fail(built.error());
  *this = std::move(next);
  return {};
}

}  // namespace pychron::sim
