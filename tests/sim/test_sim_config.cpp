#include "pychron/sim/sim_config.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/sim/gas.hpp"
#include "pychron/sim/sim_system.hpp"

namespace {

using namespace pychron;
using namespace std::chrono_literals;
using sim::Composition;
using sim::SimRole;
using sim::SimSettings;
using sim::SimSystem;
using sim::SimTopology;
using sim::Species;

constexpr std::size_t kAr36 = sim::index(Species::Ar36);
constexpr std::size_t kAr37 = sim::index(Species::Ar37);
constexpr std::size_t kAr38 = sim::index(Species::Ar38);
constexpr std::size_t kAr39 = sim::index(Species::Ar39);
constexpr std::size_t kAr40 = sim::index(Species::Ar40);
constexpr std::size_t kActive = sim::index(Species::Active);

// The names the spec's example (section 6.2) speaks of.
//   air_tank --inlet-- bone --C-- turbo        cocktail_tank
SimTopology lab() {
  SimTopology t;
  t.volumes = {{"air_tank", 0.0, SimRole::Tank},
               {"cocktail_tank", 0.0, SimRole::Tank},
               {"bone", 12.5, SimRole::Plain},
               {"turbo", 0.0, SimRole::Pump}};
  t.valves = {"inlet", "C"};
  t.edges = {{"air_tank", "inlet"}, {"inlet", "bone"}, {"bone", "C"}, {"C", "turbo"}};
  return t;
}

// A sim.toml of its own for each test, gone with it.
class SimConfig : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() / (std::string("pychron-sim-config-") + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path write(std::string_view text) const {
    const auto file = dir_ / "sim.toml";
    std::ofstream(file, std::ios::binary) << text;
    return file;
  }

  Result<SimSettings> load(std::string_view text, SimSettings base = {}) const {
    return sim::load_sim_settings(write(text), lab(), std::move(base));
  }

  // The file is refused, and says where: its name and `key`.
  void expect_refused(std::string_view text, std::string_view key) const {
    const auto loaded = load(text);
    ASSERT_FALSE(loaded) << "accepted:\n" << text;
    EXPECT_EQ(loaded.error().kind, ErrorKind::Config);
    EXPECT_NE(loaded.error().what.find("sim.toml"), std::string::npos) << loaded.error().what;
    EXPECT_NE(loaded.error().what.find(std::string(key) + ":"), std::string::npos) << loaded.error().what;
  }

  // The file is refused at `key`, and the message there says `text`.
  void expect_said(std::string_view file_text, std::string_view key, std::string_view text) const {
    const auto loaded = load(file_text);
    ASSERT_FALSE(loaded) << "accepted:\n" << file_text;
    const std::string& what = loaded.error().what;
    const auto at = what.find(std::string(key) + ": ");
    ASSERT_NE(at, std::string::npos) << what;
    const auto end = what.find('\n', at);
    const std::string line = what.substr(at, end == std::string::npos ? end : end - at);
    EXPECT_NE(line.find(text), std::string::npos) << line;
  }

  std::filesystem::path dir_;
};

// Every field of the two is the same.
void expect_same(const SimSettings& a, const SimSettings& b) {
  EXPECT_EQ(a.default_pressure, b.default_pressure);
  EXPECT_EQ(a.initial_pressures, b.initial_pressures);
  ASSERT_EQ(a.pumps.size(), b.pumps.size());
  for (const auto& [name, pump] : a.pumps) {
    ASSERT_TRUE(b.pumps.contains(name)) << name;
    EXPECT_EQ(pump.base, b.pumps.at(name).base) << name;
    EXPECT_EQ(pump.tau, b.pumps.at(name).tau) << name;
  }
  EXPECT_EQ(a.pump_speeds, b.pump_speeds);
  EXPECT_EQ(a.noise, b.noise);
  EXPECT_EQ(a.seed, b.seed);
  EXPECT_EQ(a.default_volume_cc, b.default_volume_cc);
  EXPECT_EQ(a.valve_conductance, b.valve_conductance);
  EXPECT_EQ(a.outgassing, b.outgassing);
  EXPECT_EQ(a.outgassing_active, b.outgassing_active);
  EXPECT_EQ(a.compositions, b.compositions);
  EXPECT_EQ(a.conductances, b.conductances);
  EXPECT_EQ(a.leaks, b.leaks);
  EXPECT_EQ(a.getters, b.getters);
  EXPECT_EQ(a.sizes, b.sizes);
  EXPECT_EQ(a.tank_argon40, b.tank_argon40);
  EXPECT_EQ(a.pipette_cc, b.pipette_cc);
  EXPECT_EQ(a.gauge_cc, b.gauge_cc);
  EXPECT_EQ(a.pipe_cc, b.pipe_cc);
  EXPECT_EQ(a.pump_speed, b.pump_speed);
  EXPECT_EQ(a.pump_base, b.pump_base);
  EXPECT_EQ(a.getter_speed, b.getter_speed);
  EXPECT_EQ(a.source.sensitivity, b.source.sensitivity);
  EXPECT_EQ(a.source.consumption, b.source.consumption);
  EXPECT_EQ(a.source.memory_fa_per_s, b.source.memory_fa_per_s);
  ASSERT_EQ(a.detectors.size(), b.detectors.size());
  for (const auto& [name, detector] : a.detectors) {
    ASSERT_TRUE(b.detectors.contains(name)) << name;
    EXPECT_EQ(detector.baseline, b.detectors.at(name).baseline) << name;
    EXPECT_EQ(detector.drift_per_h, b.detectors.at(name).drift_per_h) << name;
  }
  EXPECT_EQ(a.named, b.named);
}

TEST_F(SimConfig, AnEmptyFileGivesTheDefaults) {
  const auto loaded = load("");
  ASSERT_TRUE(loaded) << loaded.error().what;
  const SimSettings& s = *loaded;
  // The plan's numbers, which configs/examples/sim.toml repeats.
  EXPECT_EQ(s.default_pressure, 1e-8);
  EXPECT_EQ(s.default_volume_cc, 50.0);
  EXPECT_EQ(s.valve_conductance, 0.1);
  EXPECT_EQ(s.outgassing, 5e-13);
  EXPECT_EQ(s.outgassing_active, 1e-10);
  EXPECT_EQ(s.noise, 0.01);
  EXPECT_EQ(s.seed, std::uint64_t{0x5eed});
  EXPECT_EQ(s.tank_argon40, 3e-5);
  EXPECT_EQ(s.pipette_cc, 0.1);
  EXPECT_EQ(s.gauge_cc, 1.0);
  EXPECT_EQ(s.pipe_cc, 1.0);
  EXPECT_EQ(s.pump_speed, 50.0);
  EXPECT_EQ(s.pump_base, 1e-9);
  EXPECT_EQ(s.getter_speed, 1.0);
  EXPECT_EQ(s.source.sensitivity, 1e12);
  EXPECT_EQ(s.source.consumption, 2e-5);
  EXPECT_EQ(s.source.memory_fa_per_s, 0.01);
  EXPECT_TRUE(s.initial_pressures.empty());
  EXPECT_TRUE(s.compositions.empty());
  EXPECT_TRUE(s.pumps.empty());
  EXPECT_TRUE(s.pump_speeds.empty());
  EXPECT_TRUE(s.conductances.empty());
  EXPECT_TRUE(s.leaks.empty());
  EXPECT_TRUE(s.getters.empty());
  EXPECT_TRUE(s.sizes.empty());
  EXPECT_TRUE(s.detectors.empty());
  EXPECT_TRUE(s.named.empty());
  // And say which file they are, for whoever checks its detectors' names.
  EXPECT_EQ(s.file, (dir_ / "sim.toml").generic_string());
  EXPECT_TRUE(SimSettings{}.file.empty());
}

TEST_F(SimConfig, ReadsEveryKeyOfTheSpecExample) {
  const auto loaded = load(R"(
[defaults]
pressure = 1e-9            # mbar, every volume not listed
volume_cc = 50
valve_conductance = 0.1    # L/s for Ar40; tau = V / C
outgassing = 1e-13         # mbar L / s per litre
noise = 0.01               # relative 1-sigma on gauges
seed = 0x5eed

[compositions.cocktail]    # ratios to Ar36
Ar40 = 298.56
Ar39 = 20
Ar38 = 0.1885
Ar37 = 0.5
active = 0

[volumes.air_tank]
composition = "air"
argon40 = 2e-7             # mbar of Ar40; the rest follows the composition

[volumes.cocktail_tank]
composition = "cocktail"
argon40 = 2e-7

[volumes.bone]
leak = 0                   # mbar L / s of air

[valves.inlet]
conductance = 0.05

[pumps.turbo]
speed = 50                 # L/s
base = 1e-9

[spectrometer]
sensitivity = 1e12         # fA per mbar of an isotope in the source
consumption = 2e-5         # 1/s
memory_fA_per_s = 0.01     # as Ar40

[detectors.H1]
baseline = 50
baseline_drift_per_h = 2
)");
  ASSERT_TRUE(loaded) << loaded.error().what;
  const SimSettings& s = *loaded;
  EXPECT_EQ(s.default_pressure, 1e-9);
  EXPECT_EQ(s.default_volume_cc, 50.0);
  EXPECT_EQ(s.valve_conductance, 0.1);
  EXPECT_EQ(s.outgassing, 1e-13);
  EXPECT_EQ(s.noise, 0.01);
  EXPECT_EQ(s.seed, std::uint64_t{0x5eed});

  // As the file has it: no Ar36 given, so none.
  ASSERT_TRUE(s.named.contains("cocktail"));
  const Composition& cocktail = s.named.at("cocktail");
  EXPECT_EQ(cocktail[kAr36], 0.0);
  EXPECT_EQ(cocktail[kAr37], 0.5);
  EXPECT_EQ(cocktail[kAr38], 0.1885);
  EXPECT_EQ(cocktail[kAr39], 20.0);
  EXPECT_EQ(cocktail[kAr40], 298.56);
  EXPECT_EQ(cocktail[kActive], 0.0);

  ASSERT_TRUE(s.compositions.contains("air_tank"));
  const Composition& air = s.compositions.at("air_tank");
  EXPECT_EQ(air[kAr40], 2e-7);
  EXPECT_DOUBLE_EQ(air[kAr36], 2e-7 / 298.56);
  EXPECT_DOUBLE_EQ(air[kAr38], 2e-7 * 0.1885 / 298.56);
  EXPECT_DOUBLE_EQ(air[kActive], 106 * (air[kAr36] + air[kAr38] + air[kAr40]));
  ASSERT_TRUE(s.compositions.contains("cocktail_tank"));
  const Composition& shot = s.compositions.at("cocktail_tank");
  EXPECT_EQ(shot[kAr40], 2e-7);
  EXPECT_DOUBLE_EQ(shot[kAr39], 2e-7 * 20 / 298.56);
  EXPECT_DOUBLE_EQ(shot[kAr37], 2e-7 * 0.5 / 298.56);
  EXPECT_EQ(shot[kActive], 0.0);

  ASSERT_TRUE(s.leaks.contains("bone"));
  EXPECT_EQ(s.leaks.at("bone"), 0.0);
  ASSERT_TRUE(s.conductances.contains("inlet"));
  EXPECT_EQ(s.conductances.at("inlet"), 0.05);
  ASSERT_TRUE(s.pumps.contains("turbo"));
  EXPECT_EQ(s.pumps.at("turbo").base, 1e-9);
  ASSERT_TRUE(s.pump_speeds.contains("turbo"));
  EXPECT_EQ(s.pump_speeds.at("turbo"), 50.0);

  EXPECT_EQ(s.source.sensitivity, 1e12);
  EXPECT_EQ(s.source.consumption, 2e-5);
  EXPECT_EQ(s.source.memory_fa_per_s, 0.01);
  ASSERT_TRUE(s.detectors.contains("H1"));
  EXPECT_EQ(s.detectors.at("H1").baseline, 50.0);
  EXPECT_EQ(s.detectors.at("H1").drift_per_h, 2.0);
}

TEST_F(SimConfig, AKeyInTheFileGoesBeforeTheBaseAndTheRestStands) {
  SimSettings base;
  base.noise = 0.0;
  base.valve_conductance = 7.0;
  base.compositions["bone"] = sim::with_ar40(sim::cocktail_ratios(), 1e-6);
  base.initial_pressures["turbo"] = 1e-3;
  const auto loaded = load("[defaults]\nvalve_conductance = 0.5\n[volumes.bone]\npressure = 2e-4\nvolume_cc = 20\n", base);
  ASSERT_TRUE(loaded) << loaded.error().what;
  EXPECT_EQ(loaded->noise, 0.0);
  EXPECT_EQ(loaded->valve_conductance, 0.5);
  EXPECT_FALSE(loaded->compositions.contains("bone")) << "the file's pressure is what bone holds";
  EXPECT_EQ(loaded->initial_pressures.at("bone"), 2e-4);
  EXPECT_EQ(loaded->initial_pressures.at("turbo"), 1e-3);
  EXPECT_EQ(loaded->sizes.at("bone"), 20.0);
}

TEST_F(SimConfig, AVolumesGasIsItsCompositionAtItsArgonOrItsPressure) {
  const auto loaded = load(R"(
[compositions.spike]
Ar36 = 1
Ar38 = 3
[volumes.air_tank]
composition = "cocktail"
[volumes.bone]
composition = "spike"
pressure = 4e-6
[volumes.turbo]
argon40 = 1e-7
)");
  ASSERT_TRUE(loaded) << loaded.error().what;
  // A tank named a composition and no amount: a tank's Ar40.
  EXPECT_EQ(loaded->compositions.at("air_tank")[kAr40], 3e-5);
  EXPECT_DOUBLE_EQ(loaded->compositions.at("air_tank")[kAr39], 3e-5 * 20 / 298.56);
  EXPECT_DOUBLE_EQ(loaded->compositions.at("bone")[kAr36], 1e-6);
  EXPECT_DOUBLE_EQ(loaded->compositions.at("bone")[kAr38], 3e-6);
  EXPECT_EQ(loaded->compositions.at("turbo"), sim::with_ar40(sim::air_ratios(), 1e-7));

  expect_refused("[volumes.bone]\nargon40 = 1e-7\npressure = 1e-6\n", "volumes.bone.pressure");
  expect_refused("[compositions.spike]\nAr36 = 1\n[volumes.bone]\ncomposition = \"spike\"\nargon40 = 1e-7\n",
                 "volumes.bone.argon40");
  // 90 mbar of Ar40 as air is some 1e4 mbar of active gas.
  expect_refused("[volumes.bone]\nargon40 = 100\n", "volumes.bone.argon40");
}

TEST_F(SimConfig, RefusesAnUnknownVolumeValveOrPump) {
  expect_refused("[volumes.nosuch]\nleak = 0\n", "volumes.nosuch");
  expect_refused("[valves.nosuch]\nconductance = 0.1\n", "valves.nosuch");
  expect_refused("[pumps.nosuch]\nspeed = 1\n", "pumps.nosuch");
  // A volume is no valve, and a valve no volume.
  expect_refused("[valves.bone]\nconductance = 0.1\n", "valves.bone");
  expect_refused("[volumes.inlet]\nleak = 0\n", "volumes.inlet");
  expect_refused("[pumps.C]\nspeed = 1\n", "pumps.C");
}

TEST_F(SimConfig, RefusesAnUnknownKey) {
  expect_refused("[default]\npressure = 1e-9\n", "default");
  expect_refused("[defaults]\npresure = 1e-9\n", "defaults.presure");
  expect_refused("[defaults]\ntank_argon40 = 1e-5\n", "defaults.tank_argon40");
  expect_refused("[compositions.mix]\nAr41 = 1\n", "compositions.mix.Ar41");
  expect_refused("[volumes.bone]\nsize = 3\n", "volumes.bone.size");
  expect_refused("[valves.inlet]\nspeed = 3\n", "valves.inlet.speed");
  expect_refused("[pumps.turbo]\ntau = 3\n", "pumps.turbo.tau");
  expect_refused("[spectrometer]\nmemory = 3\n", "spectrometer.memory");
  expect_refused("[detectors.H1]\ndrift = 3\n", "detectors.H1.drift");
  expect_refused("volumes = 3\n", "volumes");
  expect_refused("[volumes]\nbone = 3\n", "volumes.bone");
}

TEST_F(SimConfig, RefusesANegativeOrNonFiniteNumber) {
  for (const char* bad : {"-1", "nan", "inf", "-inf", "\"1\"", "true"}) {
    SCOPED_TRACE(bad);
    const std::string v = bad;
    expect_refused("[defaults]\npressure = " + v + "\n", "defaults.pressure");
    expect_refused("[defaults]\nvolume_cc = " + v + "\n", "defaults.volume_cc");
    expect_refused("[defaults]\npipe_cc = " + v + "\n", "defaults.pipe_cc");
    expect_refused("[defaults]\ngauge_cc = " + v + "\n", "defaults.gauge_cc");
    expect_refused("[defaults]\nvalve_conductance = " + v + "\n", "defaults.valve_conductance");
    expect_refused("[defaults]\noutgassing = " + v + "\n", "defaults.outgassing");
    expect_refused("[defaults]\nnoise = " + v + "\n", "defaults.noise");
    expect_refused("[compositions.mix]\nAr40 = " + v + "\n", "compositions.mix.Ar40");
    expect_refused("[volumes.bone]\nargon40 = " + v + "\n", "volumes.bone.argon40");
    expect_refused("[volumes.bone]\npressure = " + v + "\n", "volumes.bone.pressure");
    expect_refused("[volumes.bone]\nleak = " + v + "\n", "volumes.bone.leak");
    expect_refused("[volumes.bone]\nvolume_cc = " + v + "\n", "volumes.bone.volume_cc");
    expect_refused("[valves.inlet]\nconductance = " + v + "\n", "valves.inlet.conductance");
    expect_refused("[pumps.turbo]\nspeed = " + v + "\n", "pumps.turbo.speed");
    expect_refused("[pumps.turbo]\nbase = " + v + "\n", "pumps.turbo.base");
    expect_refused("[spectrometer]\nsensitivity = " + v + "\n", "spectrometer.sensitivity");
    expect_refused("[spectrometer]\nconsumption = " + v + "\n", "spectrometer.consumption");
    expect_refused("[spectrometer]\nmemory_fA_per_s = " + v + "\n", "spectrometer.memory_fA_per_s");
  }
  // A baseline may be below zero; it may not be no number.
  for (const char* bad : {"nan", "inf", "-inf", "\"1\""}) {
    SCOPED_TRACE(bad);
    const std::string v = bad;
    expect_refused("[detectors.H1]\nbaseline = " + v + "\n", "detectors.H1.baseline");
    expect_refused("[detectors.H1]\nbaseline_drift_per_h = " + v + "\n", "detectors.H1.baseline_drift_per_h");
  }
  const auto below = load("[detectors.H1]\nbaseline = -3\nbaseline_drift_per_h = -0.5\n");
  ASSERT_TRUE(below) << below.error().what;
  EXPECT_EQ(below->detectors.at("H1").baseline, -3.0);
  EXPECT_EQ(below->detectors.at("H1").drift_per_h, -0.5);
  expect_refused("[defaults]\nseed = -1\n", "defaults.seed");
  expect_refused("[defaults]\nseed = 1.5\n", "defaults.seed");
}

TEST_F(SimConfig, RefusesANumberNoLineCanHave) {
  expect_refused("[defaults]\npressure = 1e5\n", "defaults.pressure");
  expect_refused("[defaults]\nvolume_cc = 0\n", "defaults.volume_cc");
  expect_refused("[defaults]\nvolume_cc = 1e10\n", "defaults.volume_cc");
  expect_refused("[defaults]\npipe_cc = 0\n", "defaults.pipe_cc");
  expect_refused("[defaults]\ngauge_cc = 1e10\n", "defaults.gauge_cc");
  expect_refused("[defaults]\nvalve_conductance = 1e10\n", "defaults.valve_conductance");
  expect_refused("[defaults]\noutgassing = 1e4\n", "defaults.outgassing");
  expect_refused("[defaults]\nnoise = 11\n", "defaults.noise");
  expect_refused("[volumes.bone]\nleak = 1e4\n", "volumes.bone.leak");
  expect_refused("[volumes.bone]\nvolume_cc = 1e-7\n", "volumes.bone.volume_cc");
  expect_refused("[pumps.turbo]\nspeed = 1e10\n", "pumps.turbo.speed");
  expect_refused("[pumps.turbo]\nbase = 1e5\n", "pumps.turbo.base");
  expect_refused("[spectrometer]\nsensitivity = 0\n", "spectrometer.sensitivity");
  expect_refused("[spectrometer]\nsensitivity = 1e31\n", "spectrometer.sensitivity");
  expect_refused("[spectrometer]\nconsumption = 1e7\n", "spectrometer.consumption");
  // 1e4 mbar a second of memory.
  expect_refused("[spectrometer]\nsensitivity = 1\nmemory_fA_per_s = 1e4\n", "spectrometer.memory_fA_per_s");

  // The ends of each range are in it.
  const auto ends = load("[defaults]\npressure = 0\nvolume_cc = 1e-6\nvalve_conductance = 1e9\noutgassing = 1e3\n"
                         "noise = 10\n[spectrometer]\nsensitivity = 1e30\nconsumption = 1e6\n");
  ASSERT_TRUE(ends) << ends.error().what;
  EXPECT_EQ(ends->default_volume_cc, 1e-6);
  EXPECT_EQ(ends->source.consumption, 1e6);
}

TEST_F(SimConfig, RefusesAnUnknownComposition) {
  expect_refused("[volumes.bone]\ncomposition = \"neon\"\nargon40 = 1e-7\n", "volumes.bone.composition");
  expect_refused("[volumes.bone]\ncomposition = 3\n", "volumes.bone.composition");
  // The built-in ones are there unlisted.
  const auto loaded = load("[volumes.bone]\ncomposition = \"cocktail\"\nargon40 = 1e-7\n");
  ASSERT_TRUE(loaded) << loaded.error().what;
  EXPECT_EQ(loaded->compositions.at("bone"), sim::with_ar40(sim::cocktail_ratios(), 1e-7));
}

TEST_F(SimConfig, EveryProblemIsReportedWithItsLine) {
  const auto loaded = load("[defaults]\nnoise = -1\n\n[volumes.nosuch]\nleak = 0\n");
  ASSERT_FALSE(loaded);
  const std::string file = (dir_ / "sim.toml").generic_string();
  EXPECT_NE(loaded.error().what.find(file + ":2:defaults.noise: "), std::string::npos) << loaded.error().what;
  EXPECT_NE(loaded.error().what.find(file + ":4:volumes.nosuch: "), std::string::npos) << loaded.error().what;
}

TEST_F(SimConfig, RefusesAFileThatIsNotThereOrIsNotToml) {
  const auto missing = sim::load_sim_settings(dir_ / "absent.toml", lab());
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorKind::Config);
  EXPECT_NE(missing.error().what.find("absent.toml"), std::string::npos) << missing.error().what;
  expect_refused("[defaults\npressure = 1\n", "toml");
}

