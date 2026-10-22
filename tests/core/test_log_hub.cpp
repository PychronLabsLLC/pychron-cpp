#include <gtest/gtest.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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
#include <future>
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
    if (entry.path().extension() == ".lock") continue;  // the hub's lock file
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

// A subscriber of the bus that throws says so in the log: once, then at the
// 10th failure, the 100th...
TEST(LogHub, ABusHandlerThatThrowsIsLoggedAndNotAtEveryFailure) {
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto hub = LogHub::create(config::LoggingConfig{}, clock, &bus);
  ASSERT_TRUE(hub.has_value());

  auto bad = bus.subscribe<ValveChanged>([](const ValveChanged&) { throw std::runtime_error("no such valve"); });
  for (int i = 0; i < 12; ++i) bus.publish(ValveChanged{});
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].level, LogLevel::Error);
  EXPECT_EQ(got[0].logger, "bus");
  EXPECT_EQ(got[0].message, "a handler of pychron::ValveChanged threw: no such valve");
  EXPECT_EQ(got[1].message, "a handler of pychron::ValveChanged threw: no such valve (10 times so far)");

  // The hub is gone: the bus goes on, and nothing is written.
  hub->reset();
  bus.publish(ValveChanged{});
  EXPECT_EQ(got.size(), 2u);
}

// The record of a failure is a Log on the same bus: a Log handler that
// throws does not make the two feed each other.
TEST(LogHub, AThrowingLogHandlerDoesNotLoop) {
  SteadyClock clock;
  SignalBus bus;
  int logs = 0;
  auto sub = bus.subscribe<Log>([&](const Log&) {
    ++logs;
    throw std::runtime_error("log handler");
  });
  auto hub = LogHub::create(config::LoggingConfig{}, clock, &bus);
  ASSERT_TRUE(hub.has_value());
  (*hub)->write(LogLevel::Info, "core", "hello");
  // "hello", then the one record of its handler's first failure.
  EXPECT_EQ(logs, 2);
  EXPECT_EQ(bus.handler_failures(), 2u);
}

TEST(LogHub, FileNameIsUsed) {
  TempDir tmp;
  SteadyClock clock;
  auto cfg = config_for(tmp.path());
  cfg.file_name = "elctl.log";
  auto hub = make_hub(cfg, clock);
  ASSERT_TRUE(hub);
  hub->write(LogLevel::Warn, "core", "x");
  hub->flush();
  EXPECT_TRUE(fs::exists(tmp.path() / "elctl.log"));
  EXPECT_FALSE(fs::exists(tmp.path() / "pychron.log"));
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

// What File > Preferences applies: the whole table at once, so a rule taken
// out of it stops applying.
TEST(LogHub, SetLevelsReplacesEveryRuleAndTheDefault) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({{"core", LogLevel::Trace}, {"scheduler", LogLevel::Debug}}, LogLevel::Warn), clock);
  ASSERT_TRUE(hub);
  Logger core = hub->logger("core");
  Logger scheduler = hub->logger("scheduler");
  Logger other = hub->logger("other");
  EXPECT_TRUE(core.enabled(LogLevel::Trace));
  EXPECT_FALSE(other.enabled(LogLevel::Info));

  hub->set_levels(LogLevel::Info, {{"scheduler", LogLevel::Error}});
  EXPECT_FALSE(core.enabled(LogLevel::Trace)) << "the rule for core was taken out";
  EXPECT_TRUE(core.enabled(LogLevel::Info));
  EXPECT_TRUE(other.enabled(LogLevel::Info)) << "the default changed";
  EXPECT_FALSE(scheduler.enabled(LogLevel::Warn));
  EXPECT_TRUE(scheduler.enabled(LogLevel::Error));
  EXPECT_TRUE(hub->logger("later").enabled(LogLevel::Info));
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

// ---- Attachments, dock level, file lock ------------------------------------

namespace {

// Calendar instant from a UTC date and time, for ManualClock epochs.
WallTime utc(int y, unsigned mo, unsigned d, int h, int mi, int s) {
  using namespace std::chrono;
  return sys_days{year{y} / month{mo} / day{d}} + hours{h} + minutes{mi} + seconds{s};
}

std::string current_pid() {
#ifdef _WIN32
  return std::to_string(_getpid());
#else
  return std::to_string(::getpid());
#endif
}

}  // namespace

TEST(LogHub, DockLevelFiltersBusNotFile) {
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto cfg = config_for(tmp.path());
  cfg.default_level = LogLevel::Debug;
  cfg.dock_level = LogLevel::Info;
  auto hub = make_hub(cfg, clock, &bus);
  ASSERT_TRUE(hub);

  hub->logger("core").debug("quiet");
  hub->logger("core").info("loud");
  hub->flush();

  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].message, "loud");
  const auto text = read_file(tmp.path() / "pychron.log");
  EXPECT_NE(text.find("[debug] core: quiet"), std::string::npos) << text;
  EXPECT_NE(text.find("[info] core: loud"), std::string::npos) << text;
}

