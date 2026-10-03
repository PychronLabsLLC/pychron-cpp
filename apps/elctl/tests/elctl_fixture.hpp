#pragma once

// Shared fixture: a scratch copy of configs/examples so tests may write
// override, trace-state and trace files without touching the repo.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cli.hpp"

namespace elctl::testing {

struct Outcome {
  int code = -1;
  std::string out;
  std::string err;
};

// `elctl args...` against string streams.
inline Outcome run_raw(std::vector<std::string> args, const std::string& input = "") {
  std::istringstream in(input);
  std::ostringstream out;
  std::ostringstream err;
  Outcome o;
  o.code = elctl::run(args, Io{in, out, err});
  o.out = out.str();
  o.err = err.str();
  return o;
}

class ElctlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           ("elctl_" + std::string(info->test_suite_name()) + "_" + info->name() + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir_);
    // Never the developer's own installs: a site config of this test's own.
    const std::string site = (dir_ / "site.toml").string();
#ifdef _WIN32
    _putenv_s("PYCHRON_SITE_CONFIG", site.c_str());
#else
    setenv("PYCHRON_SITE_CONFIG", site.c_str(), 1);
#endif
    for (const char* f : {"extraction_line.toml", "canvas.toml"}) {
      std::filesystem::copy_file(std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / f, dir_ / f);
    }
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path path(const std::string& name) const { return dir_ / name; }
  std::string config() const { return path("extraction_line.toml").string(); }

  void write(const std::string& name, const std::string& text) const {
    std::ofstream(path(name)) << text;
  }

  // `elctl -c <scratch config> args...`
  Outcome run(std::vector<std::string> args, const std::string& input = "") const {
    args.insert(args.begin(), {"-c", config()});
    return run_raw(std::move(args), input);
  }

  std::filesystem::path dir_;
};

inline bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace elctl::testing