// One file that sets every key of the format, each to a number that is not
// its default and is no other key's: a reader that checked a key and dropped
// it would show here, in what is stored and in what a line then does.
TEST_F(SimConfig, EveryKeyIsStoredNotJustValidated) {
  SimSettings base;
  base.outgassing_active = 0.0;  // no key: out of the way of the rates below
  const auto loaded = load(R"(
[defaults]
pressure = 3e-7
volume_cc = 40
pipe_cc = 2
gauge_cc = 3
valve_conductance = 0.25
outgassing = 7e-12
noise = 0.2
seed = 77

[compositions.spike]
Ar36 = 2
Ar37 = 3
Ar38 = 5
Ar39 = 7
Ar40 = 11
active = 13

[volumes.air_tank]
composition = "spike"
argon40 = 2.2e-6

[volumes.bone]
pressure = 6e-5
leak = 4e-11
volume_cc = 8

[valves.inlet]
conductance = 0.035

[pumps.turbo]
speed = 9
base = 5e-7

[spectrometer]
sensitivity = 4e11
consumption = 6e-4
memory_fA_per_s = 0.8

[detectors.H1]
baseline = 17
baseline_drift_per_h = -1.5
)",
                           base);
  ASSERT_TRUE(loaded) << loaded.error().what;
  const SimSettings& s = *loaded;

  // Stored, key by key.
  EXPECT_EQ(s.default_pressure, 3e-7);
  EXPECT_EQ(s.default_volume_cc, 40.0);
  EXPECT_EQ(s.pipe_cc, 2.0);
  EXPECT_EQ(s.gauge_cc, 3.0);
  EXPECT_EQ(s.valve_conductance, 0.25);
  EXPECT_EQ(s.outgassing, 7e-12);
  EXPECT_EQ(s.noise, 0.2);
  EXPECT_EQ(s.seed, std::uint64_t{77});
  ASSERT_TRUE(s.named.contains("spike"));
  EXPECT_EQ(s.named.at("spike"), (Composition{2, 3, 5, 7, 11, 13}));
  ASSERT_TRUE(s.compositions.contains("air_tank"));
  EXPECT_EQ(s.compositions.at("air_tank")[kAr40], 2.2e-6);
  EXPECT_DOUBLE_EQ(s.compositions.at("air_tank")[kAr36], 0.4e-6);
  EXPECT_DOUBLE_EQ(s.compositions.at("air_tank")[kAr37], 0.6e-6);
  EXPECT_DOUBLE_EQ(s.compositions.at("air_tank")[kAr38], 1.0e-6);
  EXPECT_DOUBLE_EQ(s.compositions.at("air_tank")[kAr39], 1.4e-6);
  EXPECT_DOUBLE_EQ(s.compositions.at("air_tank")[kActive], 2.6e-6);
  ASSERT_TRUE(s.initial_pressures.contains("bone"));
  EXPECT_EQ(s.initial_pressures.at("bone"), 6e-5);
  ASSERT_TRUE(s.leaks.contains("bone"));
  EXPECT_EQ(s.leaks.at("bone"), 4e-11);
  ASSERT_TRUE(s.sizes.contains("bone"));
  EXPECT_EQ(s.sizes.at("bone"), 8.0);
  ASSERT_TRUE(s.conductances.contains("inlet"));
  EXPECT_EQ(s.conductances.at("inlet"), 0.035);
  ASSERT_TRUE(s.pump_speeds.contains("turbo"));
  EXPECT_EQ(s.pump_speeds.at("turbo"), 9.0);
  ASSERT_TRUE(s.pumps.contains("turbo"));
  EXPECT_EQ(s.pumps.at("turbo").base, 5e-7);
  EXPECT_EQ(s.source.sensitivity, 4e11);
  EXPECT_EQ(s.source.consumption, 6e-4);
  EXPECT_EQ(s.source.memory_fa_per_s, 0.8);
  ASSERT_TRUE(s.detectors.contains("H1"));
  EXPECT_EQ(s.detectors.at("H1").baseline, 17.0);
  EXPECT_EQ(s.detectors.at("H1").drift_per_h, -1.5);

  // And in force. Each line below has only what its assertion needs; a
  // setting for a name a line does not have does nothing there.
  SimSettings still = s;  // the same numbers with nothing read from a gauge in the way
  still.noise = 0.0;

  {  // defaults.pressure; volume_cc, gauge_cc, pipe_cc as the sizes gas is shared between
    //   IG (gauge) --g-- plain (no size) --p-- a~b (pipe)
    SimTopology t;
    t.volumes = {{"plain"}, {"IG", 0.0, SimRole::Gauge}, {"a~b", 0.0, SimRole::Pipe}};
    t.valves = {"g", "p"};
    t.edges = {{"IG", "g"}, {"g", "plain"}, {"plain", "p"}, {"p", "a~b"}};
    ManualClock clock;
    SimSettings settings = still;
    settings.outgassing = 0.0;  // asserted below, on a line of its own
    SimSystem sim(clock, t, settings);
    ASSERT_FALSE(sim.build_error()) << sim.build_error()->what;
    EXPECT_NEAR(*sim.pressure("plain"), 3e-7, 3e-7 * 1e-12);
    ASSERT_TRUE(sim.set_pressure("plain", 1e-3));
    sim.set_valve("g", true);
    clock.advance(10s);
    const double with_gauge = (40 * 1e-3 + 3 * 3e-7) / 43;
    EXPECT_NEAR(*sim.pressure("IG"), with_gauge, with_gauge * 1e-9);
    sim.set_valve("g", false);
    sim.set_valve("p", true);
    clock.advance(10s);
    const double with_pipe = (40 * with_gauge + 2 * 3e-7) / 42;
    EXPECT_NEAR(*sim.pressure("a~b"), with_pipe, with_pipe * 1e-9);
  }

  {  // defaults.valve_conductance and valves.inlet.conductance: tau = V1 V2 / ((V1 + V2) C)
    //   a --other-- b     c --inlet-- d     40 cc each
    SimTopology t;
    t.volumes = {{"a"}, {"b"}, {"c"}, {"d"}};
    t.valves = {"other", "inlet"};
    t.edges = {{"a", "other"}, {"other", "b"}, {"c", "inlet"}, {"inlet", "d"}};
    for (const auto& [valve, left, right, conductance] :
         {std::tuple{"other", "a", "b", 0.25}, std::tuple{"inlet", "c", "d", 0.035}}) {
      ManualClock clock;
      SimSystem sim(clock, t, still);
      ASSERT_TRUE(sim.set_composition(left, sim::with_ar40(sim::air_ratios(), 1e-5)));
      const double apart = (*sim.partial_pressures(left))[kAr40] - (*sim.partial_pressures(right))[kAr40];
      sim.set_valve(valve, true);
      const double tau = 0.04 * 0.04 / (0.08 * conductance);
      clock.advance(std::chrono::duration_cast<Duration>(std::chrono::duration<double>(tau)));
      // The walls give the two the same, so the difference is the valve's alone.
      const double now = (*sim.partial_pressures(left))[kAr40] - (*sim.partial_pressures(right))[kAr40];
      EXPECT_NEAR(now / apart, std::exp(-1.0), 1e-6) << valve;
    }
  }

  {  // defaults.outgassing: a volume on its own rises by it, whatever its size
    SimTopology t;
    t.volumes = {{"plain"}};
    ManualClock clock;
    SimSystem sim(clock, t, still);
    const double start = (*sim.partial_pressures("plain"))[kAr40];
    clock.advance(100s);
    EXPECT_NEAR((*sim.partial_pressures("plain"))[kAr40] - start, 7e-12 * 100, 7e-12 * 100 * 1e-6);
  }

  {  // defaults.noise and defaults.seed
    SimTopology t;
    t.volumes = {{"plain"}};
    ManualClock clock;
    SimSystem sim(clock, t, s);
    SimSettings reseeded = s;
    reseeded.seed = SimSettings{}.seed;
    SimSystem other(clock, t, reseeded);
    SimSystem same(clock, t, s);
    double sum = 0.0;
    double squares = 0.0;
    int differing = 0;
    const int n = 4000;
    for (int i = 0; i < n; ++i) {
      clock.advance(1ms);
      const double p = *sim.pressure("plain");
      const double r = *sim.gauge_reading("plain") / p - 1.0;
      sum += r;
      squares += r * r;
      EXPECT_EQ(*sim.gauge_reading("plain"), *same.gauge_reading("plain"));
      differing += *sim.gauge_reading("plain") != *other.gauge_reading("plain") ? 1 : 0;
    }
    const double mean = sum / n;
    const double sigma = std::sqrt(squares / n - mean * mean);
    EXPECT_NEAR(sigma, 0.2, 0.02) << "the spread of the readings is the file's noise";
    EXPECT_NEAR(mean, 0.0, 0.02);
    EXPECT_GT(differing, n * 9 / 10) << "another seed, other draws";
  }

  {  // pumps.turbo: speed is how fast (tau = V / S), base is where it settles
    SimTopology t;
    t.volumes = {{"turbo", 0.0, SimRole::Pump}};
    ManualClock clock;
    SimSystem sim(clock, t, still);
    ASSERT_TRUE(sim.set_pressure("turbo", 1e-3));
    clock.advance(std::chrono::duration_cast<Duration>(std::chrono::duration<double>(0.04 / 9.0)));
    const double one_tau = 5e-7 + (1e-3 - 5e-7) * std::exp(-1.0);
    EXPECT_NEAR(*sim.pressure("turbo"), one_tau, one_tau * 1e-6);
    clock.advance(60s);
    EXPECT_NEAR(*sim.pressure("turbo"), 5e-7, 5e-7 * 1e-6);
  }

  {  // volumes.bone: its pressure, and its leak into its own size (the canvas says 12.5 cc)
    SimTopology t;
    t.volumes = {{"bone", 12.5}};
    ManualClock clock;
    SimSystem sim(clock, t, still);
    EXPECT_NEAR(*sim.pressure("bone"), 6e-5, 6e-5 * 1e-12);
    const double start = (*sim.partial_pressures("bone"))[kAr40];
    clock.advance(100s);
    const double rate = 4e-11 / 0.008 + 7e-12;  // leak / V, and the walls
    EXPECT_NEAR((*sim.partial_pressures("bone"))[kAr40] - start, rate * 100, rate * 100 * 1e-6);
  }

  {  // volumes.air_tank: the composition named, at its argon40
    SimTopology t;
    t.volumes = {{"air_tank", 0.0, SimRole::Tank}};
    ManualClock clock;
    SimSystem sim(clock, t, still);
    const Composition held = *sim.partial_pressures("air_tank");
    EXPECT_DOUBLE_EQ(held[kAr40], 2.2e-6);
    EXPECT_DOUBLE_EQ(held[kAr39], 1.4e-6);
    EXPECT_DOUBLE_EQ(held[kActive], 2.6e-6);
  }

  {  // spectrometer: consumption uses the argon up; the memory, through the sensitivity, raises Ar40
    SimTopology t;
    t.volumes = {{"source", 0.0, SimRole::Spectrometer}};
    ManualClock clock;
    SimSystem sim(clock, t, still);
    Composition gas{};
    gas[kAr39] = 1e-6;  // nothing gives Ar39 off
    ASSERT_TRUE(sim.set_composition("source", gas));
    clock.advance(1000s);
    const Composition later = *sim.partial_pressures("source");
    EXPECT_NEAR(later[kAr39], 1e-6 * std::exp(-6e-4 * 1000), 1e-6 * 1e-9);
    // dA/dt = r - k A from nothing, r the memory 0.8 / 4e11 mbar/s and the walls' 7e-12.
    const double r = 0.8 / 4e11 + 7e-12;
    const double expected = r / 6e-4 * (1 - std::exp(-6e-4 * 1000));
    EXPECT_NEAR(later[kAr40], expected, expected * 1e-6);
  }
}