TEST(LogHub, AttachBusLater) {
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  std::vector<std::string> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e.message); });
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  EXPECT_EQ(hub->bus(), nullptr);

  hub->write(LogLevel::Info, "core", "a");
  {
    const LogHub::BusAttachment attached = hub->attach_bus(bus);
    EXPECT_EQ(hub->bus(), &bus);
    hub->write(LogLevel::Info, "core", "b");
  }
  EXPECT_EQ(hub->bus(), nullptr);
  hub->write(LogLevel::Info, "core", "c");
  hub->flush();

  EXPECT_EQ(got, (std::vector<std::string>{"b"}));
  const auto lines = read_lines(tmp.path() / "pychron.log");
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_NE(lines[0].find("core: a"), std::string::npos) << lines[0];
  EXPECT_NE(lines[1].find("core: b"), std::string::npos) << lines[1];
  EXPECT_NE(lines[2].find("core: c"), std::string::npos) << lines[2];
}

// A handler of an attached bus that throws is logged, as with a bus given to
// create(); once detached, its failures are not.
TEST(LogHub, AttachedBusHandlerFailuresAreLogged) {
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto bad = bus.subscribe<ValveChanged>([](const ValveChanged&) { throw std::runtime_error("no valve"); });
  auto hub = make_hub(config::LoggingConfig{}, clock);
  ASSERT_TRUE(hub);
  {
    const LogHub::BusAttachment attached = hub->attach_bus(bus);
    bus.publish(ValveChanged{});
  }
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].logger, "bus");
  bus.publish(ValveChanged{});
  EXPECT_EQ(got.size(), 1u);
}

TEST(LogHub, DetachWaitsForInFlightPublish) {
  SteadyClock clock;
  SignalBus bus;
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<bool> first{true};
  auto sub = bus.subscribe<Log>([&](const Log&) {
    if (!first.exchange(false)) return;
    entered.set_value();
    released.wait();
  });
  auto hub = make_hub(rules_config({}, LogLevel::Info), clock);
  ASSERT_TRUE(hub);

  std::optional<LogHub::BusAttachment> attached;
  attached.emplace(hub->attach_bus(bus));
  std::thread writer([&] { hub->write(LogLevel::Info, "core", "held"); });
  const bool publishing =
      entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  EXPECT_TRUE(publishing);

  std::atomic<bool> detached{false};
  std::promise<void> done;
  std::thread detacher([&] {
    attached.reset();
    detached.store(true);
    done.set_value();
  });
  // The publish is still blocked in the handler: the detach must not return.
  for (int i = 0; i < 10000 && !detached.load(); ++i) std::this_thread::yield();
  EXPECT_FALSE(detached.load());

  release.set_value();
  EXPECT_EQ(done.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
  writer.join();
  detacher.join();
  EXPECT_TRUE(detached.load());
  EXPECT_EQ(hub->bus(), nullptr);
}

TEST(LogHub, ClockDetachFallsBackToHubClock) {
  using namespace std::chrono_literals;
  TempDir tmp;
  const ManualClock a(TimePoint{} + 10s, utc(2020, 1, 1, 0, 0, 0));
  const ManualClock b(TimePoint{} + 99s, utc(2026, 10, 22, 14, 3, 22));
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto hub = make_hub(config_for(tmp.path()), a, &bus);
  ASSERT_TRUE(hub);
  {
    const LogHub::ClockAttachment attached = hub->attach_clock(b);
    hub->write(LogLevel::Info, "core", "on b");
  }
  hub->write(LogLevel::Info, "core", "on a");
  hub->flush();

  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].ts, b.now());
  EXPECT_EQ(got[1].ts, a.now());
  const auto lines = read_lines(tmp.path() / "pychron.log");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "2026-10-22T14:03:22.000Z [info] core: on b");
  EXPECT_EQ(lines[1], "2020-01-01T00:00:00.000Z [info] core: on a");
}

TEST(LogHub, SecondProcessGetsPidFile) {
  TempDir tmp;
  SteadyClock clock;
  auto first = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->file_path(), tmp.path() / "pychron.log");

  auto second = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(second);
  const fs::path pid_file = tmp.path() / ("pychron-" + current_pid() + ".log");
  EXPECT_EQ(second->file_path(), pid_file);
  second->flush();

  const auto lines = read_lines(pid_file);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find("[info] logging: "), std::string::npos) << lines[0];
  EXPECT_NE(lines[0].find("in use"), std::string::npos) << lines[0];
  EXPECT_NE(lines[0].find((tmp.path() / "pychron.log").string()), std::string::npos) << lines[0];
}

TEST(LogHub, LockReleasedOnDestruction) {
  TempDir tmp;
  SteadyClock clock;
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    EXPECT_EQ(hub->file_path(), tmp.path() / "pychron.log");
  }
  auto again = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(again);
  EXPECT_EQ(again->file_path(), tmp.path() / "pychron.log");
}

