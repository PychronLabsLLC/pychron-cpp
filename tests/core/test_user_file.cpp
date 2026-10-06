// mark_as_user_file: a file the application wrote for the user carries no
// quarantine attribute afterwards. Off macOS there is nothing to clear; the
// call still says so and tells a missing file apart.

#include "pychron/core/user_file.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

TEST(UserFile, AFileWithoutTheAttributeIsFineAndAMissingOneIsNot) {
  const fs::path dir = fs::temp_directory_path() / "pychron_user_file_test";
  fs::create_directories(dir);
  const fs::path file = dir / "report.csv";
  std::ofstream(file) << "# a report\n";
  EXPECT_TRUE(pychron::mark_as_user_file(file));
  EXPECT_TRUE(pychron::mark_as_user_file(file));  // idempotent
  EXPECT_FALSE(pychron::mark_as_user_file(dir / "never-written.csv"));
  // The file is untouched.
  std::string line;
  {
    std::ifstream in(file);  // closed before the directory goes: Windows will not remove an open file
    std::getline(in, line);
  }
  EXPECT_EQ(line, "# a report");
  fs::remove_all(dir);
}
