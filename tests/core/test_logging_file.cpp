// logging.toml and logging.local.toml in the install folder, over the line's
// legacy [logging] table.
#include "pychron/core/config/logging_file.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace pychron;
using namespace pychron::config;
namespace fs = std::filesystem;

namespace {

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  ::setenv(name, value.c_str(), 1);
#endif
}

struct LoggingFile : ::testing::Test {
  fs::path dir = fs::temp_directory_path() / ("pychron_logging_file_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
  fs::path line = dir / "extraction_line.toml";
  fs::path line_local = dir / "extraction_line.local.toml";

  void SetUp() override {
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  static void write(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
  }
  void write_line(const std::string& logging) { write(line, "[system]\nname = \"jan\"\n" + logging); }
};

}  // namespace

TEST_F(LoggingFile, NoFilesGivesDefaults) {
  const auto r = load_logging_config(std::nullopt, std::nullopt);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->config.dir, std::nullopt);
  EXPECT_EQ(r->config.default_level, LogLevel::Debug);
  EXPECT_TRUE(r->notes.empty());
  EXPECT_TRUE(r->files.empty());
}

TEST_F(LoggingFile, PrecedenceLowestFirst) {
  write_line("[logging]\nmax_files = 3\n");
  write(line_local, "[logging]\nmax_files = 4\n");
  write(logging_file(dir), "max_files = 5\n");
  auto r = load_logging_config(dir, line);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->config.max_files, 5);

  write(logging_local_file(dir), "max_files = 6\n");
  r = load_logging_config(dir, line);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->config.max_files, 6);
  const std::vector<fs::path> want{line, line_local, logging_file(dir), logging_local_file(dir)};
  EXPECT_EQ(r->files, want);
}

TEST_F(LoggingFile, LegacyDirBeatsDefaultFolder) {
  const auto old = (dir / "old").generic_string();
  write_line("[logging]\ndir = \"" + old + "\"\n");
  const auto r = load_logging_config(dir, line);
  ASSERT_TRUE(r) << r.error().what;
  ASSERT_TRUE(r->config.dir.has_value());
  EXPECT_EQ(r->config.dir->generic_string(), old);
  ASSERT_EQ(r->notes.size(), 1U);
  EXPECT_EQ(r->notes[0].first, LogLevel::Info);
  EXPECT_NE(r->notes[0].second.find("logging.toml"), std::string::npos);
}

TEST_F(LoggingFile, LegacyOverriddenIsWarned) {
  write_line("[logging]\nmax_files = 3\n");
  write(logging_file(dir), "max_files = 5\n");
  const auto r = load_logging_config(dir, line);
  ASSERT_TRUE(r) << r.error().what;
  ASSERT_EQ(r->notes.size(), 1U);
  EXPECT_EQ(r->notes[0].first, LogLevel::Warn);
  EXPECT_NE(r->notes[0].second.find(line.generic_string()), std::string::npos);
}

TEST_F(LoggingFile, LineConfigWithoutLoggingTableGivesNoNote) {
  write_line("");
  const auto r = load_logging_config(dir, line);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_TRUE(r->notes.empty());
}

TEST_F(LoggingFile, BrokenLineConfigIsSkippedWithAWarning) {
  write(line, "this is not toml [");
  write(logging_file(dir), "max_files = 5\n");
  const auto r = load_logging_config(dir, line);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->config.max_files, 5);
  ASSERT_EQ(r->notes.size(), 1U);
  EXPECT_EQ(r->notes[0].first, LogLevel::Warn);
  EXPECT_NE(r->notes[0].second.find(line.generic_string()), std::string::npos);
}

TEST_F(LoggingFile, HomeIsExpandedInLoggingToml) {
  const char* saved = std::getenv("HOME");
  const bool had = saved != nullptr;
  const std::string old = had ? saved : "";
  const auto home = (dir / "home").generic_string();
  set_env("HOME", home);
  write(logging_file(dir), "dir = \"~/logs\"\n");
  const auto r = load_logging_config(dir, std::nullopt);
  if (had) set_env("HOME", old);
  ASSERT_TRUE(r) << r.error().what;
  ASSERT_TRUE(r->config.dir.has_value());
  EXPECT_EQ(r->config.dir->generic_string(), home + "/logs");
}

TEST_F(LoggingFile, UnknownKeyInLoggingTomlFails) {
  write(logging_file(dir), "colour = 1\n");
  const auto r = load_logging_config(dir, std::nullopt);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("logging.toml"), std::string::npos) << r.error().what;
  EXPECT_NE(r.error().what.find("colour"), std::string::npos) << r.error().what;
}

TEST_F(LoggingFile, LevelsTableReplacesWholeList) {
  write_line("[logging]\nlevels = {\"a\" = \"trace\"}\n");
  write(logging_file(dir), "[levels]\n\"b\" = \"warn\"\n");
  const auto r = load_logging_config(dir, line);
  ASSERT_TRUE(r) << r.error().what;
  const std::vector<std::pair<std::string, LogLevel>> want{{"b", LogLevel::Warn}};
  EXPECT_EQ(r->config.levels, want);
}
