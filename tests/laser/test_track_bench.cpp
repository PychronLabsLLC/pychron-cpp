// The bench: the real dragonfly stack against the simulated grain, on a
// manual clock in a scratch directory of its own (grain simulator design,
// section 7).

#include "pychron/laser/track_bench.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

using namespace pychron;
using namespace pychron::laser;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path dir;
  TempDir() : dir(fs::temp_directory_path() / ("pychron_track_test_" + std::to_string(std::random_device{}()))) {
    fs::create_directories(dir);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  ~TempDir() {
    std::error_code ignored;
    fs::remove_all(dir, ignored);
  }
};

// A scenario: top level keys, then the tables given.
std::string text(double duration, int seeds, std::string_view tables = "", double output_percent = 20,
                 std::string_view offset = "[0.1, -0.1]") {
  std::ostringstream out;
  out << "output_percent = " << output_percent << "\nduration = " << duration << "\nseeds = " << seeds
      << "\nseed = 1\npoll_ms = 100\ngrain_offset_mm = " << offset << "\n"
      << tables << "\n";
  return out.str();
}

TrackScenario scenario(const std::string& toml) {
  auto parsed = parse_track_scenario(toml, "scenario.toml");
  EXPECT_TRUE(parsed) << (parsed ? "" : parsed.error().what);
  return parsed ? *parsed : TrackScenario{};
}

std::vector<TrackCombo> run(const std::string& toml, const TrackOptions& options = {}) {
  auto done = run_track(scenario(toml), options);
  EXPECT_TRUE(done) << (done ? "" : done.error().what);
  return done ? *done : std::vector<TrackCombo>{};
}

// Everything a scenario can be refused for, short of running it.
Result<std::vector<TrackCombo>> checked(const std::string& toml) {
  auto parsed = parse_track_scenario(toml, "scenario.toml");
  if (!parsed) return fail(std::move(parsed).error());
  TrackOptions options;
  options.check_only = true;
  return run_track(*parsed, options);
}

void expect_refused(const std::string& toml, std::string_view named) {
  const auto result = checked(toml);
  ASSERT_FALSE(result) << toml;
  EXPECT_EQ(result.error().kind, ErrorKind::Config) << result.error().what;
  EXPECT_NE(result.error().what.find(named), std::string::npos) << result.error().what;
}

std::string read_file(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::vector<std::string> split(const std::string& line, char by) {
  std::vector<std::string> out;
  std::string field;
  std::istringstream in(line);
  while (std::getline(in, field, by)) out.push_back(field);
  if (!line.empty() && line.back() == by) out.emplace_back();
  return out;
}

const TrackCombo* labelled(const std::vector<TrackCombo>& combos, std::string_view label) {
  const auto it = std::find_if(combos.begin(), combos.end(), [&](const TrackCombo& c) { return c.label == label; });
  return it == combos.end() ? nullptr : &*it;
}

}  // namespace

TEST(TrackBench, SameScenarioSameTable) {
  const TempDir temp;
  const std::string toml = text(5, 2);
  TrackOptions first, second;
  first.trace = temp.dir / "first.csv";
  second.trace = temp.dir / "second.csv";
  const auto a = run(toml, first);
  const auto b = run(toml, second);
  ASSERT_EQ(a.size(), 1U);
  EXPECT_EQ(track_table(a), track_table(b));
  const std::string trace = read_file(first.trace);
  EXPECT_FALSE(trace.empty());
  EXPECT_EQ(trace, read_file(second.trace));
}

TEST(TrackBench, ASteadyGrainIsHeldOn) {
  // 20 s, not the 5 s of the others: the glow takes time to build, so the
  // tracker's first search costs a fixed half second off the grain.
  const auto combos = run(text(20, 2));
  ASSERT_EQ(combos.size(), 1U);
  EXPECT_EQ(combos[0].label, "");
  EXPECT_EQ(combos[0].failed, 0) << combos[0].first_error;
  EXPECT_GT(combos[0].summary.mean.on_pct, 95.0);
}

