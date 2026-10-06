#include <gtest/gtest.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "pychron/core/config/logging_config.hpp"
#include "pychron/core/env.hpp"
#include "pychron/core/log_match.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/core/signal_bus.hpp"

using namespace pychron;
namespace fs = std::filesystem;

namespace {

// Fresh, unique temporary directory removed at scope exit.
class TempDir {
 public:
  TempDir() : path_(unique_path()) { fs::create_directories(path_); }
  // For death tests. On Windows the child is a fresh run of the test rather
  // than a fork, so it would pick a different random directory from the one
  // the parent inspects: the parent publishes its directory in an environment
  // variable keyed by `name`, and the child adopts it. The name is never part
  // of the path, so concurrent runs of the same test (another checkout,
  // another ctest) cannot remove each other's directory.
  explicit TempDir(const std::string& name) : env_("PYCHRON_LOG_HUB_TMP_" + name) {
    if (const auto inherited = env_var(env_.c_str()); inherited && !inherited->empty()) {
      path_ = *inherited;
      owner_ = false;
      return;
    }
    path_ = unique_path();
    fs::create_directories(path_);
    set_env(path_.string());
  }
  ~TempDir() {
    if (!owner_) return;
    if (!env_.empty()) set_env("");
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }

 private:
  static fs::path unique_path() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    return fs::temp_directory_path() / ("pychron_log_hub_" + std::to_string(gen()));
  }
  // An empty value removes the variable.
  void set_env(const std::string& value) const {
#ifdef _WIN32
    _putenv_s(env_.c_str(), value.c_str());
#else
    if (value.empty()) {
      ::unsetenv(env_.c_str());
    } else {
      ::setenv(env_.c_str(), value.c_str(), 1);
    }
#endif
  }

  fs::path path_;
  std::string env_;  // empty: not shared with a death-test child
  bool owner_ = true;
};

config::LoggingConfig config_for(const fs::path& dir) {
  config::LoggingConfig cfg;
  cfg.dir = dir;
  return cfg;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> read_lines(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

std::shared_ptr<LogHub> make_hub(const config::LoggingConfig& cfg, const Clock& clock,
                                 SignalBus* bus = nullptr) {
  auto hub = LogHub::create(cfg, clock, bus);
  EXPECT_TRUE(hub.has_value());
  return hub.has_value() ? *hub : nullptr;
}

}  // namespace

TEST(LogHub, WritesFormattedLineToFile) {
  TempDir tmp;
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);

  hub->write(LogLevel::Warn, "transport.x", "hello");
  hub->flush();

  const auto lines = read_lines(tmp.path() / "pychron.log");
  ASSERT_EQ(lines.size(), 1u);
  const std::regex re(R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z \[warn\] transport\.x: hello$)");
  EXPECT_TRUE(std::regex_match(lines[0], re)) << lines[0];
}

TEST(LogHub, LevelNamesInFile) {
  TempDir tmp;
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);

  hub->write(LogLevel::Trace, "a", "t");
  hub->write(LogLevel::Debug, "a", "d");
  hub->write(LogLevel::Info, "a", "i");
  hub->write(LogLevel::Warn, "a", "w");
  hub->write(LogLevel::Error, "a", "e");
  hub->flush();

  const auto lines = read_lines(tmp.path() / "pychron.log");
  ASSERT_EQ(lines.size(), 5u);
  EXPECT_NE(lines[0].find(" [trace] a: t"), std::string::npos) << lines[0];
  EXPECT_NE(lines[1].find(" [debug] a: d"), std::string::npos) << lines[1];
  EXPECT_NE(lines[2].find(" [info] a: i"), std::string::npos) << lines[2];
  EXPECT_NE(lines[3].find(" [warn] a: w"), std::string::npos) << lines[3];
  EXPECT_NE(lines[4].find(" [error] a: e"), std::string::npos) << lines[4];
}

TEST(LogHub, ErrorIsOnDiskWithoutFlush) {
  TempDir tmp;
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);

  hub->write(LogLevel::Error, "core", "disk on fire");

  EXPECT_NE(read_file(tmp.path() / "pychron.log").find("[error] core: disk on fire"),
            std::string::npos);
}

