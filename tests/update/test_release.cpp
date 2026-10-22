#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>

#include <pychron/update/update.hpp>

namespace pychron::update {
namespace {

std::string fixture() {
  std::ifstream in(std::string(PYCHRON_UPDATE_FIXTURE_DIR) + "/latest.json");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
  const auto at = text.find(from);
  EXPECT_NE(at, std::string::npos) << from;
  if (at != std::string::npos) text.replace(at, from.size(), to);
  return text;
}

TEST(UpdateVersion, AcceptsWithAndWithoutV) {
  const auto a = parse_version("v0.5.0");
  const auto b = parse_version("0.5.0");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(*a, (Version{0, 5, 0}));
  EXPECT_EQ(*b, (Version{0, 5, 0}));
}

TEST(UpdateVersion, ComparesNumerically) {
  EXPECT_GT(*parse_version("v0.10.0"), *parse_version("0.9.3"));
}

TEST(UpdateVersion, RefusesOtherText) {
  for (const char* text : {"1.0.0-rc.1", "1.0.0+build", "1.0", "v", "", "x.y.z", "-1.0.0", "1.0.0 ", "01.0.0x",
                           "99999999999.0.0"}) {
    EXPECT_FALSE(parse_version(text).has_value()) << text;
  }
}

TEST(UpdateVersion, ToString) {
  EXPECT_EQ(to_string(Version{1, 2, 3}), "1.2.3");
}

TEST(UpdateRelease, ReadsTheFixture) {
  const auto release = parse_latest_release(fixture());
  ASSERT_TRUE(release.has_value());
  EXPECT_EQ(release->tag, "v0.4.0");
  EXPECT_EQ(release->version, (Version{0, 4, 0}));
  EXPECT_EQ(release->html_url, "https://github.com/PychronLabsLLC/pychron-cpp/releases/tag/v0.4.0");
  EXPECT_FALSE(release->notes.empty());
}

TEST(UpdateRelease, RefusesDraft) {
  EXPECT_FALSE(parse_latest_release(replaced(fixture(), "\"draft\": false", "\"draft\": true")).has_value());
}

TEST(UpdateRelease, RefusesPrerelease) {
  EXPECT_FALSE(parse_latest_release(replaced(fixture(), "\"prerelease\": false", "\"prerelease\": true")).has_value());
}

TEST(UpdateRelease, RefusesMissingTag) {
  EXPECT_FALSE(parse_latest_release(replaced(fixture(), "\"tag_name\"", "\"tag\"")).has_value());
}

TEST(UpdateRelease, RefusesMissingUrl) {
  EXPECT_FALSE(parse_latest_release(replaced(fixture(), "\"html_url\"", "\"url\"")).has_value());
}

TEST(UpdateRelease, RefusesBadTag) {
  EXPECT_FALSE(parse_latest_release(replaced(fixture(), "\"v0.4.0\",", "\"nightly\",")).has_value());
}

TEST(UpdateRelease, RefusesMalformedJson) {
  EXPECT_FALSE(parse_latest_release("{").has_value());
}

TEST(UpdateRelease, RefusesNonObject) {
  EXPECT_FALSE(parse_latest_release("[]").has_value());
}

TEST(UpdateRelease, MissingBodyIsEmptyNotes) {
  for (const char* text : {R"({"tag_name":"v1.2.3","html_url":"u"})", R"({"tag_name":"v1.2.3","html_url":"u","body":null})"}) {
    const auto release = parse_latest_release(text);
    ASSERT_TRUE(release.has_value()) << text;
    EXPECT_TRUE(release->notes.empty());
    EXPECT_EQ(release->version, (Version{1, 2, 3}));
  }
}

}  // namespace
}  // namespace pychron::update
