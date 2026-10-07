#include "pychron/sim/gas_network.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "pychron/core/error.hpp"
#include "pychron/sim/gas.hpp"

namespace {

using namespace pychron;
using namespace pychron::sim;

constexpr std::size_t kAr36 = index(Species::Ar36);
constexpr std::size_t kAr40 = index(Species::Ar40);
constexpr std::size_t kActive = index(Species::Active);

// |a - b| <= rel * max(|a|, |b|).
::testing::AssertionResult near_rel(double a, double b, double rel) {
  const double scale = std::max(std::abs(a), std::abs(b));
  if (std::abs(a - b) <= rel * scale) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure() << a << " and " << b << " differ by " << std::abs(a - b) / scale
                                       << " relative, more than " << rel;
}

// Every species of `a` within `rel` of the same species of `b`.
::testing::AssertionResult near_rel(const Composition& a, const Composition& b, double rel) {
  for (std::size_t i = 0; i < kSpeciesCount; ++i) {
    const auto one = near_rel(a[i], b[i], rel);
    if (!one) return ::testing::AssertionFailure() << kSpeciesName[i] << ": " << one.message();
  }
  return ::testing::AssertionSuccess();
}

// The same numbers from every standard library: the distributions in <random>
// are not specified bit for bit, the engine is.
struct Random {
  std::mt19937 engine{0x5eed};
  double unit() { return static_cast<double>(engine()) / 4294967296.0; }
  double log_uniform(double lo, double hi) { return lo * std::pow(hi / lo, unit()); }
  std::size_t below(std::size_t n) { return static_cast<std::size_t>(engine() % n); }
};

// Air whose Ar40 partial pressure is `p40`.
Composition air(double p40) { return with_ar40(air_ratios(), p40); }

// The network of a topology the test knows to be good. One that is refused
// leaves nothing to go on with: the test stops there.
GasNetwork must(GasTopology topology) {
  auto network = GasNetwork::make(std::move(topology));
  if (!network.has_value()) {
    std::cerr << "GasNetwork::make refused a good topology: " << to_string(network.error()) << std::endl;
    std::abort();
  }
  return std::move(*network);
}

// Partial pressures of a volume that is there; NaN, and a failure, if not.
Composition at(const GasNetwork& network, std::string_view volume) {
  const auto pressures = network.partial_pressures(volume);
  if (pressures.has_value()) return *pressures;
  ADD_FAILURE() << to_string(pressures.error());
  Composition none;
  none.fill(std::numeric_limits<double>::quiet_NaN());
  return none;
}

double ar40(const GasNetwork& network, std::string_view volume) { return at(network, volume)[kAr40]; }

::testing::AssertionResult refused(const Result<GasNetwork>& network, std::string_view naming) {
  if (network.has_value()) return ::testing::AssertionFailure() << "accepted";
  if (network.error().kind != ErrorKind::Config) return ::testing::AssertionFailure() << to_string(network.error());
  if (network.error().what.find(naming) == std::string::npos) {
    return ::testing::AssertionFailure() << "'" << network.error().what << "' does not name " << naming;
  }
  return ::testing::AssertionSuccess();
}

template <class T>
::testing::AssertionResult config_error(const Result<T>& result) {
  if (result.has_value()) return ::testing::AssertionFailure() << "accepted";
  if (result.error().kind != ErrorKind::Config) return ::testing::AssertionFailure() << to_string(result.error());
  return ::testing::AssertionSuccess();
}

// tank - T - line: a tank of air and an empty line, the valve of 0.1 L/s.
GasTopology tank_and_line() {
  GasTopology topology;
  topology.volumes = {{"tank", 0.5, air(2e-7), {}, {}}, {"line", 0.05, {}, {}, {}}};
  topology.valves = {{"T", 0.1}};
  topology.edges = {{"tank", "T"}, {"T", "line"}};
  return topology;
}

constexpr double kTank = 1.0;
constexpr double kPipette = 1e-4;
constexpr double kLine = 0.1;

// tank - T - pipette - L - line.
GasTopology pipette_line() {
  GasTopology topology;
  topology.volumes = {{"tank", kTank, air(1e-6), {}, {}},
                      {"pipette", kPipette, {}, {}, {}},
                      {"line", kLine, {}, {}, {}}};
  topology.valves = {{"T", 0.1}, {"L", 0.1}};
  topology.edges = {{"tank", "T"}, {"T", "pipette"}, {"pipette", "L"}, {"L", "line"}};
  return topology;
}

// What a script does to take one shot.
void shot(GasNetwork& network) {
  network.set_valve("T", true);
  network.advance(30.0);
  network.set_valve("T", false);
  network.set_valve("L", true);
  network.advance(30.0);
  network.set_valve("L", false);
}

TEST(GasNetwork, ValvesStartClosedAndIsolate) {
  GasTopology topology;
  topology.volumes = {{"a", 0.05, air(1e-6), {}, {}}, {"b", 0.15, air(3e-8), {}, {}}};
  topology.valves = {{"v", 0.1}};
  topology.edges = {{"a", "v"}, {"v", "b"}};
  GasNetwork network = must(topology);

  EXPECT_FALSE(network.valve_open("v"));
  EXPECT_TRUE(network.has_volume("a"));
  EXPECT_TRUE(network.has_volume("b"));
  EXPECT_FALSE(network.has_volume("v"));
  EXPECT_FALSE(network.has_volume("c"));
  // To rounding: a pressure is kept as an amount, p V, and read as n / V.
  EXPECT_TRUE(near_rel(at(network, "a"), air(1e-6), 1e-15));
  EXPECT_TRUE(near_rel(at(network, "b"), air(3e-8), 1e-15));

  network.advance(3600.0);
  EXPECT_TRUE(near_rel(at(network, "a"), air(1e-6), 1e-15));
  EXPECT_TRUE(near_rel(at(network, "b"), air(3e-8), 1e-15));
  const auto whole = network.pressure("a");
  ASSERT_TRUE(whole.has_value());
  EXPECT_TRUE(near_rel(*whole, total(air(1e-6)), 1e-15));
}

TEST(GasNetwork, AnOpenValveEquilibratesEachSpecies) {
  GasNetwork network = must(tank_and_line());
  network.set_valve("T", true);
  EXPECT_TRUE(network.valve_open("T"));
  network.advance(60.0);
  const Composition expected = air(2e-7 * 0.5 / 0.55);
  EXPECT_TRUE(near_rel(at(network, "tank"), expected, 1e-9));
  EXPECT_TRUE(near_rel(at(network, "line"), expected, 1e-9));

  // Closed again, each keeps what it has.
  network.set_valve("T", false);
  EXPECT_FALSE(network.valve_open("T"));
  network.advance(3600.0);
  EXPECT_TRUE(near_rel(at(network, "tank"), expected, 1e-9));
  EXPECT_TRUE(near_rel(at(network, "line"), expected, 1e-9));
}

TEST(GasNetwork, LighterGasArrivesFirst) {
  GasNetwork network = must(tank_and_line());
  network.set_valve("T", true);
  const double t = 0.05;
  network.advance(t);

  // Each species closes the gap at its own rate, C (1 / V1 + 1 / V2) with C
  // the valve's conductance for it: the line holds 1 - exp(-rate t) of what
  // it will, and Ar36 is ahead of Ar40 by the ratio of the two.
  const double rate40 = 0.1 * (1.0 / 0.5 + 1.0 / 0.05);
  const double rate36 = rate40 * std::sqrt(kSpeciesMass[kAr40] / kSpeciesMass[kAr36]);
  const double ahead = std::expm1(-rate36 * t) / std::expm1(-rate40 * t);
  const Composition early = at(network, "line");
  const double enrichment = (early[kAr36] / early[kAr40]) / (1.0 / 298.56);
  EXPECT_GT(enrichment, 1.0001);
  EXPECT_LT(enrichment, 1.06);
  EXPECT_TRUE(near_rel(enrichment, ahead, 1e-9));
  EXPECT_TRUE(near_rel(early[kAr40], 2e-7 * 0.5 / 0.55 * -std::expm1(-rate40 * t), 1e-9));
  // The tank is left that much heavier, by what it gave.
  const Composition left = at(network, "tank");
  EXPECT_LT((left[kAr36] / left[kAr40]) / (1.0 / 298.56), 1.0);

  network.advance(60.0 - t);
  const Composition late = at(network, "line");
  EXPECT_TRUE(near_rel(late[kAr36] / late[kAr40], 1.0 / 298.56, 1e-9));
}

TEST(GasNetwork, APipetteDeliversTankPressureTimesItsVolume) {
  GasNetwork network = must(pipette_line());
  shot(network);

  // In amounts, step by step. The tank shares what it holds with the
  // pipette by volume; the pipette is closed off with its share; the
  // pipette shares that with the line by volume.
  const double in_tank = 1e-6 * kTank;
  const double loaded = in_tank / (kTank + kPipette);  // mbar, tank and pipette
  const double in_pipette = loaded * kPipette;
  const double delivered = in_pipette / (kPipette + kLine);  // mbar, pipette and line
  EXPECT_TRUE(near_rel(ar40(network, "line"), delivered, 1e-9));
  EXPECT_TRUE(near_rel(ar40(network, "pipette"), delivered, 1e-9));
  EXPECT_TRUE(near_rel(ar40(network, "tank"), loaded, 1e-9));
  // The shot is the tank pressure times the pipette volume, all but the
  // part in ten thousand the tank lost in loading it.
  EXPECT_TRUE(near_rel(ar40(network, "line") * (kLine + kPipette), 1e-6 * kPipette, 1.01e-4));
  // And it is air: the valves have long equilibrated every species.
  EXPECT_TRUE(near_rel(at(network, "line"), air(delivered), 1e-9));
}

TEST(GasNetwork, TenShotsDeclineGeometrically) {
  // Ten shots, the line pumped out after each as a run does. The line is
  // emptied; the pipette, closed by then, keeps its share of the shot.
  GasNetwork network = must(pipette_line());
  std::vector<double> delivered;  // mbar L of Ar40 in the line after each shot
  for (int i = 0; i < 10; ++i) {
    shot(network);
    delivered.push_back(ar40(network, "line") * kLine);
    ASSERT_TRUE(network.set_partial_pressures("line", Composition{}).has_value());
  }

  // Each shot leaves the tank at V_tank / (V_tank + V_pipette) of what it
  // was, and so each shot is that much smaller than the one before:
  // 1 / (1 + 1e-4).
  const double decline = kTank / (kTank + kPipette);
  // Allowing for what stays in the pipette: of each shot, the part
  // V_pipette / (V_pipette + V_line) is still in the pipette when it opens
  // to the tank again, and goes back. The tank then loses a little less: in
  // amounts, the tank and pipette together hold (V_tank + that part of
  // V_pipette) of the pressure they held a shot before.
  const double kept = kPipette * kPipette / (kPipette + kLine);  // litres at the loading pressure
  const double exact = (kTank + kept) / (kTank + kPipette);
  EXPECT_TRUE(near_rel(exact, decline, 1e-6));
  for (std::size_t i = 1; i < delivered.size(); ++i) {
    EXPECT_TRUE(near_rel(delivered[i] / delivered[i - 1], decline, 1e-6)) << "shot " << i + 1;
    EXPECT_TRUE(near_rel(delivered[i] / delivered[i - 1], exact, 1e-9)) << "shot " << i + 1;
  }
  // The first, from the two equilibrations.
  EXPECT_TRUE(near_rel(delivered[0], 1e-6 * kTank / (kTank + kPipette) * kPipette / (kPipette + kLine) * kLine, 1e-9));
}

TEST(GasNetwork, ATankValveLeftOpenEmptiesTheTank) {
  GasNetwork network = must(pipette_line());
  network.set_valve("T", true);
  network.set_valve("L", true);
  network.advance(600.0);

  // One pressure everywhere: all the gas over all the volume.
  const Composition shared = scaled(air(1e-6), kTank / (kTank + kPipette + kLine));
  EXPECT_TRUE(near_rel(at(network, "tank"), shared, 1e-9));
  EXPECT_TRUE(near_rel(at(network, "pipette"), shared, 1e-9));
  EXPECT_TRUE(near_rel(at(network, "line"), shared, 1e-9));
  // A thousand shots' worth, not one.
  EXPECT_GT(ar40(network, "line") * kLine, 900.0 * 1e-6 * kPipette);
}

TEST(GasNetwork, APumpTakesAVolumeToItsBase) {
  GasTopology topology;
  topology.volumes = {{"stage", 0.05, air(1e-6), {}, {}}};
  topology.pumps = {{"stage", 50.0, 1e-9, true, true}};
  GasNetwork network = must(topology);

  // On the way: every species at speed / V, from where it was to its share.
  const Composition base = scaled(air_ratios(), 1e-9 / total(air_ratios()));
  const double t = 0.004;
  network.advance(t);
  Composition expected{};
  for (std::size_t i = 0; i < kSpeciesCount; ++i) {
    expected[i] = base[i] + (air(1e-6)[i] - base[i]) * std::exp(-t * 50.0 / 0.05);
  }
  EXPECT_TRUE(near_rel(at(network, "stage"), expected, 1e-9));

  network.advance(1.0 - t);
  const auto whole = network.pressure("stage");
  ASSERT_TRUE(whole.has_value());
  EXPECT_TRUE(near_rel(*whole, 1e-9, 1e-6));
  EXPECT_TRUE(near_rel(at(network, "stage"), base, 1e-6));
  network.advance(1e7);
  EXPECT_TRUE(near_rel(at(network, "stage"), base, 1e-9));
}

TEST(GasNetwork, AGetterRemovesActiveGasOnly) {
  GasTopology topology;
  topology.volumes = {{"getter", 0.05, air(1e-6), {}, {}}};
  GasPump getter;
  getter.volume = "getter";
  getter.speed = 1.0;
  getter.base = 0.0;
  getter.nobles = false;
  topology.pumps = {getter};
  GasNetwork network = must(topology);

  const Composition before = at(network, "getter");
  network.advance(10.0);
  const Composition after = at(network, "getter");
  EXPECT_GE(after[kActive], 0.0);
  EXPECT_LT(after[kActive], 1e-30);
  for (std::size_t i = 0; i < kSpeciesCount; ++i) {
    if (i != kActive) EXPECT_TRUE(near_rel(after[i], before[i], 1e-12)) << kSpeciesName[i];
  }

  // The other way about: a pump of nobles only leaves the active gas, and
  // its base is all argon, in air's proportions.
  GasPump nobles;
  nobles.volume = "getter";
  nobles.speed = 1.0;
  nobles.base = 1e-10;
  nobles.active = false;
  topology.pumps = {nobles};
  GasNetwork other = must(topology);
  other.advance(10.0);
  const Composition left = at(other, "getter");
  EXPECT_TRUE(near_rel(left[kActive], before[kActive], 1e-12));
  EXPECT_TRUE(near_rel(total(left) - left[kActive], 1e-10, 1e-9));
  EXPECT_TRUE(near_rel(left[kAr40] / left[kAr36], 298.56, 1e-9));
}

TEST(GasNetwork, OutgassingRaisesAnIsolatedVolumeLinearly) {
  GasTopology topology;
  topology.volumes = {{"stage", 0.05, air(1e-8), scaled(air(1.0), 1e-13), {}}};
  GasNetwork network = must(topology);

  const Composition before = at(network, "stage");
  network.advance(1000.0);
  const Composition after = at(network, "stage");
  EXPECT_TRUE(near_rel(after[kAr40] - before[kAr40], 1e-13 * 1000.0 / 0.05, 1e-9));
  // As air: every species by its share of the source.
  for (std::size_t i = 0; i < kSpeciesCount; ++i) {
    EXPECT_TRUE(near_rel(after[i] - before[i], air(1.0)[i] * 1e-13 * 1000.0 / 0.05, 1e-9)) << kSpeciesName[i];
  }
  // And it does not matter how the time was cut.
  GasNetwork stepped = must(topology);
  for (int i = 0; i < 1000; ++i) stepped.advance(1.0);
  EXPECT_TRUE(near_rel(at(stepped, "stage"), after, 1e-12));
}

TEST(GasNetwork, FirstOrderLossAndMemory) {
  const double v = 0.05;
  const double loss = 2e-5;
  const double q = 1e-15;
  const double steady = q / (loss * v);  // mbar
  const auto with = [&](double p40) {
    GasTopology topology;
    GasVolume source{"source", v, {}, {}, {}};
    source.initial[kAr40] = p40;
    source.source_per_s[kAr40] = q;
    source.loss_per_s[kAr40] = loss;
    topology.volumes = {source};
    return must(topology);
  };

  // From far above it falls, as the loss says.
  GasNetwork high = with(100.0 * steady);
  high.advance(1e4);
  const double fallen = ar40(high, "source");
  EXPECT_LT(fallen, 100.0 * steady);
  EXPECT_GT(fallen, steady);
  EXPECT_TRUE(near_rel(fallen, steady + 99.0 * steady * std::exp(-loss * 1e4), 1e-9));
  high.advance(2e6);
  EXPECT_TRUE(near_rel(ar40(high, "source"), steady, 1e-9));

  // From nothing it rises to the same place.
  GasNetwork low = with(0.0);
  low.advance(1e4);
  const double risen = ar40(low, "source");
  EXPECT_GT(risen, 0.0);
  EXPECT_LT(risen, steady);
  EXPECT_TRUE(near_rel(risen, steady * -std::expm1(-loss * 1e4), 1e-9));
  low.advance(2e6);
  EXPECT_TRUE(near_rel(ar40(low, "source"), steady, 1e-9));
  // No other species has a source or a loss.
  EXPECT_EQ(at(low, "source")[kAr36], 0.0);
}

TEST(GasNetwork, VolumesJoinedWithoutAValveAreOne) {
  GasTopology topology;
  topology.volumes = {{"A", 0.05, air(1e-6), {}, {}}, {"B", 0.15, {}, {}, {}}, {"C", 0.1, air(3e-7), {}, {}}};
  topology.valves = {{"v", 0.1}};
  topology.edges = {{"A", "B"}, {"B", "v"}, {"v", "C"}};
  GasNetwork network = must(topology);

  // One volume from the start, at the volume-weighted mean.
  const Composition joined = scaled(air(1e-6), 0.05 / 0.2);
  EXPECT_TRUE(near_rel(at(network, "A"), joined, 1e-15));
  EXPECT_EQ(at(network, "A"), at(network, "B"));
  EXPECT_TRUE(near_rel(at(network, "C"), air(3e-7), 1e-15));
  network.advance(100.0);
  EXPECT_EQ(at(network, "A"), at(network, "B"));
  EXPECT_TRUE(near_rel(at(network, "A"), joined, 1e-15));

  // Of the size of both: an amount put into either raises both by the
  // amount over 0.2 L.
  Composition some{};
  some[kAr40] = 1e-9;
  ASSERT_TRUE(network.inject("B", some).has_value());
  EXPECT_EQ(at(network, "A"), at(network, "B"));
  EXPECT_TRUE(near_rel(ar40(network, "A"), joined[kAr40] + 1e-9 / 0.2, 1e-15));

  // The valve equilibrates A + B with C by total volume, while it is
  // happening and after.
  network.set_valve("v", true);
  network.advance(0.1);
  EXPECT_EQ(at(network, "A"), at(network, "B"));
  network.advance(100.0);
  const double all = (joined[kAr40] * 0.2 + 1e-9 + 3e-7 * 0.1) / 0.3;
  EXPECT_EQ(at(network, "A"), at(network, "B"));
  EXPECT_TRUE(near_rel(ar40(network, "A"), all, 1e-9));
  EXPECT_TRUE(near_rel(ar40(network, "C"), all, 1e-9));
  const double all36 = (joined[kAr36] * 0.2 + air(3e-7)[kAr36] * 0.1) / 0.3;
  EXPECT_TRUE(near_rel(at(network, "B")[kAr36], all36, 1e-9));

  // Setting either sets both.
  ASSERT_TRUE(network.set_partial_pressures("A", air(5e-8)).has_value());
  EXPECT_EQ(at(network, "B"), at(network, "A"));
  EXPECT_TRUE(near_rel(at(network, "B"), air(5e-8), 1e-15));
}

TEST(GasNetwork, MergedVolumesSumSourcesWeighLossesAndKeepPumps) {
  // Sources add; a loss in one part acts on that part's share of the gas.
  const double q1 = 1e-15;
  const double q2 = 3e-15;
  const double loss = 2e-3;
  GasTopology topology;
  GasVolume a{"A", 0.05, {}, {}, {}};
  a.source_per_s[kAr40] = q1;
  a.loss_per_s[kAr40] = loss;
  GasVolume b{"B", 0.15, {}, {}, {}};
  b.source_per_s[kAr40] = q2;
  topology.volumes = {a, b};
  topology.edges = {{"B", "A"}};
  GasNetwork network = must(topology);

  const double merged_loss = loss * 0.05 / 0.2;
  const double steady = (q1 + q2) / (merged_loss * 0.2);  // mbar
  network.advance(100.0);
  EXPECT_TRUE(near_rel(ar40(network, "B"), steady * -std::expm1(-merged_loss * 100.0), 1e-9));
  network.advance(1e6);
  EXPECT_TRUE(near_rel(ar40(network, "A"), steady, 1e-9));
  EXPECT_EQ(at(network, "A"), at(network, "B"));

  // A pump on each part: both pump the whole, at their speeds over its
  // size, and two pumps on one volume add.
  GasTopology pumped;
  pumped.volumes = {{"A", 0.05, air(1e-6), {}, {}}, {"B", 0.15, air(1e-6), {}, {}}};
  pumped.edges = {{"A", "B"}};
  GasPump turbo{"A", 50.0, 1e-9, true, true};
  GasPump getter{"B", 1.0, 0.0, false, true};
  pumped.pumps = {turbo, getter};
  GasNetwork both = must(pumped);
  const Composition base = scaled(air_ratios(), 1e-9 / total(air_ratios()));
  const double t = 0.004;
  both.advance(t);
  // Argon sees the turbo alone.
  EXPECT_TRUE(near_rel(ar40(both, "B"), base[kAr40] + (1e-6 - base[kAr40]) * std::exp(-t * 50.0 / 0.2), 1e-9));
  // Active gas sees both: it goes at 51 L/s to where the turbo's base inflow
  // meets them, 50 / 51 of its share.
  const double active_end = base[kActive] * 50.0 / 51.0;
  EXPECT_TRUE(near_rel(at(both, "A")[kActive],
                       active_end + (air(1e-6)[kActive] - active_end) * std::exp(-t * 51.0 / 0.2), 1e-9));
  both.advance(10.0);
  EXPECT_TRUE(near_rel(at(both, "A")[kActive], active_end, 1e-9));
  EXPECT_TRUE(near_rel(ar40(both, "A"), base[kAr40], 1e-9));
}

TEST(GasNetwork, InjectAddsExactlyTheAmount) {
  GasNetwork network = must(tank_and_line());
  ASSERT_TRUE(network.set_partial_pressures("line", air(1e-9)).has_value());
  const Composition before = at(network, "line");
  const Composition tank = at(network, "tank");
  const Composition c = scaled(cocktail_ratios(), 1e-13);
  ASSERT_TRUE(network.inject("line", c).has_value());
  const Composition after = at(network, "line");
  for (std::size_t i = 0; i < kSpeciesCount; ++i) {
    EXPECT_DOUBLE_EQ(after[i], before[i] + c[i] / 0.05) << kSpeciesName[i];
  }
  EXPECT_EQ(at(network, "tank"), tank);

  // Nothing but an amount of gas goes in, and only into a volume.
  Composition bad{};
  bad[kAr40] = -1e-13;
  EXPECT_TRUE(config_error(network.inject("line", bad)));
  bad[kAr40] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(config_error(network.inject("line", bad)));
  bad[kAr40] = std::numeric_limits<double>::infinity();
  EXPECT_TRUE(config_error(network.inject("line", bad)));
  EXPECT_TRUE(config_error(network.inject("T", c)));
  EXPECT_TRUE(config_error(network.inject("nowhere", c)));
  EXPECT_EQ(at(network, "line"), after);
}

TEST(GasNetwork, SetsAndReadsPressuresOfVolumesOnly) {
  GasNetwork network = must(tank_and_line());
  ASSERT_TRUE(network.set_partial_pressures("line", air(4e-8)).has_value());
  EXPECT_TRUE(near_rel(at(network, "line"), air(4e-8), 1e-15));
  const auto whole = network.pressure("line");
  ASSERT_TRUE(whole.has_value());
  EXPECT_TRUE(near_rel(*whole, total(air(4e-8)), 1e-15));

  Composition bad = air(4e-8);
  bad[kActive] = -1.0;
  EXPECT_TRUE(config_error(network.set_partial_pressures("line", bad)));
  bad[kActive] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(config_error(network.set_partial_pressures("line", bad)));
  bad[kActive] = std::numeric_limits<double>::infinity();
  EXPECT_TRUE(config_error(network.set_partial_pressures("line", bad)));
  EXPECT_TRUE(config_error(network.set_partial_pressures("nowhere", air(1e-9))));
  EXPECT_TRUE(config_error(network.set_partial_pressures("T", air(1e-9))));
  EXPECT_TRUE(config_error(network.partial_pressures("nowhere")));
  EXPECT_TRUE(config_error(network.partial_pressures("T")));
  EXPECT_TRUE(config_error(network.pressure("nowhere")));
  EXPECT_TRUE(near_rel(at(network, "line"), air(4e-8), 1e-15));

  // A step that is not a time is no step.
  const Composition before = at(network, "line");
  network.set_valve("T", true);
  network.advance(0.0);
  network.advance(-5.0);
  network.advance(std::numeric_limits<double>::quiet_NaN());
  network.advance(std::numeric_limits<double>::infinity());
  EXPECT_EQ(at(network, "line"), before);
}

TEST(GasNetwork, AValveWithNoPhysicsIsTracked) {
  GasTopology topology;
  topology.volumes = {{"a", 0.05, air(1e-6), {}, {}}, {"b", 0.15, air(3e-8), {}, {}}, {"c", 0.1, {}, {}, {}}};
  // `spare` joins nothing; `tee` joins three volumes; `stub` has one volume
  // and an end that is not there; `loop` has both ends on one volume.
  topology.valves = {{"spare", 0.1}, {"tee", 0.1}, {"stub", 0.1}, {"loop", 0.1}, {"v", 0.1}};
  topology.edges = {{"a", "tee"},  {"tee", "b"},  {"c", "tee"},  {"a", "stub"}, {"stub", "nowhere"},
                    {"a", "loop"}, {"loop", "a"}, {"nobody", "b"}, {"a", "v"},  {"v", "b"}};
  GasNetwork network = must(topology);

  for (const char* name : {"spare", "tee", "stub", "loop"}) {
    EXPECT_FALSE(network.valve_open(name)) << name;
    network.set_valve(name, true);
    EXPECT_TRUE(network.valve_open(name)) << name;
  }
  network.advance(3600.0);
  EXPECT_TRUE(near_rel(at(network, "a"), air(1e-6), 1e-15));
  EXPECT_TRUE(near_rel(at(network, "b"), air(3e-8), 1e-15));
  EXPECT_EQ(total(at(network, "c")), 0.0);
  network.set_valve("spare", false);
  EXPECT_FALSE(network.valve_open("spare"));
  EXPECT_TRUE(network.valve_open("tee"));

  // A name that is no valve: ignored, and never open.
  network.set_valve("nobody", true);
  EXPECT_FALSE(network.valve_open("nobody"));
  network.set_valve("a", true);
  EXPECT_FALSE(network.valve_open("a"));
  EXPECT_FALSE(network.has_volume("nowhere"));
  network.advance(3600.0);
  EXPECT_TRUE(near_rel(at(network, "a"), air(1e-6), 1e-15));

  // The one real valve among them still works.
  network.set_valve("v", true);
  network.advance(60.0);
  EXPECT_TRUE(near_rel(ar40(network, "a"), (1e-6 * 0.05 + 3e-8 * 0.15) / 0.2, 1e-9));
  EXPECT_TRUE(near_rel(ar40(network, "b"), (1e-6 * 0.05 + 3e-8 * 0.15) / 0.2, 1e-9));
}

TEST(GasNetwork, AddsAnIsolatedVolume) {
  GasNetwork network = must(tank_and_line());
  network.set_valve("T", true);
  network.advance(0.05);
  const Composition line = at(network, "line");

  GasVolume gauge{"gauge", 0.02, air(5e-9), {}, {}};
  gauge.source_per_s[kAr40] = 1e-13;
  ASSERT_TRUE(network.add_volume(gauge).has_value());
  EXPECT_TRUE(network.has_volume("gauge"));
  EXPECT_TRUE(near_rel(at(network, "gauge"), air(5e-9), 1e-15));
  // What was there is as it was, valve and all.
  EXPECT_EQ(at(network, "line"), line);
  EXPECT_TRUE(network.valve_open("T"));

  network.advance(60.0);
  EXPECT_TRUE(near_rel(at(network, "line"), air(2e-7 * 0.5 / 0.55), 1e-9));
  EXPECT_TRUE(near_rel(ar40(network, "gauge"), 5e-9 + 1e-13 * 60.0 / 0.02, 1e-12));
  EXPECT_TRUE(near_rel(at(network, "gauge")[kAr36], air(5e-9)[kAr36], 1e-15));

  // A name is used once, by a volume or by a valve.
  EXPECT_TRUE(config_error(network.add_volume({"gauge", 0.02, {}, {}, {}})));
  EXPECT_TRUE(config_error(network.add_volume({"line", 0.02, {}, {}, {}})));
  EXPECT_TRUE(config_error(network.add_volume({"T", 0.02, {}, {}, {}})));
  EXPECT_TRUE(config_error(network.add_volume({"", 0.02, {}, {}, {}})));
  EXPECT_TRUE(config_error(network.add_volume({"zero", 0.0, {}, {}, {}})));
  GasVolume bad{"bad", 0.02, {}, {}, {}};
  bad.initial[kAr40] = -1e-9;
  EXPECT_TRUE(config_error(network.add_volume(bad)));
  EXPECT_FALSE(network.has_volume("zero"));
  EXPECT_FALSE(network.has_volume("bad"));
}

TEST(GasNetwork, RefusesBadTopology) {
  const auto good = [] {
    GasTopology topology = tank_and_line();
    topology.pumps = {{"line", 50.0, 1e-9, true, true}};
    return topology;
  };
  ASSERT_TRUE(GasNetwork::make(good()).has_value());
  ASSERT_TRUE(GasNetwork::make(GasTopology{}).has_value());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  GasTopology topology = good();
  topology.volumes.push_back({"tank", 0.1, {}, {}, {}});
  EXPECT_TRUE(refused(GasNetwork::make(topology), "tank")) << "a duplicate volume name";

  topology = good();
  topology.volumes[1].litres = 0.0;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "line")) << "a zero volume";

  topology = good();
  topology.pumps[0].volume = "stage";
  EXPECT_TRUE(refused(GasNetwork::make(topology), "stage")) << "a pump on an unknown volume";

  topology = good();
  topology.valves[0].conductance = -0.1;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "T")) << "a negative conductance";

  // And the rest of what a description can get wrong.
  topology = good();
  topology.volumes[0].litres = -0.5;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "tank")) << "a negative volume";
  topology = good();
  topology.volumes[0].litres = nan;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "tank")) << "a NaN volume";
  topology = good();
  topology.volumes[0].litres = inf;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "tank")) << "an infinite volume";
  topology = good();
  topology.volumes[0].name.clear();
  EXPECT_TRUE(refused(GasNetwork::make(topology), "name")) << "a volume with no name";
  topology = good();
  topology.volumes[0].initial[kAr36] = -1e-9;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "tank")) << "a negative pressure";
  topology = good();
  topology.volumes[0].initial[kAr36] = nan;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "tank")) << "a NaN pressure";
  topology = good();
  topology.volumes[1].source_per_s[kActive] = -1e-12;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "line")) << "a negative source";
  topology = good();
  topology.volumes[1].source_per_s[kActive] = inf;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "line")) << "an infinite source";
  topology = good();
  topology.volumes[1].loss_per_s[kAr40] = -2e-5;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "line")) << "a negative loss";
  topology = good();
  topology.volumes[1].loss_per_s[kAr40] = nan;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "line")) << "a NaN loss";

  topology = good();
  topology.valves.push_back({"T", 0.2});
  EXPECT_TRUE(refused(GasNetwork::make(topology), "T")) << "a duplicate valve name";
  topology = good();
  topology.valves.push_back({"line", 0.2});
  EXPECT_TRUE(refused(GasNetwork::make(topology), "line")) << "a valve named as a volume is";
  topology = good();
  topology.valves[0].name.clear();
  EXPECT_TRUE(refused(GasNetwork::make(topology), "name")) << "a valve with no name";
  topology = good();
  topology.valves[0].conductance = nan;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "T")) << "a NaN conductance";
  topology = good();
  topology.valves[0].conductance = inf;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "T")) << "an infinite conductance";

  topology = good();
  topology.pumps[0].speed = -50.0;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "line")) << "a negative pump speed";
  topology = good();
  topology.pumps[0].speed = nan;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "line")) << "a NaN pump speed";
  topology = good();
  topology.pumps[0].base = -1e-9;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "line")) << "a negative base pressure";
  topology = good();
  topology.pumps[0].base = inf;
  EXPECT_TRUE(refused(GasNetwork::make(topology), "line")) << "an infinite base pressure";
  topology = good();
  topology.pumps[0].volume = "T";
  EXPECT_TRUE(refused(GasNetwork::make(topology), "T")) << "a pump on a valve";

  // What is allowed: a valve that conducts nothing, a pump with no base, a
  // pump that is switched off both ways.
  topology = good();
  topology.valves[0].conductance = 0.0;
  topology.pumps[0].base = 0.0;
  topology.pumps.push_back({"tank", 5.0, 1e-9, false, false});
  auto network = GasNetwork::make(topology);
  ASSERT_TRUE(network.has_value());
  network->set_valve("T", true);
  network->advance(3600.0);
  EXPECT_TRUE(near_rel(at(*network, "tank"), air(2e-7), 1e-15));
  EXPECT_EQ(total(at(*network, "line")), 0.0);
}