TEST(LogHub, DestructorFlushesPending) {
  TempDir tmp;
  SteadyClock clock;
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    for (int i = 0; i < 1000; ++i) hub->write(LogLevel::Info, "bulk", "line " + std::to_string(i));
  }
  const auto lines = read_lines(tmp.path() / "pychron.log");
  ASSERT_EQ(lines.size(), 1000u);
  EXPECT_NE(lines.back().find("line 999"), std::string::npos);
}

TEST(LogHub, RotatesAtSizeLimit) {
  TempDir tmp;
  SteadyClock clock;
  auto cfg = config_for(tmp.path());
  cfg.max_size_mb = 1;
  cfg.max_files = 3;
  const std::string payload(200, 'x');
  {
    auto hub = make_hub(cfg, clock);
    ASSERT_TRUE(hub);
    // ~5 MB of records.
    for (int i = 0; i < 5 * 1024 * 1024 / 256; ++i) hub->write(LogLevel::Info, "rot", payload);
  }

  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(tmp.path())) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names, (std::vector<std::string>{"pychron.1.log", "pychron.2.log", "pychron.log"}));

  constexpr std::uintmax_t limit = 1024 * 1024 + 512;  // 1 MB + one line
  for (const auto& n : names) EXPECT_LE(fs::file_size(tmp.path() / n), limit) << n;
}

TEST(LogHub, UnwritableDirDegrades) {
  TempDir tmp;
  const fs::path regular = tmp.path() / "not_a_dir";
  { std::ofstream(regular) << "x"; }
  const fs::path bad = regular / "logs";

  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });

  auto hub = LogHub::create(config_for(bad), clock, &bus);
  ASSERT_TRUE(hub.has_value());
  EXPECT_NO_THROW((*hub)->write(LogLevel::Error, "core", "still alive"));
  EXPECT_NO_THROW((*hub)->flush());

  // The start-up report, then the record itself (bus sink).
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].level, LogLevel::Error);
  EXPECT_EQ(got[0].logger, "logging");
  EXPECT_NE(got[0].message.find(bad.string()), std::string::npos) << got[0].message;
  EXPECT_EQ(got[1].message, "still alive");
  EXPECT_FALSE(fs::exists(bad));
}

TEST(LogHub, EmptyDirMeansNoFileSink) {
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });

  auto hub = LogHub::create(config::LoggingConfig{}, clock, &bus);
  ASSERT_TRUE(hub.has_value());
  EXPECT_NO_THROW((*hub)->write(LogLevel::Error, "core", "nowhere"));
  EXPECT_NO_THROW((*hub)->flush());
  // No start-up report; only the record itself reaches the bus.
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].logger, "core");
  EXPECT_EQ(got[0].message, "nowhere");
}

TEST(LogHub, MessagesAreVerbatim) {
  TempDir tmp;
  SteadyClock clock;
  const std::string fmt_like = "100% {} {0}";
  const std::string multiline = "line1\nline2";
  const std::string high_byte = std::string("bad\xff") + "byte";
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    EXPECT_NO_THROW(hub->write(LogLevel::Info, "v", fmt_like));
    EXPECT_NO_THROW(hub->write(LogLevel::Info, "v", multiline));
    EXPECT_NO_THROW(hub->write(LogLevel::Info, "v", high_byte));
  }
  const std::string text = read_file(tmp.path() / "pychron.log");
  EXPECT_NE(text.find("v: " + fmt_like + "\n"), std::string::npos) << text;
  EXPECT_NE(text.find("v: " + multiline + "\n"), std::string::npos) << text;
  EXPECT_NE(text.find("v: " + high_byte + "\n"), std::string::npos) << text;
}

// ---- Task 4: hub-backed Logger -------------------------------------------

namespace {

config::LoggingConfig rules_config(std::vector<std::pair<std::string, LogLevel>> levels,
                                   LogLevel default_level) {
  config::LoggingConfig cfg;
  cfg.default_level = default_level;
  cfg.levels = std::move(levels);
  return cfg;
}

}  // namespace

