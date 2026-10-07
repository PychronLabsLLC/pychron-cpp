// elctl exp validate / run against a scratch copy of configs/examples, which
// doubles as an example lab (plans/, scripts/, conditionals/, experiment.toml).

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "elctl_fixture.hpp"
#include "exp.hpp"
#include "pychron/scripting/script_host.hpp"

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
    // Runtime state a local session left beside the examples (a locked valve,
    // say) is not part of them.
    std::vector<fs::path> state;
    for (const auto& entry : fs::recursive_directory_iterator(dir_ / "lab")) {
      if (entry.is_regular_file() && entry.path().filename().string().ends_with(".state.toml")) {
        state.push_back(entry.path());
      }
    }
    for (const auto& file : state) fs::remove(file);
  }
  std::string lab(const std::string& f) const { return (dir_ / "lab" / f).string(); }

  Outcome exp(std::vector<std::string> args, bool sim = false) const {
    std::vector<std::string> all{"-c", lab("extraction_line.toml")};
    if (sim) all.push_back("--sim");
    all.push_back("exp");
    all.insert(all.end(), args.begin(), args.end());
    return run_raw(all);
  }

  // The example queue on the sim at unlimited speed, records under <dir>/out.
  Outcome run_example_queue() const {
    return exp({"run", lab("experiment.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"), "--data",
                (dir_ / "out").string(), "--sim-speed", "max"},
               true);
  }

  // When a record says its analysis began (identity.timestamp, UTC to the second).
  std::optional<std::chrono::sys_seconds> stamp(const std::string& record) const {
    std::ifstream in(dir_ / "out" / "records" / record);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto key = text.find("\"timestamp\"");
    if (key == std::string::npos) return std::nullopt;
    const auto open = text.find('"', text.find(':', key));
    if (open == std::string::npos) return std::nullopt;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (std::sscanf(text.c_str() + open, "\"%d-%d-%dT%d:%d:%dZ\"", &y, &mo, &d, &h, &mi, &sec) != 6) return std::nullopt;
    const std::chrono::year_month_day day{std::chrono::year{y}, std::chrono::month{static_cast<unsigned>(mo)},
                                          std::chrono::day{static_cast<unsigned>(d)}};
    return std::chrono::sys_days{day} + std::chrono::hours{h} + std::chrono::minutes{mi} + std::chrono::seconds{sec};
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
  // The example queue names extraction scripts, which only the CPython host runs.
  if (!pychron::scripting::scripting_enabled()) GTEST_SKIP() << "built without PYCHRON_SCRIPTING";
  const auto data = (dir_ / "out").string();
  auto o = exp({"run", lab("experiment.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"), "--data",
                data, "--sim-speed", "max"},
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
                    "--data", data, "--sim-speed", "max", "--resume"},
                   true);
  EXPECT_EQ(again.code, 0) << again.err;
  EXPECT_TRUE(contains(again.out, "0/0 run(s)")) << again.out;

  // --from skips rows.
  auto from = exp({"run", lab("experiment.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"),
                   "--data", data, "--sim-speed", "max", "--from", "2"},
                  true);
  EXPECT_EQ(from.code, 0) << from.err;
  EXPECT_TRUE(contains(from.out, "run 2 66001-3: success")) << from.out;
  EXPECT_FALSE(contains(from.out, "run 0 ")) << from.out;
}

// The queue is some eight minutes of delays, extraction and counting; on the
// virtual clock at unlimited speed nothing waits for real time. The bound is
// for the whole command, interpreter start-up included, on a slow machine:
// thirty real seconds are still far short of the simulated minutes.
TEST_F(ElctlExpTest, SimulatedQueueTakesNoRealTime) {
  if (!pychron::scripting::scripting_enabled()) GTEST_SKIP() << "built without PYCHRON_SCRIPTING";
  const auto began = std::chrono::steady_clock::now();
  auto o = run_example_queue();
  const auto took = std::chrono::steady_clock::now() - began;
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "3/3 run(s) succeeded")) << o.out;
  EXPECT_LT(took, std::chrono::seconds(30));
}

// Simulated time starts at the real time of day and runs on from there: each
// analysis is stamped later than the one before by at least what the one
// before spent counting its main block (sim_multicollect: 2 cycles of two
// hops of 15 one-second counts).
TEST_F(ElctlExpTest, SimulatedAnalysesAreStampedInSimulatedTime) {
  if (!pychron::scripting::scripting_enabled()) GTEST_SKIP() << "built without PYCHRON_SCRIPTING";
  const auto began = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  auto o = run_example_queue();
  const auto ended = std::chrono::system_clock::now();
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  const auto blank = stamp("bu/bu-1.json");
  const auto first = stamp("66001/66001-1.json");
  const auto second = stamp("66001/66001-2.json");
  ASSERT_TRUE(blank && first && second) << o.out;
  const auto measurement = std::chrono::seconds(2 * (15 + 15));
  EXPECT_GE(*blank, began);  // the epoch is now, not 1970
  EXPECT_GE(*first - *blank, measurement);
  EXPECT_GE(*second - *first, measurement);
  EXPECT_GT(*second, ended);  // and it has run ahead of real time
}

