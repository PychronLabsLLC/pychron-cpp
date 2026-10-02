// elctl exp validate / run against a scratch copy of configs/examples, which
// doubles as an example lab (plans/, scripts/, conditionals/, experiment.toml).

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "elctl_fixture.hpp"

namespace elctl::testing {
namespace {

namespace fs = std::filesystem;

class ElctlExpTest : public ElctlTest {
 protected:
  void SetUp() override {
    ElctlTest::SetUp();
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir_ / "lab",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    fs::remove_all(dir_ / "lab" / "data");
  }
  std::string lab(const std::string& f) const { return (dir_ / "lab" / f).string(); }

  Outcome exp(std::vector<std::string> args, bool sim = false) const {
    std::vector<std::string> all{"-c", lab("extraction_line.toml")};
    if (sim) all.push_back("--sim");
    all.push_back("exp");
    all.insert(all.end(), args.begin(), args.end());
    return run_raw(all);
  }
};

TEST_F(ElctlExpTest, ValidateTheExampleQueue) {
  auto o = exp({"validate", lab("experiment.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml")});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "queue sim-example: 3 run(s), ETA")) << o.out;
  EXPECT_TRUE(contains(o.out, "blank_unknown")) << o.out;
  EXPECT_TRUE(contains(o.out, "ok: ")) << o.out;
}

TEST_F(ElctlExpTest, ValidateReportsMissingPlansScriptsAndBadConditionals) {
  std::ofstream(dir_ / "lab" / "bad.toml") << R"([queue]
name = "bad"
[[runs]]
identifier = "1"
measurement = { plan = "no_such_plan" }
[[runs]]
identifier = "2"
extraction = { script = "no_such_script" }
measurement = { plan = "sim_multicollect" }
conditionals = ["broken"]
)";
  std::ofstream(dir_ / "lab" / "conditionals" / "broken.toml") << "[[terminations]]\ncheck = \"IC9.inactive\"\n";
  auto o = exp({"validate", lab("bad.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml")});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "no_such_plan")) << o.err;
  EXPECT_TRUE(contains(o.err, "no_such_script")) << o.err;
  EXPECT_TRUE(contains(o.err, "unknown detector 'IC9'")) << o.err;
}

TEST_F(ElctlExpTest, RunTheExampleQueueOnTheSim) {
  const auto data = (dir_ / "out").string();
  auto o = exp({"run", lab("experiment.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"), "--data",
                data, "--sim-speed", "400"},
               true);
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "run 0 bu-1: success")) << o.out;
  EXPECT_TRUE(contains(o.out, "run 2 66001-2: success")) << o.out;
  EXPECT_TRUE(contains(o.out, "queue completed")) << o.out;
  EXPECT_TRUE(fs::exists(dir_ / "out" / "records" / "66001" / "66001-1.json"));
  EXPECT_TRUE(fs::exists(dir_ / "out" / "records" / "bu" / "bu-1.json"));
  // The example plan peak-centers after each run (peak_center.toml [default]).
  {
    std::ifstream in(dir_ / "out" / "records" / "66001" / "66001-1.json");
    const std::string record((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_TRUE(contains(record, "Ar40 on H1 (default): center")) << record.substr(0, 2000);
    EXPECT_TRUE(contains(record, "table updated")) << record.substr(0, 2000);
  }

  // Everything ran, so a resume has nothing left to do.
  auto again = exp({"run", lab("experiment.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"),
                    "--data", data, "--sim-speed", "400", "--resume"},
                   true);
  EXPECT_EQ(again.code, 0) << again.err;
  EXPECT_TRUE(contains(again.out, "0/0 run(s)")) << again.out;

  // --from skips rows.
  auto from = exp({"run", lab("experiment.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"),
                   "--data", data, "--sim-speed", "400", "--from", "2"},
                  true);
  EXPECT_EQ(from.code, 0) << from.err;
  EXPECT_TRUE(contains(from.out, "run 2 66001-3: success")) << from.out;
  EXPECT_FALSE(contains(from.out, "run 0 ")) << from.out;
}

TEST_F(ElctlExpTest, DryRunTouchesNothing) {
  auto o = exp({"run", lab("experiment.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"), "--dry-run"},
               true);
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "ok: ")) << o.out;
  EXPECT_FALSE(contains(o.out, "started")) << o.out;
  EXPECT_FALSE(fs::exists(dir_ / "lab" / "data"));
}

TEST_F(ElctlExpTest, InvalidQueueIsNotRun) {
  std::ofstream(dir_ / "lab" / "bad.toml") << "[queue]\n[[runs]]\nidentifier = \"1\"\nmeasurement = { plan = \"nope\" }\n";
  auto o = exp({"run", lab("bad.toml"), "--sim-speed", "100"}, true);
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "nothing was run")) << o.err;
}

TEST_F(ElctlExpTest, UsageErrors) {
  EXPECT_EQ(exp({}).code, 2);
  EXPECT_EQ(exp({"launch", lab("experiment.toml")}).code, 2);
  EXPECT_EQ(exp({"run"}).code, 2);
  EXPECT_EQ(exp({"run", lab("experiment.toml"), "--sim-speed", "10"}).code, 2);  // needs --sim
  EXPECT_EQ(exp({"run", lab("experiment.toml"), "--from", "1", "--resume"}).code, 2);
  EXPECT_EQ(exp({"run", lab("experiment.toml"), "--from", "x"}).code, 2);
  auto missing = exp({"validate", lab("nope.toml")});
  EXPECT_EQ(missing.code, 1);
}

}  // namespace
}  // namespace elctl::testing
