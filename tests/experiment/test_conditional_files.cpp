// ConditionalFiles: the files of <lab>/conditionals (editor spec 4.3).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "pychron/experiment/conditionals/library.hpp"
#include "pychron/experiment/lab/lab.hpp"

namespace pychron::experiment {
namespace {

namespace fs = std::filesystem;

class ConditionalFilesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("pychron-condfiles-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root_);
    dir_ = root_ / "conditionals";
  }
  void TearDown() override {
    std::error_code ec;
    fs::permissions(dir_, fs::perms::owner_all, ec);
    fs::remove_all(root_, ec);
  }
  static std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }
  std::vector<std::string> entries() const {
    std::vector<std::string> out;
    for (const auto& e : fs::directory_iterator(dir_)) out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
  }

  fs::path root_, dir_;
};

TEST_F(ConditionalFilesTest, MissingDirectoryListsNothing) {
  ConditionalFiles files(dir_);
  auto l = files.list();
  ASSERT_TRUE(l);
  EXPECT_TRUE(l->empty());
  EXPECT_FALSE(files.exists("system"));
}

TEST_F(ConditionalFilesTest, WriteCreatesDirectoryAndLists) {
  ConditionalFiles files(dir_);
  ASSERT_TRUE(files.write("b", "x = 1\n"));
  ASSERT_TRUE(files.write("system", ""));
  ASSERT_TRUE(files.write("a", ""));
  EXPECT_EQ(*files.list(), (std::vector<std::string>{"system", "a", "b"}));
  EXPECT_TRUE(files.exists("b"));
  EXPECT_EQ(files.path("b"), dir_ / "b.toml");
  EXPECT_EQ(entries(), (std::vector<std::string>{"a.toml", "b.toml", "system.toml"}));  // no temp file left
}

TEST_F(ConditionalFilesTest, ReadBack) {
  ConditionalFiles files(dir_);
  ASSERT_TRUE(files.write("q", "[[truncations]]\ncheck = \"Ar40 > 1\"\n"));
  EXPECT_EQ(*files.read("q"), "[[truncations]]\ncheck = \"Ar40 > 1\"\n");
  ASSERT_TRUE(files.write("q", "short\n"));  // overwrite
  EXPECT_EQ(*files.read("q"), "short\n");
  auto missing = files.read("nope");
  ASSERT_FALSE(missing);
  EXPECT_NE(missing.error().what.find((dir_ / "nope.toml").string()), std::string::npos);
}

TEST_F(ConditionalFilesTest, RemoveDeletes) {
  ConditionalFiles files(dir_);
  ASSERT_TRUE(files.write("q", ""));
  ASSERT_TRUE(files.remove("q"));
  EXPECT_FALSE(files.exists("q"));
  EXPECT_FALSE(files.remove("q"));
}

TEST_F(ConditionalFilesTest, BadNamesRefused) {
  ConditionalFiles files(dir_);
  for (const char* name : {"", "a/b", "..", "../x", ".hidden", "x.toml", "a\\b"}) {
    SCOPED_TRACE(name);
    EXPECT_FALSE(ConditionalFiles::valid_name(name));
    EXPECT_FALSE(files.write(name, "x"));
    EXPECT_FALSE(files.read(name));
    EXPECT_FALSE(files.remove(name));
    EXPECT_FALSE(files.exists(name));
  }
  EXPECT_TRUE(ConditionalFiles::valid_name("queue_ar"));
  EXPECT_FALSE(fs::exists(dir_));           // nothing was created
  EXPECT_FALSE(fs::exists(root_ / "x.toml"));
}

TEST_F(ConditionalFilesTest, IgnoresOtherFiles) {
  ConditionalFiles files(dir_);
  ASSERT_TRUE(files.write("a", ""));
  std::ofstream(dir_ / "notes.txt") << "x";
  std::ofstream(dir_ / "x.toml.tmp") << "x";
  std::ofstream(dir_ / ".hidden.toml") << "x";
  fs::create_directories(dir_ / "sub.toml");
  EXPECT_EQ(*files.list(), (std::vector<std::string>{"a"}));
}

TEST_F(ConditionalFilesTest, FailedWriteLeavesNoPartial) {
#ifdef _WIN32
  GTEST_SKIP() << "directory permissions do not block writes on Windows";
#else
  if (::geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";
  ConditionalFiles files(dir_);
  ASSERT_TRUE(files.write("a", "old\n"));
  fs::permissions(dir_, fs::perms::owner_read | fs::perms::owner_exec);
  auto r = files.write("a", "new\n");
  auto r2 = files.write("b", "new\n");
  fs::permissions(dir_, fs::perms::owner_all);
  EXPECT_FALSE(r);
  EXPECT_FALSE(r2);
  EXPECT_EQ(slurp(dir_ / "a.toml"), "old\n");
  EXPECT_EQ(entries(), (std::vector<std::string>{"a.toml"}));
#endif
}

TEST(ConditionalFilesLab, LabHasThem) {
  const fs::path dir(PYCHRON_EXAMPLE_CONFIGS_DIR);
  auto lab = lab::load_lab({dir, dir / "extraction_line.toml", dir / "spectrometer.sim-integrated.toml"});
  ASSERT_TRUE(lab.condition_files);
  auto l = lab.condition_files->list();
  ASSERT_TRUE(l);
  EXPECT_EQ(*l, (std::vector<std::string>{"system", "default_unknown"}));
}

}  // namespace
}  // namespace pychron::experiment
