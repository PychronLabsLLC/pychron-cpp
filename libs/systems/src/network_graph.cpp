#include "pychron/systems/network_graph.hpp"

#include <deque>
#include <initializer_list>

#include "pychron/systems/canvas/canvas.hpp"

namespace pychron::systems {
namespace {

// Joins every pair of `names`: tees and crosses are valveless junctions.
void join_all(NetworkGraph& g, std::initializer_list<const std::string*> names) {
  for (auto a = names.begin(); a != names.end(); ++a) {
    for (auto b = a + 1; b != names.end(); ++b) {
      if (**a != **b) (void)g.connect(**a, **b);
    }
  }
}

}  // namespace

NetworkGraph NetworkGraph::from_canvas(const canvas::Canvas& canvas) {
  NetworkGraph g;
  for (const auto& v : canvas.valves) {
    if (v.kind != canvas::ValveKind::Switch) g.add_valve(v.name);
  }
  for (const auto& s : canvas.stages) g.add_volume(s.name);
  for (const auto& p : canvas.pipettes) g.add_volume(p.name);
  for (const auto& gauge : canvas.gauges) g.add_volume(gauge.name);

  // Endpoints were validated by the loader; connect() failures cannot occur
  // for a loaded canvas and are ignored.
  for (const auto& c : canvas.connections) (void)g.connect(c.start, c.end);
  for (const auto& e : canvas.elbows) (void)g.connect(e.start, e.end);
  for (const auto& t : canvas.tees) join_all(g, {&t.left, &t.right, &t.mid});
  for (const auto& x : canvas.crosses) join_all(g, {&x.left, &x.right, &x.top, &x.bottom});
  return g;
}

void NetworkGraph::add_volume(std::string name) { nodes_.try_emplace(std::move(name), Node{false, {}}); }

void NetworkGraph::add_valve(std::string name) { nodes_.try_emplace(std::move(name), Node{true, {}}); }

Result<void> NetworkGraph::connect(std::string_view a, std::string_view b) {
  auto ia = nodes_.find(a);
  auto ib = nodes_.find(b);
  if (ia == nodes_.end()) return fail(ErrorKind::Config, "network: unknown node '" + std::string(a) + "'");
  if (ib == nodes_.end()) return fail(ErrorKind::Config, "network: unknown node '" + std::string(b) + "'");
  if (ia == ib) return fail(ErrorKind::Config, "network: '" + std::string(a) + "' connected to itself");
  ia->second.edges.insert(ib->first);
  ib->second.edges.insert(ia->first);
  return {};
}

bool NetworkGraph::contains(std::string_view name) const { return nodes_.contains(name); }

bool NetworkGraph::is_valve(std::string_view name) const {
  auto it = nodes_.find(name);
  return it != nodes_.end() && it->second.valve;
}

std::set<std::string> NetworkGraph::volumes() const {
  std::set<std::string> out;
  for (const auto& [name, node] : nodes_) {
    if (!node.valve) out.insert(name);
  }
  return out;
}

std::set<std::string> NetworkGraph::valves() const {
  std::set<std::string> out;
  for (const auto& [name, node] : nodes_) {
    if (node.valve) out.insert(name);
  }
  return out;
}

std::set<std::string> NetworkGraph::neighbors(std::string_view name) const {
  auto it = nodes_.find(name);
  return it == nodes_.end() ? std::set<std::string>{} : it->second.edges;
}

bool NetworkGraph::conducts(const std::string& name, const Node& node, const ValveStates& states) const {
  if (!node.valve) return true;
  auto it = states.find(name);
  return it != states.end() && it->second == ValveState::Open;
}

std::vector<NetworkGraph::Region> NetworkGraph::connected_volumes(const ValveStates& states) const {
  std::vector<Region> regions;
  std::set<std::string_view> seen;
  // Flood fill from each unvisited volume through conducting nodes only.
  for (const auto& [start, start_node] : nodes_) {
    if (start_node.valve || seen.contains(start)) continue;
    Region region;
    std::deque<std::string_view> queue{start};
    seen.insert(start);
    while (!queue.empty()) {
      const auto it = nodes_.find(queue.front());
      queue.pop_front();
      (it->second.valve ? region.valves : region.volumes).insert(it->first);
      for (const auto& next : it->second.edges) {
        if (seen.contains(next)) continue;
        const auto& node = nodes_.find(next)->second;
        if (!conducts(next, node, states)) continue;
        seen.insert(next);
        queue.push_back(next);
      }
    }
    regions.push_back(std::move(region));
  }
  return regions;
}

std::set<std::string> NetworkGraph::connected_to(std::string_view volume, const ValveStates& states) const {
  const auto regions = connected_volumes(states);
  const auto* r = region_of(volume, regions);
  return r == nullptr ? std::set<std::string>{} : r->volumes;
}

const NetworkGraph::Region* NetworkGraph::region_of(std::string_view volume, const std::vector<Region>& regions) {
  for (const auto& r : regions) {
    if (r.volumes.contains(std::string(volume))) return &r;
  }
  return nullptr;
}

}  // namespace pychron::systems
