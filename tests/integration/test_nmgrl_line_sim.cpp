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

}  // namespace
