// logging.toml and logging.local.toml in the install folder, over the line's
// legacy [logging] table.
#include "pychron/core/config/logging_file.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>

#include "pychron/core/env.hpp"

using namespace pychron;
using namespace pychron::config;
namespace fs = std::filesystem;

namespace {

// Sets an environment variable for its life, then puts back what was there
// (unset again if it was unset), however the test ends.
class EnvGuard {
 public:
  EnvGuard(const char* name, const std::string& value) : name_(name), old_(env_var(name)) { set(value.c_str()); }
  ~EnvGuard() { set(old_ ? old_->c_str() : nullptr); }
  EnvGuard(const EnvGuard&) = delete;
  EnvGuard& operator=(const EnvGuard&) = delete;
  EnvGuard(EnvGuard&&) = delete;
  EnvGuard& operator=(EnvGuard&&) = delete;

 private:
  // null: unset.
  void set(const char* value) const {
#ifdef _WIN32
    _putenv_s(name_, value != nullptr ? value : "");
#else
    if (value != nullptr) {
      ::setenv(name_, value, 1);
    } else {
      ::unsetenv(name_);
    }
#endif
  }
  const char* name_;
  std::optional<std::string> old_;
};

// A fresh folder name: two checkouts running ctest at once never share one.
fs::path unique_dir() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  return fs::temp_directory_path() / ("pychron_logging_file_" + std::to_string(gen()));
}

struct LoggingFile : ::testing::Test {
  fs::path dir = unique_dir();
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

TEST_F(LoggingFile, CanLeaveOutTheLocalFile) {
  write_line("[logging]\nmax_files = 3\n");
  write(logging_file(dir), "max_files = 5\n");
  write(logging_local_file(dir), "max_files = 6\n");
  const auto r = load_logging_config(dir, line, false);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->config.max_files, 5);
  const std::vector<fs::path> want{line, logging_file(dir)};
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
  EXPECT_NE(r->notes[0].second.find("overridden by logging.toml in"), std::string::npos) << r->notes[0].second;
}

// The note names the file that does the overriding, not one that is not there.
TEST_F(LoggingFile, LegacyOverriddenByTheLocalFileAloneNamesIt) {
  write_line("[logging]\nmax_files = 3\n");
  write(logging_local_file(dir), "max_files = 5\n");
  const auto r = load_logging_config(dir, line);
  ASSERT_TRUE(r) << r.error().what;
  ASSERT_EQ(r->notes.size(), 1U);
  EXPECT_EQ(r->notes[0].first, LogLevel::Warn);
  EXPECT_NE(r->notes[0].second.find("overridden by logging.local.toml in"), std::string::npos)
      << r->notes[0].second;

  write(logging_file(dir), "max_size_mb = 5\n");
  const auto both = load_logging_config(dir, line);
  ASSERT_TRUE(both) << both.error().what;
  ASSERT_EQ(both->notes.size(), 1U);
  EXPECT_NE(both->notes[0].second.find("overridden by logging.toml and logging.local.toml in"), std::string::npos)
      << both->notes[0].second;
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
  const auto home = (dir / "home").generic_string();
  // expand_home reads USERPROFILE on Windows, HOME elsewhere.
#ifdef _WIN32
  const EnvGuard guard("USERPROFILE", home);
#else
  const EnvGuard guard("HOME", home);
#endif
  write(logging_file(dir), "dir = \"~/logs\"\n");
  const auto r = load_logging_config(dir, std::nullopt);
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
