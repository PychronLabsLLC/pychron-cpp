// lab: trays, calibrations and extraction devices, and what the queue check
// says about a run that could not reach its hole (laser system design,
// section 5). A scratch copy of configs/examples with a Chromium driver on a
// simulated transport added to its line config.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/model/queue_file.hpp"

namespace pychron::experiment::lab {
namespace {

namespace fs = std::filesystem;

const char* kTray = "circle,1.0\n\n2,3,4,5,1\n1,0,0\n2,0,5\n3,5,0\n4,0,-5\n5,-5,0\n";

class LabExtractionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("pychron-lab-laser-" + std::to_string(rd()) + std::to_string(rd()));
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir_, fs::copy_options::recursive);
    fs::remove_all(dir_ / "data");
    fs::remove_all(dir_ / "tray_maps");
    fs::remove_all(dir_ / "stage_calibrations");
    // The line config with exactly one extraction device, whatever the
    // example's has by now.
    std::ofstream(dir_ / "line.toml") << "[system]\nname = \"laser_lab\"\n"
                                         "[transports.laser_pc]\nkind = \"sim\"\n"
                                         "[transports.valve_bus]\nkind = \"sim\"\n"
                                         "[drivers.co2]\nkind = \"chromium\"\ntransport = \"laser_pc\"\n"
                                         "[drivers.relays]\nkind = \"proxr_relay\"\ntransport = \"valve_bus\"\n";
    fs::create_directories(dir_ / "tray_maps");
    std::ofstream(dir_ / "tray_maps" / "small.txt") << kTray;
  }
  void TearDown() override { fs::remove_all(dir_); }

  Lab lab() const {
    return load_lab({dir_, dir_ / "line.toml", dir_ / "spectrometer.sim-integrated.toml"});
  }
  void calibrate(const Lab& lab, const std::string& device = "co2") const {
    const std::vector<laser::CalibrationPoint> points{{"1", 10, 20}, {"3", 15, 20}};
    auto saved = lab.calibrations->save(*lab.trays.find("small"), device, points);
    ASSERT_TRUE(saved) << saved.error().what;
  }
  // The example queue on co2 and the small tray; run 1 (the unknown) on hole 3.
  QueueSpec queue(const Lab& lab) const {
    auto q = load_queue_file((dir_ / "experiment.toml").string(), lab.ids);
    EXPECT_TRUE(q) << q.error().what;
    QueueSpec out = q ? *q : QueueSpec{};
    out.extract_device = "co2";
    out.tray = "small";
    for (auto& r : out.runs) r.extraction.device.clear();
    out.runs.at(1).extraction.position = Position{{3}};
    return out;
  }
  static std::vector<Diagnostic> extraction(const LabCheck& c) {
    std::vector<Diagnostic> out;
    for (const auto& d : c.all())
      if (d.field == "extraction") out.push_back(d);
    return out;
  }
  static bool says(const Diagnostic& d, std::initializer_list<const char*> words) {
    for (const char* w : words)
      if (d.message.find(w) == std::string::npos) return false;
    return true;
  }

  fs::path dir_;
};

TEST_F(LabExtractionTest, ListsExtractionDevicesAndTrays) {
  const Lab l = lab();
  EXPECT_TRUE(l.problems.empty()) << l.problems.front();
  EXPECT_EQ(l.extract_devices, (std::vector<std::string>{"co2"}));
  EXPECT_EQ(l.trays.names(), (std::vector<std::string>{"small"}));
  ASSERT_NE(l.calibrations, nullptr);
  EXPECT_EQ(l.calibrations->dir(), dir_ / "stage_calibrations");
}

TEST_F(LabExtractionTest, ALabWithNoLineHasAStoreAndNoDevices) {
  const Lab l = load_lab({dir_, {}, {}});
  ASSERT_NE(l.calibrations, nullptr);
  EXPECT_TRUE(l.extract_devices.empty());
  EXPECT_EQ(l.trays.names(), (std::vector<std::string>{"small"}));
}

TEST_F(LabExtractionTest, ABadTrayMapIsALabProblem) {
  std::ofstream(dir_ / "tray_maps" / "broken.txt") << "circle,1\n\n\n1,x\n";
  const Lab l = lab();
  ASSERT_EQ(l.problems.size(), 1u);
  EXPECT_NE(l.problems[0].find("broken:4"), std::string::npos) << l.problems[0];
  EXPECT_FALSE(check_lab_queue(l, queue(l)).ok());
}

TEST_F(LabExtractionTest, ALaserRunThatCanRunHasNoErrors) {
  const Lab l = lab();
  calibrate(l);
  // (This bare line config lacks the valves the example plans name, so the
  // whole check is not clean here; tests/integration runs a laser queue on
  // the full example lab.)
  for (const auto& d : check_lab_queue(l, queue(l)).all()) {
    if (d.field == "extraction" || d.field == "tray") ADD_FAILURE() << describe(d);
  }
}

