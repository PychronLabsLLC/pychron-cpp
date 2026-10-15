// <lab>/executor.toml: the limits of link recovery (executor hardening spec,
// section 4.10).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "pychron/experiment/lab/executor_config.hpp"
#include "temp_path.hpp"

using namespace pychron;
using namespace pychron::experiment::lab;
using namespace std::chrono_literals;

namespace {

ExecutorConfig parsed(const std::string& text) {
  auto c = ExecutorConfig::from_toml(text, "executor.toml");
  EXPECT_TRUE(c) << (c ? "" : c.error().what);
  return c ? *c : ExecutorConfig{};
}

std::string error_of(const std::string& text) {
  auto c = ExecutorConfig::from_toml(text, "executor.toml");
  EXPECT_FALSE(c);
  return c ? "" : c.error().what;
}

}  // namespace

TEST(ExecutorConfig, EveryKeyLoads) {
  const auto c = parsed(R"(
[links]
call_window_s = 30
critical_window_s = 900
backoff_min_s = 0.5
backoff_max_s = 10
[hold]
settle_s = 45
probe_interval_s = 5
limit_s = 3600
after_link_failure = "end"
[measurement]
max_gap_s = 60
min_counts_fraction = 0.5
[spool]
min_free_mb = 1000
max_pending = 50
)");
  EXPECT_EQ(c.links.call_window_s, 30);
  EXPECT_EQ(c.links.critical_window_s, 900);
  EXPECT_EQ(c.links.backoff_min_s, 0.5);
  EXPECT_EQ(c.links.backoff_max_s, 10);
  EXPECT_EQ(c.hold.settle_s, 45);
  EXPECT_EQ(c.hold.probe_interval_s, 5);
  EXPECT_EQ(c.hold.limit_s, 3600);
  EXPECT_EQ(c.hold.after_link_failure, AfterLinkFailure::End);
  EXPECT_EQ(c.measurement.max_gap_s, 60);
  EXPECT_EQ(c.measurement.min_counts_fraction, 0.5);
  EXPECT_EQ(c.spool.min_free_mb, 1000);
  EXPECT_EQ(c.spool.max_pending, 50);
  const LinkPolicy p = c.link_defaults();
  EXPECT_EQ(p.call_window, 30s);
  EXPECT_EQ(p.backoff_min, 500ms);
  EXPECT_EQ(p.backoff_max, 10s);
  EXPECT_EQ(c.critical_window(), 900s);
  EXPECT_EQ(c.max_gap(), 60s);
}

TEST(ExecutorConfig, TheDefaultsAreTheSpecs) {
  const ExecutorConfig c;
  EXPECT_EQ(c.link_defaults(), LinkPolicy{});
  EXPECT_EQ(c.critical_window(), 10min);
  EXPECT_EQ(c.max_gap(), 2min);
  EXPECT_EQ(c.hold.settle_s, 30);
  EXPECT_EQ(c.hold.probe_interval_s, 15);
  EXPECT_EQ(c.hold.limit_s, 7200);
  EXPECT_EQ(c.hold.after_link_failure, AfterLinkFailure::HoldThenContinue);
  EXPECT_EQ(c.measurement.min_counts_fraction, 0.2);
  EXPECT_EQ(c.spool.min_free_mb, 500);
  EXPECT_EQ(c.spool.max_pending, 200);
}

TEST(ExecutorConfig, EachKeyIsRangeChecked) {
  const auto e = error_of("[links]\ncall_window_s = 4000\n[measurement]\nmin_counts_fraction = 1.5\n"
                          "[hold]\nprobe_interval_s = 0\n[spool]\nmax_pending = -1\n");
  for (const char* expected : {"executor.toml:2:links.call_window_s: value 4000 out of range, must be in [0, 3600]",
                               "executor.toml:4:measurement.min_counts_fraction: value 1.5 out of range, must be in [0, 1]",
                               "executor.toml:6:hold.probe_interval_s: value 0 out of range, must be in (0, 3600]",
                               "executor.toml:8:spool.max_pending: value -1 out of range, must be in [0, 1e+06]"})
    EXPECT_NE(e.find(expected), std::string::npos) << expected << "\n in: " << e;
  EXPECT_NE(error_of("[links]\nbackoff_min_s = 10\nbackoff_max_s = 5\n").find("backoff_max_s"), std::string::npos);
  EXPECT_NE(error_of("[spool]\nmax_pending = 2.5\n").find("whole number"), std::string::npos);
  EXPECT_NE(error_of("[measurement]\nmax_gap_s = \"two minutes\"\n").find("measurement.max_gap_s: expected a number"),
            std::string::npos);
}