// A `[pumps.<name>]` with one key leaves the other as the base had it.
TEST_F(SimConfig, APumpsMissingKeyLeavesWhatTheBaseHad) {
  SimSettings base;
  base.pump_speed = 30.0;
  base.pump_base = 2e-9;
  base.pumps["turbo"] = {3e-6, 2s};
  base.pump_speeds["turbo"] = 4.0;

  const auto speed = load("[pumps.turbo]\nspeed = 9\n", base);
  ASSERT_TRUE(speed) << speed.error().what;
  EXPECT_EQ(speed->pump_speeds.at("turbo"), 9.0);
  EXPECT_EQ(speed->pumps.at("turbo").base, 3e-6) << "the base's, not pump_base";
  EXPECT_EQ(speed->pumps.at("turbo").tau, Duration{2s});

  const auto ultimate = load("[pumps.turbo]\nbase = 5e-7\n", base);
  ASSERT_TRUE(ultimate) << ultimate.error().what;
  EXPECT_EQ(ultimate->pumps.at("turbo").base, 5e-7);
  EXPECT_EQ(ultimate->pump_speeds.at("turbo"), 4.0) << "the base's, not pump_speed";
  EXPECT_EQ(ultimate->pumps.at("turbo").tau, Duration{2s});

  // A base pump described by its time constant keeps it: no speed is made up.
  base.pump_speeds.clear();
  const auto timed = load("[pumps.turbo]\nbase = 5e-7\n", base);
  ASSERT_TRUE(timed) << timed.error().what;
  EXPECT_EQ(timed->pumps.at("turbo").base, 5e-7);
  EXPECT_EQ(timed->pumps.at("turbo").tau, Duration{2s});
  EXPECT_FALSE(timed->pump_speeds.contains("turbo"));

  // No pump there before: the other key is a pump stage's.
  const auto fresh = load("[pumps.bone]\nspeed = 9\n", base);
  ASSERT_TRUE(fresh) << fresh.error().what;
  EXPECT_EQ(fresh->pumps.at("bone").base, 2e-9);
  EXPECT_EQ(fresh->pump_speeds.at("bone"), 9.0);
  const auto slow = load("[pumps.bone]\nbase = 5e-7\n", base);
  ASSERT_TRUE(slow) << slow.error().what;
  EXPECT_EQ(slow->pumps.at("bone").base, 5e-7);
  EXPECT_EQ(slow->pump_speeds.at("bone"), 30.0);
  // And an empty section is a pump stage's pump on that volume.
  const auto bare = load("[pumps.bone]\n", base);
  ASSERT_TRUE(bare) << bare.error().what;
  EXPECT_EQ(bare->pumps.at("bone").base, 2e-9);
  EXPECT_EQ(bare->pump_speeds.at("bone"), 30.0);
}

