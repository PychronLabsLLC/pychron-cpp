// The part of a *.local.toml the application writes (File > Preferences):
// one table replaced, the rest of the file left exactly as it was.
#include "pychron/core/config/local_file.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "pychron/core/config/loader.hpp"

using namespace pychron;
using namespace pychron::config;
namespace fs = std::filesystem;

namespace {

struct LocalFile : ::testing::Test {
  fs::path dir = fs::temp_directory_path() / ("pychron_local_file_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                              "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
  fs::path main_file = dir / "extraction_line.toml";
  fs::path local = dir / "extraction_line.local.toml";

  void SetUp() override {
    fs::remove_all(dir);
    fs::create_directories(dir);
    write(main_file, "[system]\nname = \"jan\"\n[transports.valve_bus]\nkind = \"serial\"\nport = \"/dev/ttyUSB0\"\n");
  }
  void TearDown() override {
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_all, ec);
    fs::remove_all(dir, ec);
  }

  static void write(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
  }
  static std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }
};

LoggingConfig logging(LogLevel level) {
  LoggingConfig l;
  l.default_level = level;
  return l;
}

}  // namespace

// ---- what to write ----------------------------------------------------------

TEST(LocalOverrideText, NothingWhenNothingDiffers) {
  EXPECT_EQ(logging_override_toml(LoggingConfig{}, LoggingConfig{}), "");
  EXPECT_EQ(metrics_override_toml(MetricsConfig{}, MetricsConfig{}), "");
}

TEST(LocalOverrideText, OnlyTheKeysThatDiffer) {
  LoggingConfig main_cfg = logging(LogLevel::Warn);
  main_cfg.max_files = 3;
  LoggingConfig wanted = main_cfg;
  wanted.default_level = LogLevel::Debug;
  wanted.echo_stderr = true;
  EXPECT_EQ(logging_override_toml(wanted, main_cfg), "[logging]\ndefault_level = \"debug\"\necho_stderr = true\n");

  MetricsConfig m;
  m.enabled = true;
  m.port = 9500;
  EXPECT_EQ(metrics_override_toml(m, MetricsConfig{}), "[metrics]\nenabled = true\nport = 9500\n");
}

TEST(LocalOverrideText, LevelsAreWrittenWholeAndInOrder) {
  LoggingConfig wanted;
  wanted.levels = {{"scheduler", LogLevel::Debug}, {"*.wire", LogLevel::Trace}};
  EXPECT_EQ(logging_override_toml(wanted, LoggingConfig{}),
            "[logging.levels]\n\"*.wire\" = \"trace\"\n\"scheduler\" = \"debug\"\n");
  // The same levels in another order are the same levels.
  LoggingConfig main_cfg;
  main_cfg.levels = {{"*.wire", LogLevel::Trace}, {"scheduler", LogLevel::Debug}};
  EXPECT_EQ(logging_override_toml(wanted, main_cfg), "");
}

TEST(LocalOverrideText, ClearingTheSharedLevelsIsAnEmptyTable) {
  LoggingConfig main_cfg;
  main_cfg.levels = {{"scheduler", LogLevel::Debug}};
  EXPECT_EQ(logging_override_toml(LoggingConfig{}, main_cfg), "[logging.levels]\n");
}

TEST(LocalOverrideText, PathsAndPatternsAreQuotedSafely) {
  LoggingConfig wanted;
  wanted.dir = R"(C:\Users\lab "A"\logs)";
  wanted.levels = {{"odd\"name", LogLevel::Error}};
  const std::string text = logging_override_toml(wanted, LoggingConfig{});
  const auto r = load_report_from_string("[system]\nname = \"x\"\n", "main.toml", text, "main.local.toml");
  ASSERT_TRUE(r.ok()) << text;
  EXPECT_EQ(r.config->logging.dir, fs::path("C:\\Users\\lab \"A\"\\logs"));
  ASSERT_EQ(r.config->logging.levels.size(), 1u);
  EXPECT_EQ(r.config->logging.levels[0].first, "odd\"name");
}

