// lab: loading a lab directory and checking a queue against it, on a scratch
// copy of configs/examples (which doubles as the example lab).

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/model/queue_file.hpp"

namespace pychron::experiment::lab {
namespace {

namespace fs = std::filesystem;

class LabTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("pychron-lab-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir_, fs::copy_options::recursive);
    fs::remove_all(dir_ / "data");
  }
  void TearDown() override { fs::remove_all(dir_); }

  LabPaths paths() const { return {dir_, dir_ / "extraction_line.toml", dir_ / "spectrometer.sim-integrated.toml"}; }
  QueueSpec example(const Lab& lab) const {
    auto q = load_queue_file((dir_ / "experiment.toml").string(), lab.ids);
    EXPECT_TRUE(q) << q.error().what;
    return q ? *q : QueueSpec{};
  }
  static bool has(const LabCheck& c, int run, const std::string& field, const std::string& text) {
    for (const auto& d : c.all())
      if (d.run == run && d.field == field && d.message.find(text) != std::string::npos) return true;
    return false;
  }

  fs::path dir_;
};

TEST_F(LabTest, TheExampleLabLoadsAndItsQueueChecks) {
  const Lab lab = load_lab(paths());
  EXPECT_TRUE(lab.problems.empty()) << lab.problems.front();
  ASSERT_TRUE(lab.line.has_value());
  ASSERT_TRUE(lab.spectrometer.has_value());
  EXPECT_TRUE(lab.plans->find("sim_multicollect"));
  EXPECT_TRUE(lab.scripts->has_script("sim_extract"));
  EXPECT_TRUE(lab.peak_centers.contains("default"));
  EXPECT_TRUE(lab.metric_catalog().detectors.contains("H1"));

  const auto q = example(lab);
  const auto check = check_lab_queue(lab, q);
  EXPECT_TRUE(check.ok());
  for (const auto& d : check.all()) ADD_FAILURE() << describe(d);
  EXPECT_EQ(check.report.run_estimates.size(), q.runs.size());
  EXPECT_GT(check.report.eta.count(), 0);
}

TEST_F(LabTest, ProblemsCarryTheirRow) {
  // A plan naming a peak-center config the lab lacks, and a conditional on a
  // detector the spectrometer does not have.
  {
    std::ifstream in(dir_ / "plans" / "sim_multicollect.toml");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto at = text.find("config = \"default\"");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, 18, "config = \"wide\"");
    std::ofstream(dir_ / "plans" / "sim_multicollect.toml") << text;
  }
  std::ofstream(dir_ / "conditionals" / "broken.toml") << "[[terminations]]\ncheck = \"IC9.inactive\"\n";
  const Lab lab = load_lab(paths());
  auto q = example(lab);
  q.runs[1].conditionals.push_back(ConditionalRef{"broken", "action"});
  q.runs[2].measurement.plan = "no_such_plan";

  const auto check = check_lab_queue(lab, q);
  EXPECT_FALSE(check.ok());
  EXPECT_TRUE(has(check, 0, "peak_center", "'wide'"));
  EXPECT_TRUE(has(check, 1, "conditionals", "unknown detector 'IC9'"));
  bool plan_reported = false;
  for (const auto& d : check.report.diagnostics) plan_reported |= d.run == 2 && d.message.find("no_such_plan") != std::string::npos;
  EXPECT_TRUE(plan_reported);
  // The peak-center problem is reported once, not once per run.
  int pc = 0;
  for (const auto& d : check.extra) pc += d.field == "peak_center" ? 1 : 0;
  EXPECT_EQ(pc, 1);
}

TEST_F(LabTest, FilesThatDoNotLoadAreLabProblems) {
  std::ofstream(dir_ / "peak_center.toml") << "[default]\nno_such_key = 1\n";
  const Lab lab = load_lab(paths());
  ASSERT_EQ(lab.problems.size(), 1u);
  const auto check = check_lab_queue(lab, QueueSpec{});
  EXPECT_FALSE(check.ok());
  ASSERT_FALSE(check.extra.empty());
  EXPECT_EQ(check.extra.front().field, "lab");
  EXPECT_EQ(describe(check.extra.front()).rfind("lab: ", 0), 0u);
}

TEST(LabDescribe, NamesTheRunOrTheQueue) {
  EXPECT_EQ(describe({Severity::Error, 3, "plan", "unknown"}), "runs[3].plan: unknown");
  EXPECT_EQ(describe({Severity::Warning, -1, "delays", "long"}), "queue.delays: long");
  EXPECT_EQ(describe({Severity::Error, -1, "queue.queue_conditionals", "unknown"}), "queue.queue_conditionals: unknown");
}

}  // namespace
}  // namespace pychron::experiment::lab
