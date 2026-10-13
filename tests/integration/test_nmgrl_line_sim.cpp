// The NMGRL valve box (configs/examples/nmgrl): a full-size line converted
// from its legacy setupfiles, every controller simulated. The files load as
// committed, the canvas matches the line, and valves on each of the five
// controllers actuate with the pipette interlocks enforced. Time is a
// VirtualClock the test's thread takes part in: the drivers' waits for their
// instruments are in its time.

#include <filesystem>

#include <gtest/gtest.h>

#include "pychron/core/virtual_clock.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "virtual_time.hpp"

namespace {

using namespace pychron;
using namespace pychron::systems;

const std::filesystem::path kDir = std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "nmgrl";

using NmgrlLineSim = pychron::testing::VirtualTimeTest;

TEST_F(NmgrlLineSim, LoadsAndActuatesOnEveryController) {
  VirtualClock clock;
  Clock::Participant test(clock, "test");
  // Valve states and locks persist beside the config by default: keep the
  // test out of the repo, and out of the state of anyone running this line.
  ExtractionLine::Options options;
  options.clock = &clock;
  options.state_file = std::filesystem::temp_directory_path() / "pychron-test-nmgrl-line.state.toml";
  std::filesystem::remove(options.state_file);
  auto made = ExtractionLine::load(kDir / "extraction_line.toml", kDir / "canvas.toml", options);
  ASSERT_TRUE(made) << made.error().what;
  auto& line = **made;
  ASSERT_TRUE(line.start());
  EXPECT_EQ(line.snapshot().valves.size(), 40u);  // 33 actuated, 7 manual

  // one valve per legacy controller (furnace; Agilent and the two Qtegra
  // boxes, real drivers against simulated instruments; Arduino), then
  // pipette 1's outer valve
  for (const char* valve : {"FD", "Q", "I", "V", "O", "W"}) {
    auto opened = line.actuate(valve, SwitchOp::Open, "test");
    ASSERT_TRUE(opened) << valve << ": " << opened.error().what;
    EXPECT_EQ(line.snapshot().valves.at(valve), ValveState::Open) << valve;
  }

  // pipette 1: outer W is open, so inner X is refused
  auto inner = line.actuate("X", SwitchOp::Open, "test");
  ASSERT_FALSE(inner);
  EXPECT_EQ(inner.error().kind, ErrorKind::Interlock);

  line.stop();
}

// The example's cryostat and bakeout heater (not in the legacy setup) answer
// in simulation, so the Cryo and Heaters docks have something to show.
TEST_F(NmgrlLineSim, HasACryostatAndAHeater) {
  VirtualClock clock;
  Clock::Participant test(clock, "test");
  ExtractionLine::Options options;
  options.clock = &clock;
  options.state_file = std::filesystem::temp_directory_path() / "pychron-test-nmgrl-cryo.state.toml";
  std::filesystem::remove(options.state_file);
  auto made = ExtractionLine::load(kDir / "extraction_line.toml", kDir / "canvas.toml", options);
  ASSERT_TRUE(made) << made.error().what;
  auto& line = **made;
  ASSERT_TRUE(line.start());

  ITemperatureController* cryostat = line.cryostat();
  ASSERT_NE(cryostat, nullptr);
  EXPECT_EQ(cryostat->inputs(), (std::vector<std::string>{"A", "B"}));
  ASSERT_TRUE(line.config().cryo);
  // every named setpoint is one the controller takes
  EXPECT_FALSE(line.config().cryo->setpoints.empty());
  for (const auto& [name, values] : line.config().cryo->setpoints) {
    for (std::size_t i = 0; i < values.size(); ++i) {
      const int output = static_cast<int>(i) + 1;
      auto set = cryostat->set_setpoint(output, values[i]);
      ASSERT_TRUE(set) << name << ": " << set.error().what;
      auto read = cryostat->setpoint(output);
      ASSERT_TRUE(read) << name;
      EXPECT_NEAR(*read, values[i], 0.01) << name;
    }
  }
  ASSERT_TRUE(cryostat->read_temperature("A"));

  ASSERT_EQ(line.config().heaters.size(), 1u);
  const std::string heater = line.config().heaters[0].name;
  ASSERT_TRUE(line.set_heater_setpoint(heater, 120.0));
  ASSERT_TRUE(line.set_heater_pid(heater, true));
  ASSERT_TRUE(line.set_heater_enabled(heater, true));
  auto sample = line.read_heater(heater);
  ASSERT_TRUE(sample) << sample.error().what;
  EXPECT_EQ(sample->enabled, std::optional<bool>(true));
  EXPECT_EQ(sample->use_pid, std::optional<bool>(true));
  ASSERT_TRUE(sample->setpoint);
  EXPECT_DOUBLE_EQ(*sample->setpoint, 120.0);
  EXPECT_TRUE(sample->readback);

  line.stop();
}

}  // namespace