TEST(LocalOverrideText, ClearingTheSharedFoldersIsAnEmptyDir) {
  LoggingConfig main_cfg;
  main_cfg.dir = "/var/log/pychron";
  const std::string text = logging_override_toml(LoggingConfig{}, main_cfg);
  EXPECT_EQ(text, "[logging]\ndir = \"\"\n");
  const auto r = load_report_from_string("[system]\nname = \"x\"\n[logging]\ndir = \"/var/log/pychron\"\n", "main.toml", text,
                                         "main.local.toml");
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.config->logging.dir.empty());
}

// ---- writing it -------------------------------------------------------------

TEST_F(LocalFile, CreatesTheFile) {
  ASSERT_TRUE(replace_local_table(local, "metrics", "[metrics]\nenabled = true\n"));
  EXPECT_EQ(read(local), "[metrics]\nenabled = true\n");
  const auto cfg = load_system_config(main_file);
  ASSERT_TRUE(cfg) << cfg.error().what;
  EXPECT_TRUE(cfg->metrics.enabled);
}

TEST_F(LocalFile, LeavesTheRestOfTheFileAsItWas) {
  const std::string before = "# this computer's serial port\n[transports.valve_bus]\nport = \"COM4\"   # the left one\n";
  write(local, before);
  ASSERT_TRUE(replace_local_table(local, "logging", "[logging]\ndefault_level = \"debug\"\n"));
  EXPECT_EQ(read(local), before + "\n[logging]\ndefault_level = \"debug\"\n");
  const auto cfg = load_system_config(main_file);
  ASSERT_TRUE(cfg) << cfg.error().what;
  EXPECT_EQ(cfg->logging.default_level, LogLevel::Debug);
}

TEST_F(LocalFile, ReplacesItsTableAndItsSubtablesWhereverTheyAre) {
  write(local,
        "[logging]\ndefault_level = \"trace\"\n\n[transports.valve_bus]\nport = \"COM4\"\n\n# wire bytes\n[logging.levels]\n"
        "\"*.wire\" = \"trace\"\n\n[metrics]\nenabled = true\n");
  ASSERT_TRUE(replace_local_table(local, "logging", "[logging]\ndefault_level = \"warn\"\n"));
  const std::string after = read(local);
  EXPECT_EQ(after.find("trace"), std::string::npos) << after;
  EXPECT_NE(after.find("[transports.valve_bus]\nport = \"COM4\"\n"), std::string::npos) << after;
  EXPECT_NE(after.find("[metrics]\nenabled = true\n"), std::string::npos) << after;
  EXPECT_NE(after.find("[logging]\ndefault_level = \"warn\"\n"), std::string::npos) << after;
  EXPECT_EQ(after.find("# wire bytes"), std::string::npos) << "a comment on a removed table went with it";
}

TEST_F(LocalFile, ACommentAboveTheNextTableStaysWithIt) {
  write(local, "[metrics]\nport = 9500\n\n# the left port\n[transports.valve_bus]\nport = \"COM4\"\n");
  ASSERT_TRUE(replace_local_table(local, "metrics", ""));
  EXPECT_EQ(read(local), "# the left port\n[transports.valve_bus]\nport = \"COM4\"\n");
}

TEST_F(LocalFile, AnEmptyReplacementRemovesTheTable) {
  write(local, "[transports.valve_bus]\nport = \"COM4\"\n\n[metrics]\nenabled = true\n");
  ASSERT_TRUE(replace_local_table(local, "metrics", ""));
  EXPECT_EQ(read(local), "[transports.valve_bus]\nport = \"COM4\"\n");
}

TEST_F(LocalFile, AFileLeftEmptyIsRemoved) {
  write(local, "[metrics]\nenabled = true\n");
  ASSERT_TRUE(replace_local_table(local, "metrics", ""));
  EXPECT_FALSE(fs::exists(local));
}

TEST_F(LocalFile, RemovingFromAFileThatIsNotThereIsNoError) {
  ASSERT_TRUE(replace_local_table(local, "metrics", ""));
  EXPECT_FALSE(fs::exists(local));
}

TEST_F(LocalFile, ATableWithASimilarNameIsNotTouched) {
  write(local, "[metrics]\nenabled = true\n");
  ASSERT_TRUE(replace_local_table(local, "logging", "[logging]\necho_stderr = true\n"));
  EXPECT_NE(read(local).find("[metrics]\nenabled = true\n"), std::string::npos);
}

