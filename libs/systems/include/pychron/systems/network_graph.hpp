#pragma once

// Semantic plumbing connectivity (spec section 6). Nodes are volumes and
// valves; edges are canvas connections. Given valve states it answers which
// volumes share gas, driving region coloring now and volume/pipette logic
// later. Qt-free; carries no state of its own beyond topology.

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/core/events.hpp"

namespace pychron::canvas {
struct Canvas;
}

namespace pychron::systems {

// Same shape as Snapshot::valves.
using ValveStates = std::map<std::string, ValveState>;

class NetworkGraph {
 public:
  // One set of volumes sharing gas, plus the open valves joining them.
  struct Region {
    std::set<std::string> volumes;
    std::set<std::string> valves;
  };

  // Stages, pipettes and gauges become volumes; valves, manual valves and
  // rough valves become valves; switches are skipped. Tees and crosses join
  // all their endpoints. The canvas must already be loaded (endpoints valid).
  static NetworkGraph from_canvas(const canvas::Canvas& canvas);

  // Re-adding an existing name is a no-op for the same kind.
  void add_volume(std::string name);
  void add_valve(std::string name);
  // Config error for unknown nodes or a self-edge.
  Result<void> connect(std::string_view a, std::string_view b);

  bool contains(std::string_view name) const;
  bool is_valve(std::string_view name) const;
  std::set<std::string> volumes() const;
  std::set<std::string> valves() const;
  std::set<std::string> neighbors(std::string_view name) const;

  // Partition of every volume into regions. A valve conducts only when its
  // state is Open; Unknown or absent from `states` counts as closed.
  std::vector<Region> connected_volumes(const ValveStates& states) const;

  // Volumes sharing gas with `volume` (including itself); empty if unknown.
  std::set<std::string> connected_to(std::string_view volume, const ValveStates& states) const;

  // The region in `regions` containing `volume`, or nullptr.
  static const Region* region_of(std::string_view volume, const std::vector<Region>& regions);

 private:
  struct Node {
    bool valve = false;
    std::set<std::string> edges;
  };

  bool conducts(const std::string& name, const Node& node, const ValveStates& states) const;

  std::map<std::string, Node, std::less<>> nodes_;
};

}  // namespace pychron::systems
