#include "pychron/sim/sim_config.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "pychron/core/error.hpp"
#include "pychron/sim/gas.hpp"
#include "pychron/sim/sim_system.hpp"

namespace {

using namespace pychron;
using sim::Composition;
using sim::SimRole;
using sim::SimSettings;
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

  std::filesystem::path dir_;
};

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

// What the lab ships with says what the code does with no file at all.
TEST_F(SimConfig, TheExampleFileIsTheDefaults) {
  const std::filesystem::path example = std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "sim.toml";
  SimTopology canvas;  // the example canvas's names
  canvas.volumes = {{"bone"}, {"prep"}, {"spec"}, {"turbo"}, {"rough"}, {"air_tank"}, {"air"}, {"IG1"}, {"PG1"}};
  canvas.volumes[5].role = SimRole::Tank;
  canvas.valves = {"A", "B", "C", "P1", "P2", "M1"};
  const auto loaded = sim::load_sim_settings(example, canvas);
  ASSERT_TRUE(loaded) << loaded.error().what;
  const SimSettings defaults;
  EXPECT_EQ(loaded->default_pressure, defaults.default_pressure);
  EXPECT_EQ(loaded->default_volume_cc, defaults.default_volume_cc);
  EXPECT_EQ(loaded->valve_conductance, defaults.valve_conductance);
  EXPECT_EQ(loaded->outgassing, defaults.outgassing);
  EXPECT_EQ(loaded->noise, defaults.noise);
  EXPECT_EQ(loaded->seed, defaults.seed);
  EXPECT_EQ(loaded->source.sensitivity, defaults.source.sensitivity);
  EXPECT_EQ(loaded->source.consumption, defaults.source.consumption);
  EXPECT_EQ(loaded->source.memory_fa_per_s, defaults.source.memory_fa_per_s);
  // Each named section says what the role gives unasked.
  EXPECT_EQ(loaded->named.at("cocktail"), sim::cocktail_ratios());
  EXPECT_EQ(loaded->compositions.at("air_tank"), sim::with_ar40(sim::air_ratios(), defaults.tank_argon40));
  EXPECT_EQ(loaded->leaks.at("bone"), 0.0);
  EXPECT_EQ(loaded->sizes.at("air"), defaults.pipette_cc);
  EXPECT_EQ(loaded->conductances.at("B"), defaults.valve_conductance);
  EXPECT_EQ(loaded->pump_speeds.at("turbo"), defaults.pump_speed);
  EXPECT_EQ(loaded->pumps.at("turbo").base, defaults.pump_base);
  EXPECT_EQ(loaded->detectors.at("H1").baseline, 0.0);
  EXPECT_EQ(loaded->detectors.at("H1").drift_per_h, 0.0);
}

}  // namespace