// A message says what would have been right: the names there are, when they
// are few enough to read, and the unit of a range.
TEST_F(SimConfig, AMessageNamesWhatIsKnownAndTheUnit) {
  expect_said("[volumes.nosuch]\nleak = 0\n", "volumes.nosuch",
              "unknown volume 'nosuch'; known: air_tank, bone, cocktail_tank, turbo");
  expect_said("[valves.nosuch]\nconductance = 0.1\n", "valves.nosuch", "unknown valve 'nosuch'; known: C, inlet");
  expect_said("[pumps.nosuch]\nspeed = 1\n", "pumps.nosuch",
              "unknown volume 'nosuch' for a pump; known: air_tank, bone, cocktail_tank, turbo");
  expect_said("[compositions.spike]\nAr36 = 1\n[volumes.bone]\ncomposition = \"neon\"\n", "volumes.bone.composition",
              "unknown composition 'neon'; known: air, cocktail, spike");
  expect_said("[defaults]\npresure = 1e-9\n", "defaults.presure",
              "unknown key; known: pressure, volume_cc, pipe_cc, gauge_cc, valve_conductance, outgassing, noise, seed");
  expect_said("[pumps.turbo]\ntau = 3\n", "pumps.turbo.tau", "unknown key; known: speed, base");
  expect_said("[compositions.mix]\nAr41 = 1\n", "compositions.mix.Ar41",
              "unknown key; known: Ar36, Ar37, Ar38, Ar39, Ar40, active");
  expect_said("[default]\npressure = 1e-9\n", "default",
              "unknown key; known: defaults, compositions, volumes, valves, pumps, spectrometer, detectors");

  // More than a dozen: how many, not which.
  SimTopology many;
  for (int i = 0; i < 13; ++i) many.volumes.push_back({"v" + std::to_string(i)});
  for (int i = 0; i < 12; ++i) many.valves.push_back("x" + std::to_string(i));
  const auto crowded = sim::load_sim_settings(write("[volumes.nosuch]\nleak = 0\n[valves.nosuch]\nconductance = 1\n"), many);
  ASSERT_FALSE(crowded);
  EXPECT_NE(crowded.error().what.find("unknown volume 'nosuch'; 13 are known"), std::string::npos)
      << crowded.error().what;
  EXPECT_EQ(crowded.error().what.find("v12"), std::string::npos) << crowded.error().what;
  EXPECT_NE(crowded.error().what.find("unknown valve 'nosuch'; known: x0, x1, x10, x11, x2"), std::string::npos)
      << crowded.error().what;
  const auto bare = sim::load_sim_settings(write("[valves.nosuch]\nconductance = 1\n"), SimTopology{});
  ASSERT_FALSE(bare);
  EXPECT_NE(bare.error().what.find("unknown valve 'nosuch'; there is none"), std::string::npos) << bare.error().what;

  // The unit of each range.
  expect_said("[defaults]\npressure = 1e5\n", "defaults.pressure", "must be from 0 to 10000 mbar");
  expect_said("[defaults]\nvolume_cc = 0\n", "defaults.volume_cc", "must be from 1e-06 to 1e+09 cc");
  expect_said("[defaults]\nvalve_conductance = -1\n", "defaults.valve_conductance", "must be from 0 to 1e+09 L/s");
  expect_said("[defaults]\noutgassing = 1e4\n", "defaults.outgassing", "must be from 0 to 1000 mbar L/s");
  expect_said("[defaults]\nnoise = 11\n", "defaults.noise", "must be from 0 to 10 as a fraction of the reading");
  expect_said("[compositions.mix]\nAr40 = -1\n", "compositions.mix.Ar40", "must be from 0 to 1e+09 as a ratio to Ar36");
  expect_said("[volumes.bone]\nleak = -1\n", "volumes.bone.leak", "must be from 0 to 1000 mbar L/s");
  expect_said("[pumps.turbo]\nspeed = -1\n", "pumps.turbo.speed", "must be from 0 to 1e+09 L/s");
  expect_said("[pumps.turbo]\nbase = -1\n", "pumps.turbo.base", "must be from 0 to 10000 mbar");
  expect_said("[spectrometer]\nsensitivity = 0\n", "spectrometer.sensitivity", "must be above 0 to 1e+30 fA per mbar");
  expect_said("[spectrometer]\nconsumption = -1\n", "spectrometer.consumption", "must be from 0 to 1e+06 1/s");
  expect_said("[spectrometer]\nmemory_fA_per_s = -1\n", "spectrometer.memory_fA_per_s", "must be from 0 to 1e+30 fA/s");
  expect_said("[volumes.bone]\nargon40 = 100\n", "volumes.bone.argon40", "must be from 0 to 10000 mbar");

  // The memory as gas is the one over the other: the key at fault is the
  // one the file gives.
  expect_said("[spectrometer]\nsensitivity = 1e-6\n", "spectrometer.sensitivity", "must be from 0 to 1000 mbar/s");
  EXPECT_EQ(load("[spectrometer]\nsensitivity = 1e-6\n").error().what.find("spectrometer.memory_fA_per_s:"),
            std::string::npos);
  expect_said("[spectrometer]\nmemory_fA_per_s = 1e16\n", "spectrometer.memory_fA_per_s",
              "must be from 0 to 1000 mbar/s");
  expect_said("[spectrometer]\nsensitivity = 1\nmemory_fA_per_s = 1e4\n", "spectrometer.memory_fA_per_s",
              "must be from 0 to 1000 mbar/s");
}