TEST(ExecutorConfig, AnUnknownKeyOrTableIsAnError) {
  EXPECT_NE(error_of("[links]\ncall_window = 20\n").find("links.call_window: unknown key"), std::string::npos);
  EXPECT_NE(error_of("[holding]\nsettle_s = 3\n").find("holding: unknown table"), std::string::npos);
  EXPECT_NE(error_of("links = 3\n").find("links: expected a table"), std::string::npos);
}

TEST(ExecutorConfig, AfterLinkFailureTakesOnlyItsTwoValues) {
  EXPECT_EQ(parsed("[hold]\nafter_link_failure = \"hold_then_continue\"\n").hold.after_link_failure,
            AfterLinkFailure::HoldThenContinue);
  EXPECT_EQ(parsed("[hold]\nafter_link_failure = \"end\"\n").hold.after_link_failure, AfterLinkFailure::End);
  EXPECT_NE(error_of("[hold]\nafter_link_failure = \"continue\"\n").find("'hold_then_continue' or 'end'"),
            std::string::npos);
  EXPECT_NE(error_of("[hold]\nafter_link_failure = 1\n").find("hold.after_link_failure"), std::string::npos);
}

TEST(ExecutorConfig, AMissingFileIsTheDefaults) {
  const auto dir = pychron::testing::unique_temp_path("executor_config_");
  auto c = ExecutorConfig::load(dir / "executor.toml");
  ASSERT_TRUE(c) << c.error().what;
  EXPECT_EQ(c->link_defaults(), LinkPolicy{});
  EXPECT_EQ(c->hold.limit_s, 7200);
}

// configs/examples/executor.toml lists every key with its default, commented
// out: uncommented, it says the defaults, and as shipped it sets nothing.
TEST(ExecutorConfig, TheExampleFileIsTheDefaults) {
  const auto example = std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "executor.toml";
  auto shipped = ExecutorConfig::load(example);
  ASSERT_TRUE(shipped) << shipped.error().what;
  std::ifstream in(example);
  ASSERT_TRUE(in);
  const std::regex commented_out(R"(^# (\[[a-z_]+\]\s*$|[a-z_]+ = \S))");
  std::ostringstream uncommented;
  std::set<std::string> keys;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line[0] != '#') ADD_FAILURE() << "set as shipped: " << line;
    if (!std::regex_search(line, commented_out)) continue;
    line.erase(0, 2);
    if (line[0] != '[') keys.insert(line.substr(0, line.find(' ')));
    uncommented << line << "\n";
  }
  EXPECT_EQ(keys, (std::set<std::string>{"call_window_s", "critical_window_s", "backoff_min_s", "backoff_max_s",
                                         "settle_s", "probe_interval_s", "limit_s", "after_link_failure", "max_gap_s",
                                         "min_counts_fraction", "min_free_mb", "max_pending"}));
  const auto all = parsed(uncommented.str());
  const ExecutorConfig defaults;
  EXPECT_EQ(all.link_defaults(), defaults.link_defaults());
  EXPECT_EQ(all.links.critical_window_s, defaults.links.critical_window_s);
  EXPECT_EQ(all.hold.settle_s, defaults.hold.settle_s);
  EXPECT_EQ(all.hold.probe_interval_s, defaults.hold.probe_interval_s);
  EXPECT_EQ(all.hold.limit_s, defaults.hold.limit_s);
  EXPECT_EQ(all.hold.after_link_failure, defaults.hold.after_link_failure);
  EXPECT_EQ(all.measurement.max_gap_s, defaults.measurement.max_gap_s);
  EXPECT_EQ(all.measurement.min_counts_fraction, defaults.measurement.min_counts_fraction);
  EXPECT_EQ(all.spool.min_free_mb, defaults.spool.min_free_mb);
  EXPECT_EQ(all.spool.max_pending, defaults.spool.max_pending);
}
