// ExtractionLine valve locks: facade, SwitchLockChanged events, Snapshot and
// persistence across restarts via the state file.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <gtest/gtest.h>

#include "pychron/core/config/loader.hpp"
#include "pychron/systems/extraction_line.hpp"

namespace {

using namespace pychron;
using namespace pychron::systems;

constexpr const char* kSystem = R"(
[system]
name = "t"

[transports.bus]
kind = "sim"

[drivers.relay]
kind = "proxr_relay"
transport = "bus"

[[valves]]
name = "A"
actuator = "relay"
address = "1"

[[valves]]
name = "B"
actuator = "relay"
address = "2"

[[manual_valves]]
name = "M1"

[[switches]]
name = "S1"
actuator = "relay"
address = "3"
)";

class LineLocks : public ::testing::Test {
 protected:
  void SetUp() override {
    // Named for the test: ctest runs each test in its own process, in parallel, and the
    // fixture's address repeats across processes.
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() / (std::string("pychron-locks-") + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    state_file_ = dir_ / "line.state.toml";
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::unique_ptr<ExtractionLine> make(bool with_state = true) {
    auto cfg = config::load_system_config_from_string(kSystem, "t.toml");
    EXPECT_TRUE(cfg) << cfg.error().what;
    ExtractionLine::Options o;
    o.clock = &clock_;
    o.scheduler.threads = 0;
    o.run_scheduler = false;
    if (with_state) o.state_file = state_file_;
    auto line = ExtractionLine::create(*cfg, std::nullopt, o);
    EXPECT_TRUE(line) << line.error().what;
    return std::move(*line);
  }

  std::string read_state() const {
    std::ifstream in(state_file_);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  ManualClock clock_;
  std::filesystem::path dir_;
  std::filesystem::path state_file_;
};

TEST_F(LineLocks, SetLockedLocksRefusesActuationAndPublishesEvent) {
  auto line = make();
  ASSERT_TRUE(line->start());
  std::vector<SwitchLockChanged> events;
  auto sub = line->bus().subscribe<SwitchLockChanged>([&](const SwitchLockChanged& e) { events.push_back(e); });

  ASSERT_TRUE(line->set_locked("A", true));
  EXPECT_TRUE(line->is_locked("A"));
  EXPECT_FALSE(line->is_locked("B"));
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].name, "A");
  EXPECT_TRUE(events[0].locked);

  auto refused = line->actuate("A", SwitchOp::Open, "ui");
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Interlock);

  ASSERT_TRUE(line->set_locked("A", false));
  EXPECT_FALSE(line->is_locked("A"));
  ASSERT_EQ(events.size(), 2u);
  EXPECT_FALSE(events[1].locked);
  EXPECT_TRUE(line->actuate("A", SwitchOp::Open, "ui"));
}

TEST_F(LineLocks, RepeatedSetLockedPublishesOnlyOnChange) {
  auto line = make();
  int events = 0;
  auto sub = line->bus().subscribe<SwitchLockChanged>([&](const SwitchLockChanged&) { ++events; });
  ASSERT_TRUE(line->set_locked("A", true));
  ASSERT_TRUE(line->set_locked("A", true));
  EXPECT_EQ(events, 1);
}

TEST_F(LineLocks, UnknownAndManualValvesAreRejected) {
  auto line = make();
  auto unknown = line->set_locked("Z", true);
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
  auto manual = line->set_locked("M1", true);
  ASSERT_FALSE(manual);
  EXPECT_EQ(manual.error().kind, ErrorKind::Config);
  EXPECT_FALSE(line->is_locked("Z"));
}

TEST_F(LineLocks, SwitchesCanBeLocked) {
  auto line = make();
  ASSERT_TRUE(line->set_locked("S1", true));
  EXPECT_TRUE(line->is_locked("S1"));
}

TEST_F(LineLocks, LocksPersistAcrossRestart) {
  {
    auto line = make();
    ASSERT_TRUE(line->set_locked("B", true));
    ASSERT_TRUE(line->set_locked("A", true));
    ASSERT_TRUE(line->set_locked("A", false));
  }
  EXPECT_NE(read_state().find('B'), std::string::npos);  // names only: no capitals elsewhere
  EXPECT_EQ(read_state().find('A'), std::string::npos);

  auto line = make();
  EXPECT_TRUE(line->is_locked("B"));
  EXPECT_FALSE(line->is_locked("A"));
  auto refused = line->actuate("B", SwitchOp::Open, "ui");
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Interlock);
}

TEST_F(LineLocks, SnapshotCarriesLocks) {
  auto line = make();
  ASSERT_TRUE(line->set_locked("B", true));
  const Snapshot s = line->snapshot();
  EXPECT_EQ(s.locked.count("B"), 1u);
  EXPECT_EQ(s.locked.count("A"), 0u);
}