TEST(TrackBench, ATrackerThatDoesNotFollowScoresWorse) {
  const auto combos = run(text(20, 1,
                               "[grain]\ncreep_mm_per_s = [-0.05, 0]\n"  // away from where its first search leaves the beam
                               "[sweep.pattern]\naggressiveness = [0, 1]\n"));
  ASSERT_EQ(combos.size(), 2U);
  const TrackCombo* still = labelled(combos, "pattern.aggressiveness=0");
  const TrackCombo* following = labelled(combos, "pattern.aggressiveness=1");
  ASSERT_NE(still, nullptr);
  ASSERT_NE(following, nullptr);
  EXPECT_GT(following->summary.mean.on_pct, still->summary.mean.on_pct);
}

TEST(TrackBench, ABoolSweepValueIsLabelledTrueOrFalse) {
  // No dragonfly key is a bool yet, so the combination is refused; its label says which.
  expect_refused(text(1, 1, "[sweep.pattern]\nsingle_pass = [true, false]\n"), "pattern.single_pass=true");
  expect_refused(text(1, 1, "[sweep.pattern]\nsingle_pass = [false]\n"), "pattern.single_pass=false");
}

TEST(TrackBench, ALossOpenAtTheEndCountsToTheDurationNotTheTripHome) {
  // A grain that goes dark for good almost at once: the beam starts on it,
  // locks, then searches away and never finds it again. The stage's trip home follows the duration.
  constexpr double kDuration = 20;
  const TempDir temp;
  TrackOptions options;
  options.trace = temp.dir / "trace.csv";
  const auto combos = run(text(kDuration, 1,
                               "[grain]\ndropouts_per_s = 0.15\ndropout_s = 60\n"),
                          options);
  ASSERT_EQ(combos.size(), 1U);
  const TrackSummary& s = combos[0].summary;
  ASSERT_EQ(s.mean.lost, 1.0);
  // When it went off: the first poll of the stretch that runs to the end.
  double went_off = -1;
  std::istringstream trace(read_file(options.trace));
  std::string line;
  std::getline(trace, line);
  while (std::getline(trace, line)) {
    const auto f = split(line, ',');
    ASSERT_EQ(f.size(), 11U) << line;
    const bool off = std::hypot(std::stod(f[3]) - std::stod(f[5]), std::stod(f[4]) - std::stod(f[6])) > 0.4;
    if (!off) went_off = -1;
    else if (went_off < 0) went_off = std::stod(f[2]);
  }
  ASSERT_GE(went_off, 0);
  EXPECT_LE(s.mean.back_s, kDuration - went_off + 0.05);
}

TEST(TrackBench, EveryCombinationOnTheSameSeeds) {
  const TempDir temp;
  TrackOptions options;
  options.trace = temp.dir / "trace.csv";
  const auto combos = run(text(1, 2,
                               "[sweep.pattern]\naggressiveness = [0.5, 1]\n"
                               "[sweep.grain]\njump_mm = [0.1, 0.2, 0.3]\n"),
                          options);
  ASSERT_EQ(combos.size(), 6U);
  std::set<std::string> labels;
  for (const auto& combo : combos) labels.insert(combo.label);
  EXPECT_EQ(labels.size(), 6U);
  EXPECT_TRUE(labels.contains("pattern.aggressiveness=0.5 grain.jump_mm=0.3"));

  std::set<std::pair<std::string, std::string>> runs;
  std::set<std::string> seeds;
  std::istringstream trace(read_file(options.trace));
  std::string line;
  std::getline(trace, line);  // the header
  while (std::getline(trace, line)) {
    const auto fields = split(line, ',');
    ASSERT_GE(fields.size(), 2U) << line;
    runs.emplace(fields[0], fields[1]);
    seeds.insert(fields[1]);
  }
  EXPECT_EQ(runs.size(), 12U);
  EXPECT_EQ(seeds, (std::set<std::string>{"1", "2"}));
}

