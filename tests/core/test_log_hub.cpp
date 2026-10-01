#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "pychron/core/config/logging_config.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/signal_bus.hpp"

using namespace pychron;
namespace fs = std::filesystem;

namespace {

// Fresh, unique temporary directory removed at scope exit.
class TempDir {
 public:
  TempDir() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    path_ = fs::temp_directory_path() / ("pychron_log_hub_" + std::to_string(gen()));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
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

  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].level, LogLevel::Error);
  EXPECT_NE(got[0].message.find(bad.string()), std::string::npos) << got[0].message;
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
  EXPECT_TRUE(got.empty());
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