// At a thousandth of real speed the queue's first delay alone is minutes long
// and simulated time all but stands: the operator's Ctrl-C is heard in real
// time all the same. The two interrupts are kept standing rather than timed,
// since the command clears the count when it starts the queue. Thirty real
// seconds allow the command its start-up on a slow machine and are nothing
// beside the days the queue would take at that speed.
TEST_F(ElctlExpTest, AnInterruptStopsAPacedQueueInRealTime) {
  if (!pychron::scripting::scripting_enabled()) GTEST_SKIP() << "built without PYCHRON_SCRIPTING";
  std::atomic<bool> done{false};
  std::thread operator_([&] {
    while (!done) {
      int seen = elctl::interrupt_count().load();
      while (seen < 2 && !elctl::interrupt_count().compare_exchange_weak(seen, 2)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  const auto began = std::chrono::steady_clock::now();
  auto o = exp({"run", lab("experiment.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"), "--data",
                (dir_ / "out").string(), "--sim-speed", "0.001"},
               true);
  const auto took = std::chrono::steady_clock::now() - began;
  done = true;
  operator_.join();
  elctl::interrupt_count() = 0;
  EXPECT_EQ(o.code, 1) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "interrupt: stopping after the current run")) << o.out;
  EXPECT_TRUE(contains(o.out, "interrupt: cancelling")) << o.out;
  EXPECT_TRUE(contains(o.out, "queue cancelled: cancelled by the operator")) << o.out;
  EXPECT_TRUE(contains(o.out, "0/")) << o.out;
  EXPECT_LT(took, std::chrono::seconds(30));
}

// A run that gives up after the simulated beam is in place leaves nothing
// behind that refers to its clock: the next run in the same process is whole.
TEST_F(ElctlExpTest, ARunThatFailsEarlyLeavesTheNextOneClean) {
  if (!pychron::scripting::scripting_enabled()) GTEST_SKIP() << "built without PYCHRON_SCRIPTING";
  const auto data = (dir_ / "out").string();
  // Nothing has run into this directory, so there is nothing to resume.
  auto failed = exp({"run", lab("experiment.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"),
                     "--data", data, "--sim-speed", "max", "--resume"},
                    true);
  EXPECT_EQ(failed.code, 1) << failed.out;
  EXPECT_TRUE(contains(failed.err, "error: --resume: ")) << failed.err;
  auto o = exp({"run", lab("experiment.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"), "--data",
                data, "--sim-speed", "max"},
               true);
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "3/3 run(s) succeeded")) << o.out;
}

// What a run says is printed under its state lines: here, that each hole of
// the example laser queue was centered, and by how much.
TEST_F(ElctlExpTest, ARunsLogIsPrinted) {
  if (!pychron::scripting::scripting_enabled()) GTEST_SKIP() << "built without PYCHRON_SCRIPTING";
  auto o = exp({"run", lab("experiment.laser.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"),
                "--data", (dir_ / "out").string(), "--sim-speed", "max"},
               true);
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "\n  66001: hole 3: centered, moved ")) << o.out;
  EXPECT_TRUE(contains(o.out, "\n  66001: hole 7: centered, moved ")) << o.out;
  // in its place: after the run reached extraction, before it finished
  const auto extracting = o.out.find("  66001: extracting");
  const auto said = o.out.find("  66001: hole 3: centered");
  const auto finished = o.out.find("run 0 66001-1: success");
  ASSERT_NE(extracting, std::string::npos) << o.out;
  ASSERT_NE(finished, std::string::npos) << o.out;
  EXPECT_LT(extracting, said);
  EXPECT_LT(said, finished);
  std::ifstream in(dir_ / "out" / "records" / "66001" / "66001-1.json");
  const std::string record((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_TRUE(contains(record, "hole 3: centered, moved ")) << record.substr(0, 2000);
}

TEST_F(ElctlExpTest, DryRunTouchesNothing) {
  auto o = exp({"run", lab("experiment.toml"), "--spectrometer", lab("spectrometer.sim-integrated.toml"), "--dry-run"},
               true);
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "ok: ")) << o.out;
  EXPECT_FALSE(contains(o.out, "started")) << o.out;
  EXPECT_FALSE(fs::exists(dir_ / "lab" / "data"));
}