TEST(TrackBench, TooManyRunsAreRefused) {
  // of the scenario, of combinations times seeds, and of what --seeds says
  EXPECT_FALSE(parse_track_scenario(text(1, 10001), "scenario.toml"));
  expect_refused(text(1, 5000, "[sweep.pattern]\naggressiveness = [0.5, 1, 2]\n"), "10000");

  TrackOptions options;
  options.seeds = 10001;
  auto single = run_track(scenario(text(3600, 1)), options);
  ASSERT_FALSE(single);
  EXPECT_EQ(single.error().kind, ErrorKind::Config);

  // --seeds replaces the scenario's, so that is the number that counts
  options.seeds = 1;
  options.check_only = true;
  EXPECT_TRUE(run_track(scenario(text(1, 5000, "[sweep.pattern]\naggressiveness = [0.5, 1, 2]\n")), options));
}

TEST(TrackBench, UnknownKeysAreRefused) {
  expect_refused("nonsense = 1\n" + text(1, 1), "nonsense");
  expect_refused(text(1, 1, "[camera]\nnonsense = 1\n"), "nonsense");
  expect_refused(text(1, 1, "[grain]\nnonsense = 1\n"), "nonsense");
  expect_refused(text(1, 1, "[pattern]\nnonsense = 1\n"), "nonsense");
  expect_refused(text(1, 1, "[nonsense]\n"), "nonsense");
  expect_refused(text(1, 1, "[sweep.nothing]\naggressiveness = [1, 2]\n"), "nothing");
  expect_refused(text(1, 1, "[sweep.camera]\nnonsense = [1, 2]\n"), "nonsense");
  expect_refused(text(1, 1, "[sweep.grain]\nnonsense = [1, 2]\n"), "nonsense");
}

TEST(TrackBench, TopLevelKeysAreInRange) {
  const auto with = [](std::string_view line) { return std::string(line) + "\n"; };
  for (const char* bad : {"output_percent = 0", "output_percent = 101", "duration = 0", "duration = 3601", "seeds = 0",
                          "poll_ms = 9", "poll_ms = 1001", "seed = -1", "grain_offset_mm = [1]"}) {
    EXPECT_FALSE(parse_track_scenario(with(bad), "scenario.toml")) << bad;
  }
  EXPECT_TRUE(parse_track_scenario(with("output_percent = 100\nduration = 3600\npoll_ms = 10"), "scenario.toml"));
}

TEST(TrackBench, PatternMayNotNameKindOrDuration) {
  expect_refused(text(1, 1, "[pattern]\nkind = \"polygon\"\n"), "kind");
  expect_refused(text(1, 1, "[pattern]\nduration = 2\n"), "duration");
  expect_refused(text(1, 1, "[sweep.pattern]\nduration = [1, 2]\n"), "duration");
}

TEST(TrackBench, ABadSweepValueIsRefusedFirst) {
  const TempDir temp;
  TrackOptions options;
  options.trace = temp.dir / "trace.csv";
  const auto done = run_track(scenario(text(3600, 10, "[sweep.pattern]\naggressiveness = [1, -1]\n")), options);
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().kind, ErrorKind::Config);
  EXPECT_NE(done.error().what.find("aggressiveness"), std::string::npos) << done.error().what;
  EXPECT_FALSE(fs::exists(options.trace));
}

TEST(TrackBench, AnEmptySweepIsRefused) {
  expect_refused(text(1, 1, "[sweep.pattern]\naggressiveness = []\n"), "aggressiveness");
}

TEST(TrackBench, AnUnwritableTraceIsRefusedFirst) {
  const TempDir temp;
  std::ofstream(temp.dir / "regular") << "a file, not a directory\n";
  TrackOptions options;
  options.trace = temp.dir / "regular" / "trace.csv";
  const auto done = run_track(scenario(text(3600, 1)), options);
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().kind, ErrorKind::Io) << done.error().what;
}

TEST(TrackBench, AnUnwritableFramesDirectoryIsRefusedFirst) {
  const TempDir temp;
  std::ofstream(temp.dir / "regular") << "a file, not a directory\n";
  TrackOptions options;
  options.frames = temp.dir / "regular" / "frames";
  const auto done = run_track(scenario(text(3600, 1)), options);
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().kind, ErrorKind::Io) << done.error().what;
}