TEST(GasNetwork, ManyTogglesStayPhysical) {
  // A whole line: tank and pipette, a stage that leaks, a getter, an inlet,
  // a spectrometer that uses up what it measures, and a pumped manifold
  // reached from two sides.
  GasTopology topology;
  GasVolume stage{"stage", 0.1, {}, scaled(air(1.0), 1e-12), {}};
  GasVolume spectrometer{"spectrometer", 1.0, {}, {}, {}};
  spectrometer.loss_per_s[kAr40] = 2e-5;
  topology.volumes = {{"tank", 1.0, air(1e-6), {}, {}},
                      {"pipette", 1e-4, {}, {}, {}},
                      stage,
                      {"getter", 0.05, {}, scaled(air(1.0), 1e-15), {}},
                      {"inlet", 0.02, air(1e-3), {}, {}},
                      spectrometer,
                      {"manifold", 0.5, {}, {}, {}}};
  topology.valves = {{"T", 0.1}, {"L", 0.1}, {"G", 1.0}, {"I", 1e-3}, {"M", 1000.0}, {"P", 10.0}, {"Q", 1e-6}};
  topology.edges = {{"tank", "T"},  {"T", "pipette"},      {"pipette", "L"}, {"L", "stage"},        {"stage", "G"},
                    {"G", "getter"}, {"stage", "I"},        {"I", "inlet"},   {"inlet", "M"},        {"M", "spectrometer"},
                    {"stage", "P"},  {"P", "manifold"},     {"spectrometer", "Q"}, {"Q", "manifold"}};
  topology.pumps = {{"manifold", 50.0, 1e-9, true, true}, {"getter", 1.0, 0.0, false, true}};
  GasNetwork network = must(topology);

  // No pressure can be above the highest there was, the inlet's 0.1 mbar of
  // air, by more than the fastest source could have added to its own volume
  // alone in the time gone by.
  const double fastest = std::max(total(topology.volumes[2].source_per_s) / topology.volumes[2].litres,
                                  total(topology.volumes[3].source_per_s) / topology.volumes[3].litres);
  double elapsed = 0.0;

  Random random;
  for (int step = 0; step < 10000; ++step) {
    const GasValve& valve = topology.valves[random.below(topology.valves.size())];
    network.set_valve(valve.name, !network.valve_open(valve.name));
    const double dt = random.log_uniform(1e-6, 1e5);
    network.advance(dt);
    elapsed += dt;
    for (const GasVolume& volume : topology.volumes) {
      const auto pressures = network.partial_pressures(volume.name);
      ASSERT_TRUE(pressures.has_value());
      for (std::size_t i = 0; i < kSpeciesCount; ++i) {
        ASSERT_TRUE(std::isfinite((*pressures)[i])) << volume.name << " " << kSpeciesName[i] << " at step " << step;
        ASSERT_GE((*pressures)[i], 0.0) << volume.name << " " << kSpeciesName[i] << " at step " << step;
      }
      const auto whole = network.pressure(volume.name);
      ASSERT_TRUE(whole.has_value());
      ASSERT_TRUE(std::isfinite(*whole)) << volume.name << " at step " << step;
      ASSERT_LE(*whole, (total(air(1e-3)) + fastest * elapsed) * (1.0 + 1e-9))
          << volume.name << " at step " << step;
    }
  }
}

}  // namespace