// configs/examples/sim.toml has every key with its unit, its meaning and its
// default, each commented out. As shipped it changes nothing; with every
// `# key = value` line (and `# [section]`) uncommented it says the defaults,
// so what it documents cannot drift from the code.
TEST_F(SimConfig, TheExampleFileIsTheDefaults) {
  const std::filesystem::path example = std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "sim.toml";
  SimTopology canvas;  // the example canvas's names
  canvas.volumes = {{"bone"}, {"prep"}, {"spec"}, {"turbo"}, {"rough"}, {"air_tank"}, {"air"}, {"IG1"}, {"PG1"}};
  canvas.volumes[5].role = SimRole::Tank;
  canvas.valves = {"A", "B", "C", "P1", "P2", "M1"};

  // As shipped: nothing but comments, and a base that is nobody's default
  // comes back as it went in.
  std::ifstream in(example);
  ASSERT_TRUE(in) << example;
  const std::regex commented_out(R"(^# (\[[A-Za-z0-9_."~-]+\]\s*$|[A-Za-z0-9_]+ = \S))");
  std::ostringstream uncommented;
  std::vector<std::string> keys;
  int sections = 0;
  for (std::string line; std::getline(in, line);) {
    EXPECT_TRUE(line.empty() || line[0] == '#') << "not commented out: " << line;
    if (std::regex_search(line, commented_out)) {
      line.erase(0, 2);
      if (line[0] == '[') {
        ++sections;
      } else {
        keys.push_back(line.substr(0, line.find(' ')));
      }
    }
    uncommented << line << '\n';
  }
  SimSettings base;
  base.default_pressure = 4e-6;
  base.default_volume_cc = 7.0;
  base.pipe_cc = 0.3;
  base.gauge_cc = 0.4;
  base.valve_conductance = 0.02;
  base.outgassing = 1e-14;
  base.noise = 0.5;
  base.seed = 9;
  base.named["cocktail"] = Composition{1, 2, 3, 4, 5, 6};
  base.compositions["air_tank"] = sim::with_ar40(sim::cocktail_ratios(), 1e-6);
  base.initial_pressures["prep"] = 2e-5;
  base.leaks["bone"] = 3e-12;
  base.sizes["air"] = 0.7;
  base.conductances["B"] = 0.6;
  base.pumps["turbo"] = {3e-6, 2s};
  base.pump_speeds["turbo"] = 4.0;
  base.pumps["rough"] = {4e-6, 3s};
  base.source = {5e10, 7e-3, 0.9};
  base.detectors["H1"] = {12.0, 0.25};
  const auto shipped = sim::load_sim_settings(example, canvas, base);
  ASSERT_TRUE(shipped) << shipped.error().what;
  expect_same(*shipped, base);

  // Uncommented: every key of the format, each at its default.
  EXPECT_EQ(sections, 11);
  const std::vector<std::string> every_key{
      "pressure", "volume_cc",   "pipe_cc",     "gauge_cc",        "valve_conductance", "outgassing", "noise",
      "seed",     "Ar36",        "Ar37",        "Ar38",            "Ar39",              "Ar40",       "active",
      "composition", "argon40",  "pressure",    "leak",            "volume_cc",         "conductance", "speed",
      "base",     "speed",       "base",        "sensitivity",     "consumption",       "memory_fA_per_s",
      "baseline", "baseline_drift_per_h"};
  EXPECT_EQ(keys, every_key);
  const auto loaded = sim::load_sim_settings(write(uncommented.str()), canvas);
  ASSERT_TRUE(loaded) << loaded.error().what;
  const SimSettings defaults;
  // What is said by name is what the role, or the default, gives unasked.
  SimSettings expected = defaults;
  expected.named["cocktail"] = sim::cocktail_ratios();
  expected.compositions["air_tank"] = sim::with_ar40(sim::air_ratios(), defaults.tank_argon40);
  expected.initial_pressures["prep"] = defaults.default_pressure;
  expected.leaks["bone"] = 0.0;
  expected.sizes["air"] = defaults.pipette_cc;
  expected.conductances["B"] = defaults.valve_conductance;
  expected.pumps["turbo"] = sim::SimPump{defaults.pump_base, sim::SimPump{}.tau};
  expected.pump_speeds["turbo"] = defaults.pump_speed;
  expected.pumps["rough"] = sim::SimPump{defaults.pump_base, sim::SimPump{}.tau};
  expected.pump_speeds["rough"] = defaults.pump_speed;
  expected.detectors["H1"] = {0.0, 0.0};
  expect_same(*loaded, expected);
}

}  // namespace