// A lock that cannot be taken for a reason other than another holder (here
// the lock path is a directory) keeps the main name and says so at warn.
TEST(LogHub, UnlockableFileIsAWarning) {
  TempDir tmp;
  fs::create_directories(tmp.path() / "pychron.log.lock");
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  EXPECT_EQ(hub->file_path(), tmp.path() / "pychron.log");
  hub->flush();
  const auto lines = read_lines(tmp.path() / "pychron.log");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find("[warn] logging: cannot lock "), std::string::npos) << lines[0];
}

// Attachments made, replaced and dropped while other threads write: every
// record reaches the file, no bus hears a record after its attachment's
// destructor has returned, and every stamp comes from one of the clocks.
TEST(LogHub, AttachAndDropUnderConcurrentWrites) {
  using namespace std::chrono_literals;
  TempDir tmp;
  SteadyClock base;
  const ManualClock c1(TimePoint{}, utc(2001, 1, 1, 0, 0, 0));
  const ManualClock c2(TimePoint{}, utc(2002, 1, 1, 0, 0, 0));
  auto hub = make_hub(config_for(tmp.path()), base);
  ASSERT_TRUE(hub);
  Logger relay = hub->logger("relay");

  // A bus is "live" from before its attach until its attachment's
  // destructor has returned; an event outside that is a violation.
  struct Watched {
    SignalBus bus;
    std::atomic<bool> live{false};
    std::atomic<int> seen{0};
    std::atomic<int> late{0};
    SignalBus::Subscription sub;
  };
  Watched b1;
  Watched b2;
  for (Watched* w : {&b1, &b2}) {
    w->sub = w->bus.subscribe<Log>([w, &relay](const Log& e) {
      if (!w->live.load()) w->late.fetch_add(1);
      w->seen.fetch_add(1);
      if (e.logger != "relay") relay.info("saw " + e.logger);  // logs back
    });
  }

  constexpr int kWriters = 4;
  constexpr int kPerWriter = 1000;
  constexpr int kMinRounds = 50;
  std::atomic<int> writers_left{kWriters};
  std::atomic<int> bus_rounds{0};
  std::atomic<int> clock_rounds{0};
  std::promise<void> all_done;
  std::vector<std::thread> threads;
  threads.reserve(kWriters + 2);

  for (int t = 0; t < kWriters; ++t) {
    threads.emplace_back([&, t] {
      Logger log = hub->logger("w" + std::to_string(t));
      for (int i = 0; i < kPerWriter; ++i) log.info(std::to_string(i));
      writers_left.fetch_sub(1);
    });
  }
  threads.emplace_back([&] {
    while (writers_left.load() > 0 || bus_rounds.load() < kMinRounds) {
      b1.live.store(true);
      std::optional<LogHub::BusAttachment> a1(hub->attach_bus(b1.bus));
      b2.live.store(true);
      std::optional<LogHub::BusAttachment> a2(hub->attach_bus(b2.bus));  // replaces b1
      a2.reset();
      b2.live.store(false);
      a1.reset();
      b1.live.store(false);
      bus_rounds.fetch_add(1);
    }
  });
  threads.emplace_back([&] {
    while (writers_left.load() > 0 || clock_rounds.load() < kMinRounds) {
      std::optional<LogHub::ClockAttachment> k1(hub->attach_clock(c1));
      std::optional<LogHub::ClockAttachment> k2(hub->attach_clock(c2));
      k1.reset();  // replaced already: changes nothing
      k2.reset();
      clock_rounds.fetch_add(1);
    }
  });
  std::thread joiner([&] {
    for (auto& th : threads) th.join();
    all_done.set_value();
  });
  if (all_done.get_future().wait_for(30s) != std::future_status::ready) {
    ADD_FAILURE() << "attach/drop under writes did not finish in 30 s";
    std::abort();
  }
  joiner.join();
  hub->flush();

  EXPECT_EQ(b1.late.load(), 0);
  EXPECT_EQ(b2.late.load(), 0);
  EXPECT_GT(b1.seen.load() + b2.seen.load(), 0);
  EXPECT_EQ(hub->bus(), nullptr);

  std::vector<int> per_writer(kWriters, 0);
  const std::regex stamp(R"(^(\d{4})-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z \[)");
  const int this_year = [] {
    const auto days = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    return static_cast<int>(std::chrono::year_month_day(days).year());
  }();
  for (const auto& line : read_lines(tmp.path() / "pychron.log")) {
    std::smatch m;
    ASSERT_TRUE(std::regex_search(line, m, stamp)) << line;
    const int year = std::stoi(m[1].str());
    EXPECT_TRUE(year == 2001 || year == 2002 || year == this_year || year == this_year - 1) << line;
    for (int t = 0; t < kWriters; ++t) {
      if (line.find("[info] w" + std::to_string(t) + ": ") != std::string::npos) ++per_writer[t];
    }
  }
  for (int t = 0; t < kWriters; ++t) EXPECT_EQ(per_writer[t], kPerWriter) << "writer " << t;
}

TEST(LogHub, NoFileMeansEmptyFilePath) {
  SteadyClock clock;
  auto hub = make_hub(config::LoggingConfig{}, clock);
  ASSERT_TRUE(hub);
  EXPECT_TRUE(hub->file_path().empty());
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
