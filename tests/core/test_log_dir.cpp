// The default log folder per install and operating system.
#include "pychron/core/log_dir.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>

using namespace pychron;
namespace fs = std::filesystem;

namespace {

EnvLookup env_of(std::map<std::string, std::string> vars) {
  return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    const auto it = vars.find(std::string(name));
    if (it == vars.end()) return std::nullopt;
    return it->second;
  };
}

std::string generic(const fs::path& p) { return p.generic_string(); }

}  // namespace

TEST(LogDir, InstallRootWins) {
  const auto env = env_of({{"HOME", "/h"}, {"LOCALAPPDATA", "C:/L"}});
  for (const HostOs os : {HostOs::Mac, HostOs::Linux, HostOs::Windows}) {
    EXPECT_EQ(generic(default_log_dir(fs::path("/r"), os, env)), "/r/logs");
  }
}

TEST(LogDir, MacFolder) {
  EXPECT_EQ(generic(default_log_dir(std::nullopt, HostOs::Mac, env_of({{"HOME", "/h"}}))),
            "/h/Library/Logs/pychron");
}

TEST(LogDir, LinuxXdg) {
  const auto env = env_of({{"XDG_STATE_HOME", "/s"}, {"HOME", "/h"}});
  EXPECT_EQ(generic(default_log_dir(std::nullopt, HostOs::Linux, env)), "/s/pychron/logs");
}

TEST(LogDir, LinuxNoXdg) {
  EXPECT_EQ(generic(default_log_dir(std::nullopt, HostOs::Linux, env_of({{"HOME", "/h"}}))),
            "/h/.local/state/pychron/logs");
}

TEST(LogDir, LinuxEmptyXdgCountsAsUnset) {
  const auto env = env_of({{"XDG_STATE_HOME", ""}, {"HOME", "/h"}});
  EXPECT_EQ(generic(default_log_dir(std::nullopt, HostOs::Linux, env)), "/h/.local/state/pychron/logs");
}

TEST(LogDir, WindowsLocalAppData) {
  EXPECT_EQ(generic(default_log_dir(std::nullopt, HostOs::Windows, env_of({{"LOCALAPPDATA", "C:/L"}}))),
            "C:/L/pychron/logs");
}

TEST(LogDir, NoHomeGivesEmpty) {
  const auto none = env_of({});
  EXPECT_TRUE(default_log_dir(std::nullopt, HostOs::Mac, none).empty());
  EXPECT_TRUE(default_log_dir(std::nullopt, HostOs::Linux, none).empty());
  EXPECT_TRUE(default_log_dir(std::nullopt, HostOs::Windows, none).empty());
}

TEST(LogDir, ResolveKeepsGivenDir) {
  const auto env = env_of({{"HOME", "/h"}});
  config::LoggingConfig empty;
  empty.dir = fs::path();
  resolve_log_dir(empty, std::nullopt, HostOs::Mac, env);
  ASSERT_TRUE(empty.dir.has_value());
  EXPECT_TRUE(empty.dir->empty());

  config::LoggingConfig given;
  given.dir = fs::path("/x");
  resolve_log_dir(given, std::nullopt, HostOs::Mac, env);
  EXPECT_EQ(generic(*given.dir), "/x");

  config::LoggingConfig unset;
  resolve_log_dir(unset, std::nullopt, HostOs::Mac, env);
  ASSERT_TRUE(unset.dir.has_value());
  EXPECT_EQ(generic(*unset.dir), "/h/Library/Logs/pychron");
}

TEST(LogDir, ProcessEnvReadsTheRealEnvironment) {
  const auto env = process_env();
  EXPECT_FALSE(env("PYCHRON_NO_SUCH_VARIABLE_FOR_TEST").has_value());
}

TEST(LogDir, EnvNameIsTheVariableTheOsFolderNeeds) {
  EXPECT_STREQ(log_dir_env_name(HostOs::Mac), "HOME");
  EXPECT_STREQ(log_dir_env_name(HostOs::Linux), "HOME");
  EXPECT_STREQ(log_dir_env_name(HostOs::Windows), "LOCALAPPDATA");
}

TEST(ExamplesStateDir, EachOsFolder) {
  const auto env = env_of({{"HOME", "/h"}, {"XDG_STATE_HOME", "/s"}, {"LOCALAPPDATA", "C:/L"}});
  EXPECT_EQ(generic(default_examples_state_dir(HostOs::Mac, env)), "/h/Library/Application Support/Pychron/examples");
  EXPECT_EQ(generic(default_examples_state_dir(HostOs::Linux, env)), "/s/pychron/examples");
  EXPECT_EQ(generic(default_examples_state_dir(HostOs::Windows, env)), "C:/L/pychron/examples");
}

TEST(ExamplesStateDir, LinuxEmptyXdgCountsAsUnset) {
  const auto env = env_of({{"XDG_STATE_HOME", ""}, {"HOME", "/h"}});
  EXPECT_EQ(generic(default_examples_state_dir(HostOs::Linux, env)), "/h/.local/state/pychron/examples");
}

TEST(ExamplesStateDir, NoHomeGivesEmpty) {
  const auto none = env_of({{"HOME", ""}});
  EXPECT_TRUE(default_examples_state_dir(HostOs::Mac, none).empty());
  EXPECT_TRUE(default_examples_state_dir(HostOs::Linux, none).empty());
  EXPECT_TRUE(default_examples_state_dir(HostOs::Windows, none).empty());
}