// The simulated spectrometer is joined to the simulated line: the lab's
// sim.toml names a detector, which only the two together can check.
TEST_F(ElctlExpTest, ASimTomlDetectorTheSpectrometerLacksStopsTheRun) {
  std::ofstream(dir_ / "lab" / "sim.toml") << "[detectors.H9]\nbaseline = 50\n";
  auto o = run_example_queue();
  EXPECT_EQ(o.code, 1) << o.out;
  EXPECT_TRUE(contains(o.err, "sim.toml")) << o.err;
  EXPECT_TRUE(contains(o.err, "detectors.H9")) << o.err;
  EXPECT_TRUE(contains(o.err, "known: ")) << o.err;
  EXPECT_FALSE(fs::exists(dir_ / "out" / "records")) << "nothing was run";

  // One it has is that detector's baseline: 50 fA, where the fixed beam the
  // spectrometer has on its own reads nothing off the peaks. (The example
  // queue names scripts, which only the CPython host runs.)
  if (!pychron::scripting::scripting_enabled()) return;
  std::ofstream(dir_ / "lab" / "sim.toml") << "[detectors.H1]\nbaseline = 50\n";
  auto ok = run_example_queue();
  ASSERT_EQ(ok.code, 0) << ok.out << ok.err;
  std::ifstream in(dir_ / "out" / "records" / "bu" / "bu-1.json");
  const std::string record((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  // results.baselines.H1.value, the last key of its table.
  const auto baselines = record.find("\"baselines\"");
  ASSERT_NE(baselines, std::string::npos);
  const auto h1 = record.find("\"H1\"", baselines);
  ASSERT_NE(h1, std::string::npos);
  const auto value = record.find("\"value\"", h1);
  ASSERT_NE(value, std::string::npos);
  EXPECT_NEAR(std::strtod(record.c_str() + record.find(':', value) + 1, nullptr), 50.0, 2.0) << record.substr(h1, 400);
}

TEST_F(ElctlExpTest, InvalidQueueIsNotRun) {
  std::ofstream(dir_ / "lab" / "bad.toml") << "[queue]\n[[runs]]\nidentifier = \"1\"\nmeasurement = { plan = \"nope\" }\n";
  auto o = exp({"run", lab("bad.toml"), "--sim-speed", "100"}, true);
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "nothing was run")) << o.err;
}

TEST_F(ElctlExpTest, NotifySendsATestOnEachChannel) {
  auto none = exp({"notify", "--lab", (dir_ / "lab").string()});
  EXPECT_EQ(none.code, 1);
  EXPECT_TRUE(contains(none.err, "no notifications are configured")) << none.err;

#ifdef _WIN32
  const std::string ok = R"(["cmd", "/c", "exit 0"])", bad = R"(["cmd", "/c", "exit 3"])";
#else
  const std::string ok = R"(["true"])", bad = R"(["sh", "-c", "echo nope; exit 3"])";
#endif
  std::ofstream(dir_ / "lab" / "notifications.toml")
      << "[[command]]\nname = \"log\"\nargv = " << ok << "\n[[command]]\nname = \"pager\"\nargv = " << bad << "\n";
  auto o = exp({"notify", "--lab", (dir_ / "lab").string()});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.out, "sent: log")) << o.out;
  EXPECT_TRUE(contains(o.err, "failed: pager: ")) << o.err;
  EXPECT_TRUE(contains(o.err, "exited with 3")) << o.err;

  std::ofstream(dir_ / "lab" / "notifications.toml") << "[[command]]\nargv = 3\n";
  auto broken = exp({"notify", "--lab", (dir_ / "lab").string()});
  EXPECT_EQ(broken.code, 1);
  EXPECT_TRUE(contains(broken.err, "command[0].argv")) << broken.err;
  EXPECT_EQ(exp({"notify", lab("experiment.toml")}).code, 2);
}

TEST_F(ElctlExpTest, UsageErrors) {
  EXPECT_EQ(exp({}).code, 2);
  EXPECT_EQ(exp({"launch", lab("experiment.toml")}).code, 2);
  EXPECT_EQ(exp({"run"}).code, 2);
  EXPECT_EQ(exp({"run", lab("experiment.toml"), "--sim-speed", "10"}).code, 2);  // needs --sim
  EXPECT_EQ(exp({"run", lab("experiment.toml"), "--sim-speed", "max"}).code, 2);  // so does max
  for (const char* bad : {"fast", "nan", "inf", "infinity", "4x", "maximum", ""}) {
    auto o = exp({"run", lab("experiment.toml"), "--sim-speed", bad}, true);
    EXPECT_EQ(o.code, 2) << bad;
    EXPECT_TRUE(contains(o.err, "--sim-speed needs a number")) << bad << ": " << o.err;
  }
  for (const char* bad : {"0", "-2"}) {
    auto o = exp({"run", lab("experiment.toml"), "--sim-speed", bad}, true);
    EXPECT_EQ(o.code, 2) << bad;
    EXPECT_TRUE(contains(o.err, "--sim-speed must be positive")) << bad << ": " << o.err;
  }
  EXPECT_EQ(exp({"run", lab("experiment.toml"), "--from", "1", "--resume"}).code, 2);
  EXPECT_EQ(exp({"run", lab("experiment.toml"), "--from", "x"}).code, 2);
  auto missing = exp({"validate", lab("nope.toml")});
  EXPECT_EQ(missing.code, 1);
}

}  // namespace
}  // namespace elctl::testing