TEST(LogHub, LevelFromRules) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({{"transport.*", LogLevel::Debug},
                                    {"transport.serial.*", LogLevel::Trace}},
                                   LogLevel::Warn),
                      clock);
  ASSERT_TRUE(hub);
  EXPECT_EQ(hub->logger("transport.serial.ig1").level(), LogLevel::Trace);
  EXPECT_EQ(hub->logger("transport.tcp").level(), LogLevel::Debug);
  EXPECT_EQ(hub->logger("core").level(), LogLevel::Warn);
  EXPECT_EQ(hub->logger("transport").level(), LogLevel::Warn);  // glob needs a suffix
}

TEST(LogHub, BareNameRuleMatchesChildren) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({{"scheduler", LogLevel::Debug}}, LogLevel::Info), clock);
  ASSERT_TRUE(hub);
  EXPECT_EQ(hub->logger("scheduler").level(), LogLevel::Debug);
  EXPECT_EQ(hub->logger("scheduler.jobs").level(), LogLevel::Debug);
  EXPECT_EQ(hub->logger("schedulerx").level(), LogLevel::Info);
}

TEST(LogHub, RuleOrderIndependent) {
  SteadyClock clock;
  const std::pair<std::string, LogLevel> a{"transport.*", LogLevel::Debug};
  const std::pair<std::string, LogLevel> b{"transport.serial.*", LogLevel::Error};
  auto h1 = make_hub(rules_config({a, b}, LogLevel::Info), clock);
  auto h2 = make_hub(rules_config({b, a}, LogLevel::Info), clock);
  ASSERT_TRUE(h1 && h2);
  for (const char* name : {"transport.serial.ig1", "transport.tcp", "core"}) {
    EXPECT_EQ(h1->logger(name).level(), h2->logger(name).level()) << name;
  }
  EXPECT_EQ(h1->logger("transport.serial.ig1").level(), LogLevel::Error);
}

TEST(LogHub, EqualSpecificityTieIsDeterministic) {
  SteadyClock clock;
  // Same specificity (2 literals, one star) and same length: the
  // lexicographically greater pattern ("a*b" > "*ab") wins, in either order.
  const std::pair<std::string, LogLevel> a{"a*b", LogLevel::Trace};
  const std::pair<std::string, LogLevel> b{"*ab", LogLevel::Error};
  ASSERT_EQ(log_rule_specificity(a.first), log_rule_specificity(b.first));
  auto h1 = make_hub(rules_config({a, b}, LogLevel::Info), clock);
  auto h2 = make_hub(rules_config({b, a}, LogLevel::Info), clock);
  ASSERT_TRUE(h1 && h2);
  EXPECT_EQ(h1->logger("aab").level(), LogLevel::Trace);
  EXPECT_EQ(h2->logger("aab").level(), LogLevel::Trace);
}

TEST(LogHub, LongerPatternBreaksSpecificityTie) {
  SteadyClock clock;
  // Both have 2 literals and a star; "a**b" is longer than "a*b".
  const std::pair<std::string, LogLevel> a{"a*b", LogLevel::Error};
  const std::pair<std::string, LogLevel> b{"a**b", LogLevel::Trace};
  ASSERT_EQ(log_rule_specificity(a.first), log_rule_specificity(b.first));
  auto h1 = make_hub(rules_config({a, b}, LogLevel::Info), clock);
  auto h2 = make_hub(rules_config({b, a}, LogLevel::Info), clock);
  ASSERT_TRUE(h1 && h2);
  EXPECT_EQ(h1->logger("axb").level(), LogLevel::Trace);
  EXPECT_EQ(h2->logger("axb").level(), LogLevel::Trace);
}

TEST(LogHub, SetLevelReresolvesLiveLoggers) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({}, LogLevel::Warn), clock);
  ASSERT_TRUE(hub);
  Logger log = hub->logger("core");
  Logger copy = log;
  EXPECT_FALSE(log.enabled(LogLevel::Trace));
  EXPECT_FALSE(copy.enabled(LogLevel::Trace));

  hub->set_level("core", LogLevel::Trace);
  EXPECT_TRUE(log.enabled(LogLevel::Trace));
  EXPECT_TRUE(copy.enabled(LogLevel::Trace));
  EXPECT_EQ(log.level(), LogLevel::Trace);
  EXPECT_TRUE(hub->logger("core").enabled(LogLevel::Trace));
  EXPECT_TRUE(hub->logger("core.sub").enabled(LogLevel::Trace));
  EXPECT_FALSE(hub->logger("other").enabled(LogLevel::Trace));
}

