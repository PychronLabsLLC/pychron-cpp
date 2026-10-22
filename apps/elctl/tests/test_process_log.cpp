#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include "elctl_fixture.hpp"
#include "pychron/core/clock.hpp"
#include "line.hpp"
#include "pychron/core/config/loader.hpp"
#include "pychron/core/log_dir.hpp"
#include "pychron/core/process_log.hpp"
#include "pychron/setup/site.hpp"
#include "process_log.hpp"

namespace elctl::testing {
namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& p) {
  std::ifstream f(p);
  return {std::istreambuf_iterator<char>(f), {}};
}

class ProcessLog : public ElctlTest {
 protected:
  // A site config naming one install "lab" rooted in the scratch folder.
  fs::path make_install() {
    const fs::path root = path("lab");
    fs::create_directories(root);
    pychron::setup::SiteConfig site;
    pychron::setup::SiteInstall install;
    install.name = "lab";
    install.kind = "instrument";
    install.profile = "argus";
    install.root = root;
    install.line = "extraction_line.toml";
    site.default_install = "lab";
    site.installs.push_back(install);
    EXPECT_TRUE(pychron::setup::save_site(site, path("site.toml")));
    return root;
  }

  pychron::EnvLookup home_env() const {
    const std::string home = path("home").string();
    return [home](std::string_view name) -> std::optional<std::string> {
      if (name == "HOME") return home;
      return std::nullopt;
    };
  }

  pychron::SteadyClock clock_;
  std::shared_ptr<pychron::LogHub> hub_;

  // A hub writing <scratch>/plogs/elctl.log, as main() would make.
  void config_hub() {
    pychron::config::LoggingConfig lc;
    lc.dir = path("plogs");
    lc.file_name = "elctl.log";
    auto made = pychron::LogHub::create(lc, clock_);
    ASSERT_TRUE(made);
    hub_ = *made;
  }
};

TEST_F(ProcessLog, NoInstallWritesToOsFolder) {
  std::ostringstream err;
  fs::path file;
  {
    auto hub = open_process_log({"version"}, clock_, pychron::HostOs::Mac, home_env(), err);
    ASSERT_NE(hub, nullptr) << err.str();
    file = hub->file_path();
  }
  EXPECT_EQ(file, path("home") / "Library" / "Logs" / "pychron" / "elctl.log");
  ASSERT_TRUE(fs::exists(file));
  EXPECT_TRUE(contains(slurp(file), "app=elctl")) << slurp(file);
}

TEST_F(ProcessLog, InstallWritesToRootLogs) {
  const auto root = make_install();
  std::ostringstream err;
  {
    auto hub = open_process_log({"--install", "lab", "version"}, clock_, pychron::HostOs::Mac, home_env(), err);
    ASSERT_NE(hub, nullptr) << err.str();
  }
  const auto file = root / "logs" / "elctl.log";
  ASSERT_TRUE(fs::exists(file)) << err.str();
  EXPECT_TRUE(contains(slurp(file), "profiles=argus")) << slurp(file);
  EXPECT_FALSE(fs::exists(path("home")));
}

TEST_F(ProcessLog, UnwritableInstallLogsDegrades) {
  make_install();
  write("lab/logs", "a file, not a folder");
  std::ostringstream err;
  auto hub = open_process_log({"--install", "lab", "version"}, clock_, pychron::HostOs::Mac, home_env(), err);
  ASSERT_NE(hub, nullptr);
  EXPECT_TRUE(hub->file_path().empty());
  EXPECT_TRUE(contains(err.str(), "file logging disabled")) << err.str();
  const std::string text = err.str();
  EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 1) << text;
}

TEST_F(ProcessLog, LoggingTomlDirOff) {
  const auto root = make_install();
  write("lab/logging.toml", "dir = \"\"\n");
  std::ostringstream err;
  {
    auto hub = open_process_log({"--install", "lab", "version"}, clock_, pychron::HostOs::Mac, home_env(), err);
    ASSERT_NE(hub, nullptr) << err.str();
    EXPECT_TRUE(hub->file_path().empty());
  }
  EXPECT_FALSE(fs::exists(root / "logs"));
}

TEST_F(ProcessLog, AnUnknownInstallFallsBackToTheOsFolder) {
  std::ostringstream err;
  auto hub = open_process_log({"--install", "nope", "version"}, clock_, pychron::HostOs::Mac, home_env(), err);
  ASSERT_NE(hub, nullptr) << err.str();
  EXPECT_EQ(hub->file_path(), path("home") / "Library" / "Logs" / "pychron" / "elctl.log");
}

TEST_F(ProcessLog, GlobalOptionsAreSeenInAnyOrder) {
  const auto root = make_install();
  std::ostringstream err;
  for (const std::vector<std::string>& args :
       {std::vector<std::string>{"--sim", "--install", "lab", "version"},
        std::vector<std::string>{"-c", config(), "--install", "lab", "version"},
        std::vector<std::string>{"--install", "lab", "--sim", "-c", config(), "version"}}) {
    auto hub = open_process_log(args, clock_, pychron::HostOs::Mac, home_env(), err);
    ASSERT_NE(hub, nullptr) << err.str();
    EXPECT_EQ(hub->file_path(), root / "logs" / "elctl.log");
  }
}

TEST_F(ProcessLog, LineBuildUsesTheProcessHub) {
  config_hub();
  auto cfg = pychron::config::load_system_config(config());
  ASSERT_TRUE(cfg) << cfg.error().what;
  const pychron::ProcessLogScope scope(hub_);
  auto line = Line::build(std::move(*cfg), LineOptions{});
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_EQ(hub_->bus(), &(*line)->bus());
  hub_->logger("probe").warn("a line record");
  hub_->flush();
  EXPECT_TRUE(contains(slurp(path("plogs") / "elctl.log"), "a line record"));
}

}  // namespace
}  // namespace elctl::testing