TEST(TrackBench, AGrainTooDimIsARunNotAFailure) {
  // A narrow beam: its search passes over the grain, and never stays a second.
  const auto combos = run(text(5, 2, "[grain]\nbeam_radius_mm = 0.05\n", 1));
  ASSERT_EQ(combos.size(), 1U);
  EXPECT_EQ(combos[0].failed, 0) << combos[0].first_error;
  EXPECT_DOUBLE_EQ(combos[0].summary.mean.lock_s, 5.0);
}

TEST(TrackBench, APatternErrorIsARunThatFailed) {
  const auto combos = run(text(1, 2, "[pattern]\ntarget_radius = 0.001\n"));  // a look of less than a pixel
  ASSERT_EQ(combos.size(), 1U);
  EXPECT_EQ(combos[0].failed, 2);
  EXPECT_NE(combos[0].first_error.find("target_radius"), std::string::npos) << combos[0].first_error;
  EXPECT_NE(track_table(combos).find("failed 2"), std::string::npos);
}

TEST(TrackBench, FramesOfOneRun) {
  const TempDir temp;
  TrackOptions options;
  options.trace = temp.dir / "trace.csv";
  options.frames = temp.dir / "frames";
  options.frames_seed = 2;
  const auto combos = run(text(3, 2,
                               "[camera]\nframes_per_step = 2\n"
                               "[sweep.pattern]\naggressiveness = [0.5, 1]\n"),
                          options);
  ASSERT_EQ(combos.size(), 2U);

  int looks = 0;
  std::istringstream trace(read_file(options.trace));
  std::string line;
  std::getline(trace, line);
  while (std::getline(trace, line)) {
    const auto fields = split(line, ',');
    ASSERT_EQ(fields.size(), 11U) << line;
    if (fields[0] == "0" && fields[1] == "2" && !fields[9].empty()) ++looks;
  }
  ASSERT_GT(looks, 0);

  int frames = 0;
  for (const auto& entry : fs::directory_iterator(options.frames)) {
    ++frames;
    EXPECT_EQ(entry.path().extension(), ".png");
    EXPECT_EQ(entry.path().stem().string().size(), 5U);
    const std::string bytes = read_file(entry.path());
    EXPECT_EQ(bytes.substr(0, 8), std::string("\x89PNG\r\n\x1a\n", 8));
  }
  EXPECT_EQ(frames, looks * 2);
  EXPECT_TRUE(fs::exists(options.frames / "00000.png"));
}

TEST(TrackBench, FramesOfASeedThatIsNotRunAreRefused) {
  const TempDir temp;
  TrackOptions options;
  options.frames = temp.dir / "frames";
  options.frames_seed = 99;
  const auto done = run_track(scenario(text(3600, 2)), options);
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().kind, ErrorKind::Config);
}

TEST(TrackBench, ASeedThatIsNotRunIsRefusedWithoutFramesToo) {
  TrackOptions options;
  options.frames_seed = 99;
  options.check_only = true;
  const auto done = run_track(scenario(text(3, 2)), options);
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().kind, ErrorKind::Config);
}

TEST(TrackBench, TraceHasARowPerPoll) {
  const TempDir temp;
  TrackOptions options;
  options.trace = temp.dir / "trace.csv";
  (void)run(text(5, 2), options);
  std::istringstream trace(read_file(options.trace));
  std::string line;
  ASSERT_TRUE(std::getline(trace, line));
  EXPECT_EQ(line, "combo,seed,t_s,beam_x,beam_y,grain_x,grain_y,heat,brightness,reason,saturation");
  int rows = 0, with_look = 0;
  while (std::getline(trace, line)) {
    ++rows;
    const auto fields = split(line, ',');
    ASSERT_EQ(fields.size(), 11U) << line;
    EXPECT_EQ(fields[9].empty(), fields[10].empty()) << line;
    with_look += fields[9].empty() ? 0 : 1;
  }
  EXPECT_NEAR(rows, 100, 5);
  EXPECT_GT(with_look, 0);
  EXPECT_LT(with_look, rows);  // the stage settles after a move before the next look
}