TEST(LogHub, ChildResolvesItsOwnLevel) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({{"systems.gauges", LogLevel::Trace}}, LogLevel::Warn), clock);
  ASSERT_TRUE(hub);
  Logger parent = hub->logger("systems");
  Logger child = parent.child("gauges");
  EXPECT_EQ(child.name(), "systems.gauges");
  EXPECT_EQ(parent.level(), LogLevel::Warn);
  EXPECT_EQ(child.level(), LogLevel::Trace);
}

TEST(LogHub, ExplicitLoggerLevelSticks) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({}, LogLevel::Warn), clock);
  ASSERT_TRUE(hub);
  Logger log = hub->logger("core");
  log.set_level(LogLevel::Error);
  hub->set_level("core", LogLevel::Trace);
  EXPECT_EQ(log.level(), LogLevel::Error);
  EXPECT_FALSE(log.enabled(LogLevel::Warn));
}

TEST(LogHub, PublishesBusEventsWithClockTs) {
  using namespace std::chrono_literals;
  ManualClock clock;
  clock.advance(5s);
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto hub = make_hub(rules_config({}, LogLevel::Info), clock, &bus);
  ASSERT_TRUE(hub);

  Logger log = hub->logger("switches");
  log.debug("hidden");
  log.info("valve A opened");
  log.error("valve B failed");

  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].level, LogLevel::Info);
  EXPECT_EQ(got[0].logger, "switches");
  EXPECT_EQ(got[0].message, "valve A opened");
  EXPECT_EQ(got[0].ts, clock.now());
  EXPECT_EQ(got[1].level, LogLevel::Error);
  EXPECT_EQ(got[1].message, "valve B failed");
}

TEST(LogHub, LoggerWritesToFile) {
  TempDir tmp;
  SteadyClock clock;
  auto cfg = config_for(tmp.path());
  cfg.default_level = LogLevel::Info;
  auto hub = make_hub(cfg, clock);
  ASSERT_TRUE(hub);
  Logger log = hub->logger("core");
  log.debug("hidden");
  log.warn("shown");
  hub->flush();
  const auto lines = read_lines(tmp.path() / "pychron.log");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find(" [warn] core: shown"), std::string::npos) << lines[0];
}

TEST(LogHub, EchoesToStderr) {
  SteadyClock clock;
  auto cfg = rules_config({}, LogLevel::Info);
  cfg.echo_stderr = true;
  auto hub = make_hub(cfg, clock);
  ASSERT_TRUE(hub);
  testing::internal::CaptureStderr();
  hub->logger("core").warn("careful");
  hub->logger("core").debug("hidden");
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_EQ(err, "[warn] core: careful\n");
}

TEST(LogHub, NoStderrEchoByDefault) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({}, LogLevel::Info), clock);
  ASSERT_TRUE(hub);
  testing::internal::CaptureStderr();
  hub->logger("core").warn("careful");
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
}

TEST(LogHub, SubscriberMayLogBack) {
  // Bus publishing runs on the caller thread, so a subscriber that logs an
  // error (which waits for the writer) cannot deadlock against the writer.
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(config_for(tmp.path()), clock, &bus);
  ASSERT_TRUE(hub);
  Logger relay = hub->logger("relay");
  int seen = 0;
  auto sub = bus.subscribe<Log>([&](const Log& e) {
    ++seen;
    if (e.logger != "relay") relay.error("saw " + e.message);
  });
  hub->logger("core").error("boom");
  EXPECT_EQ(seen, 2);
  EXPECT_NE(read_file(tmp.path() / "pychron.log").find("[error] relay: saw boom"),
            std::string::npos);
}

