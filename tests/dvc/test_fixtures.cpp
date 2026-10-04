// The legacy fixtures are read byte for byte (hashes, blob shas, parity to
// 5e-9). A checkout that converts line endings would change them.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// Fix wave F6: .gitattributes turns text conversion off for the fixtures.
TEST(Fixtures, AreExemptFromLineEndingConversion) {
  const std::filesystem::path root = std::filesystem::path(PYCHRON_DVC_FIXTURES_DIR) / ".." / ".." / "..";
  std::ifstream in(root / ".gitattributes", std::ios::binary);
  ASSERT_TRUE(in) << "no .gitattributes at the repository root";
  std::ostringstream text;
  text << in.rdbuf();
  bool found = false;
  std::istringstream lines(text.str());
  for (std::string line; std::getline(lines, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line == "tests/dvc/fixtures/** -text") found = true;
  }
  EXPECT_TRUE(found) << text.str();
}