TEST_F(LabExtractionTest, UnknownDevice) {
  const Lab l = lab();
  calibrate(l);
  auto q = queue(l);
  q.extract_device = "diode";
  const auto found = extraction(check_lab_queue(l, q));
  ASSERT_EQ(found.size(), 1u);  // once, on the first run that has an extraction
  EXPECT_EQ(found[0].severity, Severity::Error);
  EXPECT_EQ(found[0].run, 0);
  EXPECT_TRUE(says(found[0], {"'diode'", "co2"})) << found[0].message;
}

TEST_F(LabExtractionTest, UnknownTray) {
  const Lab l = lab();
  calibrate(l);
  auto q = queue(l);
  q.tray = "221-hole";
  const auto check = check_lab_queue(l, q);
  EXPECT_FALSE(check.ok());
  EXPECT_TRUE(extraction(check).empty());  // said once, about the queue
  std::vector<Diagnostic> found;
  for (const auto& d : check.all())
    if (d.field == "tray") found.push_back(d);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].run, -1);
  EXPECT_EQ(found[0].severity, Severity::Error);
  EXPECT_TRUE(says(found[0], {"221-hole", "small"})) << found[0].message;
  EXPECT_EQ(describe(found[0]).rfind("queue.tray: ", 0), 0u);
}

TEST_F(LabExtractionTest, HolesNeedATray) {
  const Lab l = lab();
  calibrate(l);
  auto q = queue(l);
  q.tray.clear();
  const auto found = extraction(check_lab_queue(l, q));
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].run, 1);
  EXPECT_TRUE(says(found[0], {"tray"})) << found[0].message;
}

TEST_F(LabExtractionTest, AHoleNotOnTheTray) {
  const Lab l = lab();
  calibrate(l);
  auto q = queue(l);
  q.runs.at(1).extraction.position = Position{{3, 99}};
  const auto found = extraction(check_lab_queue(l, q));
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].run, 1);
  EXPECT_TRUE(says(found[0], {"99", "small"})) << found[0].message;
}

TEST_F(LabExtractionTest, TrayNotCalibratedForTheDevice) {
  const Lab l = lab();
  auto found = extraction(check_lab_queue(l, queue(l)));
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].run, 1);
  EXPECT_TRUE(says(found[0], {"co2", "small", "not calibrated"})) << found[0].message;

  calibrate(l);
  EXPECT_TRUE(extraction(check_lab_queue(l, queue(l))).empty());

  // the map is edited after the calibration was made
  std::ofstream(dir_ / "tray_maps" / "small.txt", std::ios::app) << "9,9\n";
  const Lab edited = lab();
  found = extraction(check_lab_queue(edited, queue(edited)));
  ASSERT_EQ(found.size(), 1u);
  EXPECT_TRUE(says(found[0], {"co2", "small", "stale"})) << found[0].message;
}

TEST_F(LabExtractionTest, ATrayWithoutHolesInUseNeedsNoCalibration) {
  const Lab l = lab();  // not calibrated
  auto q = queue(l);
  q.runs.at(1).extraction.position.reset();
  EXPECT_TRUE(extraction(check_lab_queue(l, q)).empty());
}

TEST_F(LabExtractionTest, TheRunsOwnDeviceWins) {
  const Lab l = lab();
  calibrate(l);
  auto q = queue(l);
  q.runs.at(1).extraction.device = "diode";
  const auto found = extraction(check_lab_queue(l, q));
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].run, 1);
  EXPECT_TRUE(says(found[0], {"'diode'"})) << found[0].message;
}

TEST_F(LabExtractionTest, EachMessageOnce) {
  const Lab l = lab();
  auto q = queue(l);
  for (auto& r : q.runs) r.extraction.position = Position{{3}};
  ASSERT_GE(q.runs.size(), 3u);
  const auto found = extraction(check_lab_queue(l, q));
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].run, 0);
}

TEST_F(LabExtractionTest, SkippedRunsAreNotChecked) {
  const Lab l = lab();
  auto q = queue(l);
  q.runs.at(1).skip = true;
  EXPECT_TRUE(extraction(check_lab_queue(l, q)).empty());
}

TEST_F(LabExtractionTest, ARunWithNoDeviceIsNotChecked) {
  const Lab l = lab();
  auto q = queue(l);
  q.extract_device.clear();
  EXPECT_TRUE(extraction(check_lab_queue(l, q)).empty());
}

// The example lab's own line config, and every lab before lasers: with no
// extraction device configured the device name is free text.
TEST_F(LabExtractionTest, ALabWithoutDevicesChecksAsBefore) {
  std::ofstream(dir_ / "line.toml") << "[system]\nname = \"plain\"\n";
  const Lab l = lab();
  ASSERT_TRUE(l.problems.empty()) << l.problems.front();
  EXPECT_TRUE(l.extract_devices.empty());
  auto q = queue(l);
  q.extract_device = "anything";
  q.tray = "no-such-tray";
  q.runs.at(1).extraction.position = Position{{99}};
  EXPECT_TRUE(extraction(check_lab_queue(l, q)).empty());
}

}  // namespace
}  // namespace pychron::experiment::lab