TEST(LogHub, LoggerOutlivesHub) {
  SteadyClock clock;
  SignalBus bus;
  int seen = 0;
  auto sub = bus.subscribe<Log>([&](const Log&) { ++seen; });
  std::optional<Logger> survivor;
  {
    auto hub = make_hub(rules_config({}, LogLevel::Info), clock, &bus);
    ASSERT_TRUE(hub);
    survivor.emplace(hub->logger("core"));
  }
  EXPECT_NO_THROW(survivor->error("x"));
  EXPECT_NO_THROW(survivor->set_level(LogLevel::Trace));
  EXPECT_EQ(survivor->level(), LogLevel::Trace);
  EXPECT_NO_THROW(survivor->trace("y"));
  Logger child = survivor->child("sub");
  EXPECT_NO_THROW(child.error("z"));
  EXPECT_EQ(seen, 0);
}

TEST(LogHub, ConcurrentWriteAndSetLevel) {
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  std::atomic<int> bus_errors{0};
  auto sub = bus.subscribe<Log>([&](const Log& e) {
    if (e.level == LogLevel::Error) bus_errors.fetch_add(1);
  });
  auto hub = make_hub(config_for(tmp.path()), clock, &bus);
  ASSERT_TRUE(hub);

  constexpr int kThreads = 4;
  constexpr int kPerThread = 100;
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      Logger log = hub->logger("core.w" + std::to_string(t));
      while (!go.load()) std::this_thread::yield();
      for (int i = 0; i < kPerThread; ++i) {
        log.debug("d");
        log.error("e " + std::to_string(i));
      }
    });
  }
  threads.emplace_back([&] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 1000; ++i) {
      hub->set_level("core", (i % 2 == 0) ? LogLevel::Trace : LogLevel::Error);
    }
  });
  go.store(true);
  for (auto& th : threads) th.join();
  hub->flush();

  EXPECT_EQ(bus_errors.load(), kThreads * kPerThread);
  int file_errors = 0;
  for (const auto& line : read_lines(tmp.path() / "pychron.log")) {
    if (line.find("[error] core.w") != std::string::npos) ++file_errors;
  }
  EXPECT_EQ(file_errors, kThreads * kPerThread);
}

// --- Crash and terminate handlers (spec 4.5) -------------------------------
//
// Each death test runs a child (a fork in "fast" style; TempDir(name) hands
// it the parent's directory either way) that builds its own hub, crashes, and
// dies; the parent then inspects pychron.log.

namespace {

std::size_t count_lines_containing(const fs::path& p, const std::string& needle) {
  const auto lines = read_lines(p);
  return static_cast<std::size_t>(std::count_if(
      lines.begin(), lines.end(), [&](const std::string& l) { return l.find(needle) != std::string::npos; }));
}

// Not inline-visible to the noexcept caller, so no "will always terminate"
// warning; the throw escapes a noexcept frame and reaches std::terminate with
// the exception active.
[[noreturn]] void throw_boom() { throw std::runtime_error("boom"); }
void (*volatile g_throw_boom)() = &throw_boom;
void call_noexcept_throw() noexcept { g_throw_boom(); }

int g_prev_terminate_calls = 0;
[[noreturn]] void counting_terminate() {
  ++g_prev_terminate_calls;
  std::fprintf(stderr, "prev terminate calls=%d\n", g_prev_terminate_calls);
  std::fflush(stderr);
  std::_Exit(3);
}

}  // namespace

TEST(LogHubCrash, TerminateFlushesQueuedRecordsAndExceptionText) {
  GTEST_FLAG_SET(death_test_style, "fast");
  TempDir tmp("TerminateFlushesQueuedRecordsAndExceptionText");
  SteadyClock clock;
  EXPECT_DEATH(
      {
        auto hub = LogHub::create(config_for(tmp.path()), clock);
        if (!hub) std::_Exit(10);
        LogHub::install_crash_handlers();
        auto log = (*hub)->logger("crash.test");
        for (int i = 0; i < 200; ++i) log.info("record " + std::to_string(i));
        call_noexcept_throw();
      },
      "");
  const auto file = tmp.path() / "pychron.log";
  EXPECT_EQ(count_lines_containing(file, "[info] crash.test: record "), 200u);
  EXPECT_GE(count_lines_containing(file, "boom"), 1u);
}

