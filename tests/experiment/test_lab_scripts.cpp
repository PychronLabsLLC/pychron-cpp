// The lab's scripts for the script editor, on a scratch copy of the example lab.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "pychron/experiment/lab/scripts.hpp"

namespace pychron::experiment::lab {
namespace {

namespace fs = std::filesystem;
using scripting::ScriptKind;

class LabScriptsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("pychron-scripts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir_, fs::copy_options::recursive);
    fs::create_directories(dir_ / "scripts" / "extraction" / "co2");
    std::ofstream(dir_ / "scripts" / "extraction" / "co2" / "degas.py") << "def main():\n    sleep(1)\n";
    fs::create_directories(dir_ / "scripts" / "lib");
    std::ofstream(dir_ / "scripts" / "lib" / "pump.py") << "def main():\n    sleep(5)\n";
    lab_ = load_lab({dir_, dir_ / "extraction_line.toml", dir_ / "spectrometer.sim-integrated.toml"});
  }
  void TearDown() override { fs::remove_all(dir_); }

  fs::path dir_;
  Lab lab_;
};

TEST_F(LabScriptsTest, ListsScriptsWithQueueNamesAndPaths) {
  const auto files = lab_scripts(lab_);
  std::vector<std::string> names;
  for (const auto& f : files) names.push_back(std::string(scripting::to_string(f.kind)) + "/" + f.name);
  EXPECT_EQ(names, (std::vector<std::string>{"extraction/co2:degas", "extraction/sim_extract",
                                             "post_measurement/sim_pump", "extraction/lib:pump"}));
  EXPECT_TRUE(fs::is_regular_file(files[1].path));
  auto p = script_path(lab_, ScriptKind::Extraction, "co2:degas");
  ASSERT_TRUE(p);
  EXPECT_EQ(fs::canonical(*p), fs::canonical(files[0].path));
  EXPECT_FALSE(script_path(lab_, ScriptKind::Extraction, "../escape"));
}

TEST_F(LabScriptsTest, CompletionWordsFollowTheKind) {
  const auto ex = completion_words(ScriptKind::Extraction);
  auto has = [](const std::vector<std::string>& w, const char* s) { return std::binary_search(w.begin(), w.end(), s); };
  EXPECT_TRUE(std::is_sorted(ex.begin(), ex.end()));
  EXPECT_TRUE(has(ex, "sleep"));
  EXPECT_TRUE(has(ex, "close"));
  EXPECT_TRUE(has(ex, "gosub"));
  EXPECT_TRUE(has(ex, "run_identifier"));
  EXPECT_TRUE(has(ex, "duration"));
  EXPECT_TRUE(has(ex, "opt"));
  EXPECT_TRUE(has(ex, "range"));
  EXPECT_TRUE(has(ex, "while"));
  EXPECT_FALSE(has(ex, "open_file"));  // not a builtin scripts get
  // post-measurement-only commands appear only there.
  const auto pm = completion_words(ScriptKind::PostMeasurement);
  EXPECT_TRUE(has(pm, "signal_pump_time_start"));
  EXPECT_FALSE(has(ex, "signal_pump_time_start"));
}

TEST_F(LabScriptsTest, FindsGosubsWithTheirPlace) {
  const std::string text =
      "def main():\n"
      "    gosub('lib_pump')\n"
      "    # gosub('commented')\n"
      "    x = 'gosub(\"in_a_string\")'\n"
      "    gosub(\"co2:degas\", duration=3); mygosub('no')\n";
  const auto refs = find_gosubs(text);
  ASSERT_EQ(refs.size(), 2u);
  EXPECT_EQ(refs[0], (GosubRef{2, 11, 8, "lib_pump"}));
  EXPECT_EQ(refs[1].line, 5);
  EXPECT_EQ(refs[1].name, "co2:degas");
  EXPECT_EQ(text.substr(text.find("co2:degas"), 9), "co2:degas");
}

TEST_F(LabScriptsTest, ResolvesGosubsInTheKindThenLib) {
  auto in_kind = resolve_gosub(lab_, ScriptKind::Extraction, "co2:degas");
  ASSERT_TRUE(in_kind);
  EXPECT_EQ(in_kind->kind, ScriptKind::Extraction);
  auto in_lib = resolve_gosub(lab_, ScriptKind::PostMeasurement, "pump");
  ASSERT_TRUE(in_lib);
  EXPECT_EQ(in_lib->name, "lib:pump");
  EXPECT_FALSE(resolve_gosub(lab_, ScriptKind::Extraction, "nope"));
}

TEST_F(LabScriptsTest, ChecksAndEstimatesWithoutHardware) {
  auto host = scripting::make_script_host();
  const auto env = editor_environment(lab_);
  ASSERT_TRUE(env.valve_names.has_value());
  EXPECT_FALSE(env.valve_names->empty());
  const std::string good = "def main():\n    close('C')\n    sleep(2)\n    sleep(3)\n";
  auto c = check_script(*host, lab_, ScriptKind::Extraction, "good", good);
  if (!host->available()) {
    EXPECT_FALSE(c.error.empty());  // the stub cannot check
    GTEST_SKIP() << "built without embedded Python";
  }
  EXPECT_TRUE(c.report.ok());
  ASSERT_TRUE(c.estimate.has_value()) << c.error;
  EXPECT_GE(c.estimate->total.count(), 5.0);

  const std::string bad = "def main():\n    close('NOPE')\n    frobnicate(3)\n";
  c = check_script(*host, lab_, ScriptKind::Extraction, "bad", bad);
  EXPECT_FALSE(c.report.ok());
  EXPECT_FALSE(c.estimate.has_value());
  bool unknown_command = false, unknown_valve = false;
  for (const auto& d : c.report.diagnostics) {
    unknown_command |= d.line == 3 && d.message.find("frobnicate") != std::string::npos;
    unknown_valve |= d.line == 2 && d.message.find("NOPE") != std::string::npos;
  }
  EXPECT_TRUE(unknown_command);
  EXPECT_TRUE(unknown_valve);

  // A while loop makes the estimate a lower bound.
  c = check_script(*host, lab_, ScriptKind::Extraction, "loop", "def main():\n    while False:\n        sleep(1)\n    sleep(1)\n");
  ASSERT_TRUE(c.estimate.has_value()) << c.error;
}

}  // namespace
}  // namespace pychron::experiment::lab