TEST_F(LocalFile, WindowsLineEndingsAreUnderstood) {
  write(local, "[transports.valve_bus]\r\nport = \"COM4\"\r\n\r\n[metrics]\r\nenabled = true\r\n");
  ASSERT_TRUE(replace_local_table(local, "metrics", "[metrics]\nenabled = false\n"));
  const auto cfg = load_system_config(main_file);
  ASSERT_TRUE(cfg) << cfg.error().what;
  EXPECT_FALSE(cfg->metrics.enabled);
  EXPECT_NE(read(local).find("[transports.valve_bus]\r\nport = \"COM4\"\r\n"), std::string::npos);
}

TEST_F(LocalFile, AFileItCannotMakeSenseOfIsLeftAlone) {
  const std::string broken = "[transports.valve_bus\nport = = 3\n";
  write(local, broken);
  const auto r = replace_local_table(local, "metrics", "[metrics]\nenabled = true\n");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find(local.filename().string()), std::string::npos) << r.error().what;
  EXPECT_EQ(read(local), broken);
}

TEST_F(LocalFile, AReplacementThatIsNotTomlIsRefusedAndTheFileKept) {
  const std::string before = "[transports.valve_bus]\nport = \"COM4\"\n";
  write(local, before);
  ASSERT_FALSE(replace_local_table(local, "metrics", "[metrics]\nenabled = = true\n"));
  EXPECT_EQ(read(local), before);
}

#ifndef _WIN32
TEST_F(LocalFile, AFolderThatCannotBeWrittenIsAnErrorNamingTheFile) {
  const std::string before = "[transports.valve_bus]\nport = \"COM4\"\n";
  write(local, before);
  fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
  if (std::ofstream(dir / "probe").is_open()) return;  // running as root: nothing is unwritable
  const auto r = replace_local_table(local, "metrics", "[metrics]\nenabled = true\n");
  fs::permissions(dir, fs::perms::owner_all);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_NE(r.error().what.find(local.filename().string()), std::string::npos) << r.error().what;
  EXPECT_EQ(read(local), before);
}

// The setup wizard writes a local file for its owner's eyes only (the
// spectrometer's holds a password); replacing the file must not open it up.
TEST_F(LocalFile, TheFileStaysItsOwnersOnly) {
  write(local, "[transports.valve_bus]\nport = \"COM4\"\n");
  fs::permissions(local, fs::perms::owner_read | fs::perms::owner_write);
  ASSERT_TRUE(replace_local_table(local, "metrics", "[metrics]\nenabled = true\n"));
  EXPECT_EQ(fs::status(local).permissions() & fs::perms::mask, fs::perms::owner_read | fs::perms::owner_write);
}

TEST_F(LocalFile, ANewFileIsItsOwnersOnly) {
  ASSERT_TRUE(replace_local_table(local, "metrics", "[metrics]\nenabled = true\n"));
  EXPECT_EQ(fs::status(local).permissions() & (fs::perms::group_all | fs::perms::others_all), fs::perms::none);
}

TEST_F(LocalFile, AFileOthersCouldReadStaysReadable) {
  write(local, "[transports.valve_bus]\nport = \"COM4\"\n");
  const fs::perms shared = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read;
  fs::permissions(local, shared);
  ASSERT_TRUE(replace_local_table(local, "metrics", "[metrics]\nenabled = true\n"));
  EXPECT_EQ(fs::status(local).permissions() & fs::perms::mask, shared);
}
#endif

TEST_F(LocalFile, NoTemporaryFileIsLeftBehind) {
  ASSERT_TRUE(replace_local_table(local, "metrics", "[metrics]\nenabled = true\n"));
  int files = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    (void)e;
    ++files;
  }
  EXPECT_EQ(files, 2);  // the main file and the local one
}

TEST_F(LocalFile, TheMainFileAloneSaysWhatTheSharedConfigIs) {
  write(main_file, "[system]\nname = \"jan\"\n[logging]\ndefault_level = \"warn\"\n");
  write(local, "[logging]\ndefault_level = \"trace\"\n");
  const auto shared = load_system_config_without_local(main_file);
  ASSERT_TRUE(shared) << shared.error().what;
  EXPECT_EQ(shared->logging.default_level, LogLevel::Warn);
  const auto effective = load_system_config(main_file);
  ASSERT_TRUE(effective);
  EXPECT_EQ(effective->logging.default_level, LogLevel::Trace);
}
