// A scene written as SVG: a valid, deterministic document of one element per
// draw-list item, and bytes that do not depend on the compiler (fixture).

#include <gtest/gtest.h>

#include <string>

#include "pychron/processing/scene_draw.hpp"
#include "pychron/processing/scene_svg.hpp"
#include "scene_fixture.hpp"

namespace pychron::processing {
namespace {

std::size_t count(const std::string& s, const std::string& what) {
  std::size_t n = 0;
  for (auto at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size())) ++n;
  return n;
}

TEST(SceneSvg, EmptySceneIsAValidDocument) {
  const std::string svg = scene_svg(Scene{});
  EXPECT_TRUE(svg.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg "));
  EXPECT_EQ(count(svg, "<svg "), 1u);
  EXPECT_EQ(count(svg, "</svg>"), 1u);
  EXPECT_TRUE(svg.ends_with("</svg>\n"));
  EXPECT_NE(svg.find(">No data</text>"), std::string::npos);
}

TEST(SceneSvg, SameSceneSameBytes) {
  const Scene s = testing::fixture_scene();
  EXPECT_EQ(scene_svg(s), scene_svg(s));
}

TEST(SceneSvg, OneElementPerItem) {
  const Scene s = testing::fixture_scene();
  const std::string svg = scene_svg(s);
  const DrawList list = draw_scene(s);
  EXPECT_EQ(count(svg, "<path ") + count(svg, "<text "), list.items.size());
  EXPECT_EQ(count(svg, "<clipPath "), list.clips.size());
  EXPECT_EQ(svg.find("nan"), std::string::npos);
  EXPECT_EQ(svg.find("inf"), std::string::npos);
}

TEST(SceneSvg, SizeIsInPoints) {
  const std::string svg = scene_svg(testing::fixture_scene());
  EXPECT_NE(svg.find("width=\"869pt\" height=\"653pt\" viewBox=\"0 0 869 653\""), std::string::npos);
}

TEST(SceneSvg, EscapesMarkup) {
  const std::string svg = scene_svg(testing::fixture_scene());
  EXPECT_NE(svg.find(">FC-1 &lt;A&amp;B&gt; &quot;test&quot;</text>"), std::string::npos);
  EXPECT_EQ(svg.find("<A&B>"), std::string::npos);
  EXPECT_NE(svg.find("\xc2\xb1 0.05 Ma (2\xcf\x83)</text>"), std::string::npos);  // UTF-8 as is
}

TEST(SceneSvg, DropsControlCharacters) {
  Scene s;
  Graph g;
  g.title = std::string("a\x01" "b");
  g.panels.emplace_back();
  s.graphs.push_back(g);
  EXPECT_NE(scene_svg(s).find(">ab</text>"), std::string::npos);
}

TEST(SceneSvg, NamesTheFontFamily) {
  Scene s;
  EXPECT_NE(scene_svg(s).find("font-family=\"Helvetica, Arial, sans-serif\""), std::string::npos);
  s.style.fonts.family = "Times";
  EXPECT_NE(scene_svg(s).find("font-family=\"Times, Helvetica, Arial, sans-serif\""), std::string::npos);
  EXPECT_EQ(scene_svg(s).find("@font-face"), std::string::npos);
}

TEST(SceneSvg, AlphaIsAnOpacity) {
  const std::string svg = scene_svg(testing::fixture_scene());
  EXPECT_NE(svg.find("fill=\"#1f77b4\" fill-opacity=\"0.24\""), std::string::npos);  // band, alpha 60
}

TEST(SceneSvg, MatchesTheCommittedFixture) {
  const std::string why = testing::compare_with_fixture("figure.svg", scene_svg(testing::fixture_scene()));
  EXPECT_TRUE(why.empty()) << why;
}

TEST(SceneSvg, InvalidUtf8AndNonCharactersAreReplacedOrDropped) {
  Scene s;
  Graph g;
  g.title = std::string("a\xb0z\xef\xbf\xbe" "b");  // a lone continuation byte, U+FFFE
  g.panels.emplace_back();
  s.graphs.push_back(g);
  EXPECT_NE(scene_svg(s).find(">a\xef\xbf\xbdzb</text>"), std::string::npos);
}

TEST(SceneSvg, KeepsRunsOfSpaces) {
  EXPECT_NE(scene_svg(Scene{}).find(" xml:space=\"preserve\""), std::string::npos);
}

}  // namespace
}  // namespace pychron::processing