TEST(TrackBench, TableIsBestFirst) {
  const auto combos = run(text(8, 2,
                               "[grain]\ncreep_mm_per_s = [-0.1, 0]\n"
                               "[sweep.pattern]\naggressiveness = [0, 0.2, 1]\n"));
  ASSERT_EQ(combos.size(), 3U);
  for (std::size_t i = 1; i < combos.size(); ++i) {
    EXPECT_GE(combos[i - 1].summary.mean.on_pct, combos[i].summary.mean.on_pct);
  }
  const std::string table = track_table(combos);
  std::istringstream lines(table);
  std::string header;
  ASSERT_TRUE(std::getline(lines, header));
  for (const char* column : {"on%", "rms", "max", "lock_s", "lost", "back_s", "moves", "heat%", "track%", "hold%",
                             "sat%", "search%"}) {
    EXPECT_NE(header.find(column), std::string::npos) << column;
  }
  std::size_t at = 0;
  for (const auto& combo : combos) {
    const auto found = table.find(combo.label, at);
    ASSERT_NE(found, std::string::npos) << combo.label;
    at = found;
  }
  EXPECT_NE(table.find("mean"), std::string::npos);
  EXPECT_NE(table.find("worst"), std::string::npos);
}

TEST(TrackBench, TableSaysWhatFailed) {
  TrackCombo combo;
  combo.label = "pattern.velocity=2";
  combo.failed = 3;
  combo.first_error = "the stage stopped";
  const std::string table = track_table({combo});
  EXPECT_NE(table.find("failed 3"), std::string::npos) << table;
  EXPECT_NE(table.find("the stage stopped"), std::string::npos) << table;
}

TEST(TrackBench, CheckOnlyRunsNothing) {
  const TempDir temp;
  const auto began = std::chrono::steady_clock::now();
  TrackOptions options;
  options.check_only = true;
  options.trace = temp.dir / "trace.csv";
  const auto done = run_track(scenario(text(3600, 10000)), options);
  ASSERT_TRUE(done) << done.error().what;
  EXPECT_TRUE(done->empty());
  EXPECT_FALSE(fs::exists(options.trace));  // looked at, and left as it was
  EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(5));

  expect_refused(text(3600, 10000, "[pattern]\naggressiveness = -1\n"), "aggressiveness");
}

TEST(TrackBench, CheckOnlyLeavesATraceThatIsThere) {
  const TempDir temp;
  TrackOptions options;
  options.check_only = true;
  options.trace = temp.dir / "trace.csv";
  std::ofstream(options.trace) << "kept\n";
  ASSERT_TRUE(run_track(scenario(text(1, 1)), options));
  EXPECT_EQ(read_file(options.trace), "kept\n");
}

TEST(TrackBench, LoadReadsAFile) {
  const TempDir temp;
  const fs::path file = temp.dir / "scenario.toml";
  std::ofstream(file) << text(1, 1);
  EXPECT_TRUE(load_track_scenario(file));
  const auto missing = load_track_scenario(temp.dir / "absent.toml");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorKind::Io);
}

namespace {
// A shipped scenario, a single seed.
std::vector<TrackCombo> run_shipped(const char* name) {
  const fs::path file = fs::path(PYCHRON_TEST_DATA_DIR) / ".." / ".." / "configs" / "examples" / "track" / name;
  auto loaded = load_track_scenario(file);
  EXPECT_TRUE(loaded) << (loaded ? "" : loaded.error().what);
  if (!loaded) return {};
  TrackOptions options;
  options.seeds = 1;
  auto done = run_track(*loaded, options);
  EXPECT_TRUE(done) << (done ? "" : done.error().what);
  return done ? *done : std::vector<TrackCombo>{};
}
}  // namespace

TEST(TrackBench, ABigGrainIsHeldOn) {
  const auto combos = run_shipped("big_grain.toml");
  ASSERT_FALSE(combos.empty());
  EXPECT_EQ(combos.front().failed, 0) << combos.front().first_error;
  EXPECT_GE(combos.front().summary.mean.on_pct, 95.0);
}

TEST(TrackBench, AMeltingGrainIsHeldOn) {
  const auto combos = run_shipped("melting.toml");
  ASSERT_FALSE(combos.empty());
  EXPECT_EQ(combos.front().failed, 0) << combos.front().first_error;
  EXPECT_GE(combos.front().summary.mean.on_pct, 90.0);
}
