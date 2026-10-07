#pragma once

// GasNetwork: the gas in a simulated extraction line, by species (spec
// sections 3 and 4).
//
// Volumes hold gas; valves join them; pumps and getters take it away;
// sources (outgassing, leaks) and first-order losses (ion consumption,
// memory) are properties of a volume. Nothing else is modelled, and nothing
// more is needed for a pipette: a tank is a volume with gas in it, a pipette
// a small volume between two valves, and a shot is what the script's own
// valve sequence leaves in the line.
//
// Physics:
//   - Each of the six species of `gas.hpp` moves by itself. In volume i it
//     has an amount n_i (mbar L) and a partial pressure p_i = n_i / V_i; a
//     volume's pressure is the sum over species.
//   - An open valve between volumes i and j carries C (p_j - p_i), where C
//     is the valve's conductance for Ar40 times sqrt(39.962 / mass)
//     (molecular flow): lighter gas arrives first, and an inlet fractionates
//     until it has equilibrated. A closed valve carries nothing. All valves
//     start closed.
//   - Volumes joined by an edge with no valve between them are one volume:
//     its size is the sum, its starting pressure the volume-weighted mean,
//     its sources the sum, its loss the volume-weighted mean (a loss acts on
//     the gas in its own part), and it has every pump of its parts. Each
//     name still answers, with the pressure of the whole.
//   - A pump takes the species it pumps towards its base pressure,
//     dp/dt = -(speed / V) (p - share), the base being shared among those
//     species in the proportions of air. `nobles` are the five argon,
//     `active` the active gas: a getter is a pump with `nobles = false`.
//     Pumps on one volume add.
//   - A volume's `source_per_s` is a constant inflow, its `loss_per_s` a
//     first-order loss.
//
// Between valve events that is, for each species, dn/dt = K n + s with K and
// s constant: a `LinearFlow`, which has no step size. The six are made anew
// when a valve changes state or a volume is added; `advance` only applies
// them. So a step of an hour is 3600 steps of a second, a valve that
// equilibrates in a millisecond and an outgassing rate of hours coexist, and
// no pressure is ever negative, infinite or not a number.
//
// A valve that does not stand between exactly two (merged) volumes is still
// a valve, with a state that can be set and read, and no physics: one named
// in no edge, one with a single volume or three, one whose two ends are the
// same merged volume. Only an edge between a valve and a volume gives the
// valve a neighbour; an edge naming something that is not there, or joining
// two valves, joins nothing.
//
// Units: mbar, litres, mbar L, seconds, L/s. Not thread-safe, and knows no
// clock: whoever owns the network holds the mutex and the time.

#include <array>
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

using SpeciesRates = Composition;  // 1/s per species

struct GasVolume {
  std::string name;
  double litres = 0.05;
  Composition initial{};       // mbar
  Composition source_per_s{};  // mbar L / s: outgassing and leaks
  SpeciesRates loss_per_s{};   // 1 / s: first-order loss
};

struct GasValve {
  std::string name;
  double conductance = 0.1;  // L/s for Ar40
};

struct GasPump {
  std::string volume;
  double speed = 50.0;  // L/s
  double base = 1e-9;   // mbar, total over the species it pumps
  bool nobles = true;   // pumps the five argon
  bool active = true;   // pumps the active gas
};

struct GasTopology {
  std::vector<GasVolume> volumes;
  std::vector<GasValve> valves;
  std::vector<GasPump> pumps;
  // Endpoints name volumes or valves. Volume to volume: the two are one.
  // Volume to valve: the volume is on one side of the valve.
  std::vector<std::pair<std::string, std::string>> edges;
};

class GasNetwork {
 public:
  // Config error, naming the item, on: a volume or valve with no name, or a
  // name used twice (a volume and a valve may not share one); a volume whose
  // size is not above zero; a pump on a volume that is not there; and any
  // pressure, source, loss, conductance, speed or base that is negative or
  // not finite.
  static Result<GasNetwork> make(GasTopology topology);

  // Every volume `seconds` later. A step that is not a time (zero, negative,
  // not finite) is no step.
  void advance(double seconds);

  void set_valve(std::string_view name, bool open);  // unknown name: ignored
  bool valve_open(std::string_view name) const;      // unknown: false

  bool has_volume(std::string_view name) const;
  // Config error when no volume has the name.
  Result<Composition> partial_pressures(std::string_view volume) const;  // mbar
  Result<double> pressure(std::string_view volume) const;                // total, mbar

  // Both act on the whole merged volume the name is part of: it is given
  // these pressures, or these amounts are added to it. Config error on an
  // unknown volume or a value that is negative or not finite; nothing is
  // changed then.
  Result<void> set_partial_pressures(std::string_view volume, const Composition& mbar);
  Result<void> inject(std::string_view volume, const Composition& mbar_litres);

  // A volume joined to nothing (a gauge off the canvas), with the pumps on
  // it (their `volume` is not read: they are this one's). Config error as in
  // `make`, and when a volume or valve already has the name.
  Result<void> add_volume(GasVolume volume, const std::vector<GasPump>& pumps = {});

  // The valves that carry nothing whatever their state, by name, each with
  // why: what the line's builder tells the user it could not model.
  std::vector<std::pair<std::string, std::string>> valves_without_physics() const;

 private:
  GasNetwork() = default;

  // A volume after merging, with the pumps on it folded into its rates.
  struct Volume {
    double litres = 0.0;
    Composition source{};  // mbar L / s
    SpeciesRates loss{};   // 1 / s
  };
  struct Valve {
    double conductance = 0.0;  // L/s for Ar40
    bool open = false;
    bool linked = false;  // stands between volumes a and b
    std::size_t a = 0;
    std::size_t b = 0;
  };

  // A pump is a loss of speed / V on its (merged) volume and a source of
  // its base share times its speed: together, -(speed / V) (p - share).
  static void fit(Volume& whole, const GasPump& pump);
  // The six flows of the volumes and open valves as they are now.
  Result<void> rebuild();
  // Index into volumes_, or volumes_.size() for a name that is no volume.
  std::size_t find_volume(std::string_view name) const;

  std::vector<Volume> volumes_;
  std::map<std::string, std::size_t, std::less<>> volume_of_;  // every original name
  std::vector<Valve> valves_;
  std::map<std::string, std::size_t, std::less<>> valve_of_;
  std::map<std::string, std::string, std::less<>> unlinked_;  // valve -> why it has no physics
  // mbar L, per species, per volume: what `LinearFlow` advances.
  std::array<std::vector<double>, kSpeciesCount> amount_;
  std::vector<LinearFlow> flows_;  // one per species
};

}  // namespace pychron::sim
