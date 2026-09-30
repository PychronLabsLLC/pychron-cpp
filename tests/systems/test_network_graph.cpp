#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>

#include "pychron/systems/canvas/loader.hpp"
#include "pychron/systems/network_graph.hpp"

namespace pychron::systems {
namespace {

using Names = std::set<std::string>;
using States = std::map<std::string, ValveState>;

// bone --A-- prep --B-- spec
//              |
//              C
//              |
//             air
NetworkGraph line() {
  NetworkGraph g;
  g.add_volume("bone");
  g.add_volume("prep");
  g.add_volume("spec");
  g.add_volume("air");
  g.add_valve("A");
  g.add_valve("B");
  g.add_valve("C");
  g.connect("bone", "A");
  g.connect("A", "prep");
  g.connect("prep", "B");
  g.connect("B", "spec");
  g.connect("prep", "C");
  g.connect("C", "air");
  return g;
}

TEST(NetworkGraph, AllClosedIsolatesEveryVolume) {
  auto g = line();
  const auto regions = g.connected_volumes({{"A", ValveState::Closed}, {"B", ValveState::Closed}, {"C", ValveState::Closed}});
  ASSERT_EQ(regions.size(), 4u);
  for (const auto& r : regions) {
    EXPECT_EQ(r.volumes.size(), 1u);
    EXPECT_TRUE(r.valves.empty());
  }
}

TEST(NetworkGraph, OpenValveJoinsVolumes) {
  auto g = line();
  const auto regions = g.connected_volumes({{"A", ValveState::Open}, {"B", ValveState::Closed}, {"C", ValveState::Closed}});
  ASSERT_EQ(regions.size(), 3u);
  const auto* r = g.region_of("bone", regions);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->volumes, (Names{"bone", "prep"}));
  EXPECT_EQ(r->valves, (Names{"A"}));
  EXPECT_EQ(g.region_of("spec", regions)->volumes, (Names{"spec"}));
}

TEST(NetworkGraph, ChainOfOpenValvesJoinsAll) {
  auto g = line();
  const auto regions = g.connected_volumes({{"A", ValveState::Open}, {"B", ValveState::Open}, {"C", ValveState::Open}});
  ASSERT_EQ(regions.size(), 1u);
  EXPECT_EQ(regions[0].volumes, (Names{"air", "bone", "prep", "spec"}));
  EXPECT_EQ(regions[0].valves, (Names{"A", "B", "C"}));
}

TEST(NetworkGraph, UnknownAndMissingStatesTreatedAsClosed) {
  auto g = line();
  const auto regions = g.connected_volumes({{"A", ValveState::Unknown}});
  EXPECT_EQ(regions.size(), 4u);
}

TEST(NetworkGraph, AdjacentValvesBothMustBeOpen) {
  // v1 -- X -- Y -- v2
  NetworkGraph g;
  g.add_volume("v1");
  g.add_volume("v2");
  g.add_valve("X");
  g.add_valve("Y");
  g.connect("v1", "X");
  g.connect("X", "Y");
  g.connect("Y", "v2");
  EXPECT_EQ(g.connected_volumes({{"X", ValveState::Open}, {"Y", ValveState::Closed}}).size(), 2u);
  EXPECT_EQ(g.connected_volumes({{"X", ValveState::Open}, {"Y", ValveState::Open}}).size(), 1u);
}

TEST(NetworkGraph, ConnectedToAnswersForOneVolume) {
  auto g = line();
  States s{{"A", ValveState::Open}, {"C", ValveState::Open}};
  EXPECT_EQ(g.connected_to("air", s), (Names{"air", "bone", "prep"}));
  EXPECT_EQ(g.connected_to("spec", s), (Names{"spec"}));
  EXPECT_TRUE(g.connected_to("nope", s).empty());
}

TEST(NetworkGraph, ConnectUnknownNodeFails) {
  NetworkGraph g;
  g.add_volume("v");
  auto r = g.connect("v", "ghost");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_TRUE(g.connect("v", "v").has_value() == false);
}

TEST(NetworkGraph, NodeKindQueries) {
  auto g = line();
  EXPECT_TRUE(g.is_valve("A"));
  EXPECT_FALSE(g.is_valve("bone"));
  EXPECT_TRUE(g.contains("bone"));
  EXPECT_FALSE(g.contains("ghost"));
  EXPECT_EQ(g.volumes(), (Names{"air", "bone", "prep", "spec"}));
  EXPECT_EQ(g.valves(), (Names{"A", "B", "C"}));
  EXPECT_EQ(g.neighbors("prep"), (Names{"A", "B", "C"}));
}

TEST(NetworkGraph, BuildsFromCanvasIncludingTeesCrossesElbows) {
  auto c = canvas::load_canvas_from_string(R"toml(
[[valve]]
name = "A"
pos = [0, 0]
[[rough_valve]]
name = "R"
pos = [0, 0]
[[manual_valve]]
name = "M"
pos = [0, 0]
[[switch]]
name = "pump"
pos = [0, 0]
[[stage]]
name = "bone"
pos = [0, 0]
[[stage]]
name = "prep"
pos = [0, 0]
[[stage]]
name = "spec"
pos = [0, 0]
[[stage]]
name = "turbo"
pos = [0, 0]
[[pipette]]
name = "air"
pos = [0, 0]
[[gauge]]
name = "IG1"
pos = [0, 0]
[[stage]]
name = "x"
pos = [0, 0]
[[connection]]
start = "bone"
end = "A"
[[elbow]]
start = "A"
end = "prep"
corner = "lr"
[[tee]]
left = "prep"
right = "R"
mid = "IG1"
[[connection]]
start = "R"
end = "turbo"
[[cross]]
left = "prep"
right = "M"
top = "x"
bottom = "air"
[[connection]]
start = "M"
end = "spec"
)toml",
                                           "c.toml");
  ASSERT_TRUE(c.has_value()) << c.error().what;
  const auto g = NetworkGraph::from_canvas(*c);

  EXPECT_EQ(g.valves(), (Names{"A", "M", "R"}));
  EXPECT_FALSE(g.contains("pump"));
  EXPECT_TRUE(g.contains("IG1"));

  // Tee joins prep, R, IG1 without a valve between them.
  EXPECT_EQ(g.connected_to("IG1", {}), (Names{"IG1", "air", "prep", "x"}));
  EXPECT_EQ(g.connected_to("bone", {{"A", ValveState::Open}}), (Names{"IG1", "air", "bone", "prep", "x"}));
  EXPECT_EQ(g.connected_to("turbo", {{"R", ValveState::Open}, {"M", ValveState::Open}}),
            (Names{"IG1", "air", "prep", "spec", "turbo", "x"}));
}

}  // namespace
}  // namespace pychron::systems