TEST(LogHubCrash, SigabrtLeavesFatalSignalLine) {
#ifdef _WIN32
  GTEST_SKIP() << "POSIX signal handlers";
#else
  GTEST_FLAG_SET(death_test_style, "fast");
  TempDir tmp("SigabrtLeavesFatalSignalLine");
  SteadyClock clock;
  EXPECT_EXIT(
      {
        // Installed before the hub exists: create() opens the descriptor.
        LogHub::install_crash_handlers();
        auto hub = LogHub::create(config_for(tmp.path()), clock);
        if (!hub) std::_Exit(10);
        (*hub)->logger("crash.test").error("about to abort");
        std::abort();
      },
      ::testing::KilledBySignal(SIGABRT), "fatal signal 6");
  const auto file = tmp.path() / "pychron.log";
  EXPECT_EQ(count_lines_containing(file, "[error] crash.test: about to abort"), 1u);
  EXPECT_EQ(count_lines_containing(file, "fatal signal 6"), 1u);
#endif
}

TEST(LogHubCrash, SignalGuaranteeIsBounded) {
#ifdef _WIN32
  GTEST_SKIP() << "POSIX signal handlers";
#else
  // Documents the 1 s periodic-flush bound only: an info record older than
  // that survives a hard crash. Nothing asserts younger sub-error records do.
  GTEST_FLAG_SET(death_test_style, "fast");
  TempDir tmp("SignalGuaranteeIsBounded");
  SteadyClock clock;
  EXPECT_EXIT(
      {
        auto hub = LogHub::create(config_for(tmp.path()), clock);
        if (!hub) std::_Exit(10);
        LogHub::install_crash_handlers();
        (*hub)->logger("crash.test").info("old enough");
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        std::abort();
      },
      ::testing::KilledBySignal(SIGABRT), "fatal signal 6");
  const auto file = tmp.path() / "pychron.log";
  EXPECT_EQ(count_lines_containing(file, "[info] crash.test: old enough"), 1u);
  EXPECT_EQ(count_lines_containing(file, "fatal signal 6"), 1u);
#endif
}

TEST(LogHubCrash, FatalSignalLineFollowsRotation) {
#ifdef _WIN32
  GTEST_SKIP() << "POSIX signal handlers";
#else
  // The crash descriptor is opened on the pychron.log of the moment; once the
  // file rotates, the next flusher tick must move it onto the new live file.
  GTEST_FLAG_SET(death_test_style, "fast");
  TempDir tmp("FatalSignalLineFollowsRotation");
  SteadyClock clock;
  EXPECT_EXIT(
      {
        auto cfg = config_for(tmp.path());
        cfg.max_size_mb = 1;
        cfg.max_files = 3;
        auto hub = LogHub::create(cfg, clock);
        if (!hub) std::_Exit(10);
        LogHub::install_crash_handlers();
        auto log = (*hub)->logger("crash.test");
        const std::string filler(1000, 'x');
        for (int i = 0; i < 1500; ++i) log.info(filler);  // > 1 MiB: rotates
        (*hub)->flush();
        if (!fs::exists(tmp.path() / "pychron.1.log")) std::_Exit(11);
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // >= 2 flusher ticks
        std::abort();
      },
      ::testing::KilledBySignal(SIGABRT), "fatal signal 6");
  EXPECT_EQ(count_lines_containing(tmp.path() / "pychron.log", "fatal signal 6"), 1u);
  EXPECT_EQ(count_lines_containing(tmp.path() / "pychron.1.log", "fatal signal 6"), 0u);
#endif
}

TEST(LogHubCrash, InstallIsIdempotent) {
  GTEST_FLAG_SET(death_test_style, "fast");
  TempDir tmp("InstallIsIdempotent");
  SteadyClock clock;
  EXPECT_EXIT(
      {
        std::set_terminate(&counting_terminate);
        LogHub::install_crash_handlers();
        LogHub::install_crash_handlers();
        auto hub = LogHub::create(config_for(tmp.path()), clock);
        if (!hub) std::_Exit(10);
        std::terminate();
      },
      ::testing::ExitedWithCode(3), "prev terminate calls=1\n");
  // No active exception: the terminate route still logs a line.
  EXPECT_EQ(count_lines_containing(tmp.path() / "pychron.log", "terminate"), 1u);
}