// Valve states are remembered between runs, like locks. A simulated
// controller starts every run closed and a manual valve is only ever what
// the operator last reported, so the line puts both back.
TEST_F(LineLocks, ValveStatesSurviveARestart) {
  {
    auto line = make();
    ASSERT_TRUE(line->start());
    ASSERT_TRUE(line->actuate("A", SwitchOp::Open, "ui"));
    ASSERT_TRUE(line->actuate("M1", SwitchOp::Open, "ui"));
    ASSERT_TRUE(line->actuate("S1", SwitchOp::Open, "ui"));
    ASSERT_TRUE(line->actuate("S1", SwitchOp::Close, "ui"));
    line->stop();
  }
  auto line = make();
  // not before start(): nothing has been read back or restored yet
  EXPECT_EQ(line->snapshot().valves.at("M1"), ValveState::Unknown);
  std::vector<ValveChanged> events;
  auto sub = line->bus().subscribe<ValveChanged>([&](const ValveChanged& e) { events.push_back(e); });
  ASSERT_TRUE(line->start());
  const auto valves = line->snapshot().valves;
  EXPECT_EQ(valves.at("A"), ValveState::Open);
  EXPECT_EQ(valves.at("B"), ValveState::Closed);
  EXPECT_EQ(valves.at("M1"), ValveState::Open);
  EXPECT_EQ(valves.at("S1"), ValveState::Closed);
  // the simulated relay was really driven, not just the record changed
  EXPECT_TRUE(line->sim()->valve_open("A"));
  bool announced = false;
  for (const auto& e : events) announced |= e.valve == "A" && e.state == ValveState::Open;
  EXPECT_TRUE(announced);
}

TEST_F(LineLocks, ALockedValveIsRestoredAndStaysLocked) {
  {
    auto line = make();
    ASSERT_TRUE(line->start());
    ASSERT_TRUE(line->actuate("A", SwitchOp::Open, "ui"));
    ASSERT_TRUE(line->set_locked("A", true));
    line->stop();
  }
  auto line = make();
  ASSERT_TRUE(line->start());
  EXPECT_EQ(line->snapshot().valves.at("A"), ValveState::Open);
  EXPECT_TRUE(line->is_locked("A"));
  EXPECT_FALSE(line->actuate("A", SwitchOp::Close, "ui"));
}

TEST_F(LineLocks, ALockChangeBeforeStartKeepsTheRememberedValves) {
  std::ofstream(state_file_) << "locked = []\n[valves]\nA = \"open\"\nM1 = \"closed\"\n";
  {
    auto line = make();
    ASSERT_TRUE(line->set_locked("B", true));  // never started
  }
  auto line = make();
  EXPECT_TRUE(line->is_locked("B"));
  ASSERT_TRUE(line->start());
  EXPECT_EQ(line->snapshot().valves.at("A"), ValveState::Open);
  EXPECT_EQ(line->snapshot().valves.at("M1"), ValveState::Closed);
}

TEST_F(LineLocks, SavedValveStatesThatMakeNoSenseAreIgnoredAndWarned) {
  std::ofstream(state_file_) << "[valves]\nA = \"ajar\"\nGONE = \"open\"\nB = \"open\"\n";
  auto line = make();
  int warned = 0;
  for (const auto& w : line->warnings()) warned += w.field == "valves";
  EXPECT_EQ(warned, 2);
  ASSERT_TRUE(line->start());
  EXPECT_EQ(line->snapshot().valves.at("A"), ValveState::Closed);
  EXPECT_EQ(line->snapshot().valves.at("B"), ValveState::Open);
}

TEST_F(LineLocks, NoStateFileMeansNothingIsWritten) {
  auto line = make(/*with_state=*/false);
  ASSERT_TRUE(line->set_locked("A", true));
  EXPECT_FALSE(std::filesystem::exists(state_file_));
}

TEST_F(LineLocks, StateNamingUnknownValveIsIgnoredAndWarned) {
  std::ofstream(state_file_) << "locked = [\"A\", \"GONE\"]\n";
  auto line = make();
  EXPECT_TRUE(line->is_locked("A"));
  bool warned = false;
  for (const auto& w : line->warnings()) warned |= w.message.find("GONE") != std::string::npos;
  EXPECT_TRUE(warned);
}

TEST_F(LineLocks, CorruptStateFileLeavesEverythingUnlockedAndWarns) {
  std::ofstream(state_file_) << "locked = [[[ not toml\n";
  auto line = make();
  EXPECT_FALSE(line->is_locked("A"));
  EXPECT_FALSE(line->warnings().empty());
  // The next change replaces the corrupt file with a valid one.
  ASSERT_TRUE(line->set_locked("A", true));
  auto again = make();
  EXPECT_TRUE(again->is_locked("A"));
}

TEST_F(LineLocks, UnwritableStateFileStillLocksInMemory) {
  // state_file under a regular file cannot be created.
  std::ofstream(dir_ / "plain") << "x";
  state_file_ = dir_ / "plain" / "line.state.toml";
  auto line = make();
  std::vector<Log> logs;
  auto sub = line->bus().subscribe<Log>([&](const Log& l) { logs.push_back(l); });
  ASSERT_TRUE(line->set_locked("A", true));
  EXPECT_TRUE(line->is_locked("A"));
  bool warned = false;
  for (const auto& l : logs) warned |= l.level == LogLevel::Warn;
  EXPECT_TRUE(warned);
}

}  // namespace
