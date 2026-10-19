// A scene written as a one-page PDF: a file a reader can parse (xref offsets
// and stream lengths checked by the small reader below), the same bytes
// every time and on every compiler (fixture), text in the standard fonts.

#include <gtest/gtest.h>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "pychron/processing/scene_pdf.hpp"
#include "pychron/processing/scene_svg.hpp"
#include "scene_fixture.hpp"
#include "temp_path.hpp"

namespace pychron::processing {
namespace {

struct PdfCheck {
  bool ok = false;
  std::string why;
};

std::size_t number_at(std::string_view s, std::size_t at) {
  std::size_t v = 0;
  std::from_chars(s.data() + at, s.data() + s.size(), v);
  return v;
}

// The structure a reader relies on: startxref names the xref table, every
// in-use entry's offset starts "<n> 0 obj", /Size is the entry count, and
// every stream's /Length is its byte count.
PdfCheck check_pdf(std::string_view pdf) {
  if (!pdf.starts_with("%PDF-1.4\n")) return {false, "no %PDF-1.4 header"};
  if (!pdf.ends_with("%%EOF\n")) return {false, "no %%EOF at the end"};
  const auto sx = pdf.rfind("startxref\n");
  if (sx == std::string_view::npos) return {false, "no startxref"};
  const std::size_t xref = number_at(pdf, sx + 10);
  if (pdf.substr(xref, 5) != "xref\n") return {false, "startxref does not point at xref"};
  std::size_t at = xref + 5;
  const std::size_t first = number_at(pdf, at);
  at = pdf.find(' ', at) + 1;
  const std::size_t count = number_at(pdf, at);
  at = pdf.find('\n', at) + 1;
  if (first != 0) return {false, "xref does not start at object 0"};
  for (std::size_t n = 0; n < count; ++n, at += 20) {
    const std::string_view entry = pdf.substr(at, 20);
    if (entry.size() != 20 || entry.substr(18) != " \n") return {false, "xref entry " + std::to_string(n) + " is not 20 bytes"};
    if (entry[17] == 'f') continue;
    const std::size_t offset = number_at(entry, 0);
    const std::string head = std::to_string(n) + " 0 obj";
    if (pdf.substr(offset, head.size()) != head) return {false, "object " + std::to_string(n) + " is not at its offset"};
  }
  const auto size = pdf.find("/Size ", at);
  if (size == std::string_view::npos || number_at(pdf, size + 6) != count) return {false, "/Size is not the xref count"};
  for (auto s = pdf.find("/Length "); s != std::string_view::npos; s = pdf.find("/Length ", s + 1)) {
    const std::size_t length = number_at(pdf, s + 8);
    const auto begin = pdf.find("stream\n", s);
    if (begin == std::string_view::npos) return {false, "a /Length with no stream"};
    if (pdf.substr(begin + 7 + length, 10) != "\nendstream") return {false, "a stream's /Length is not its size"};
  }
  return {true, {}};
}

std::size_t count(const std::string& s, const std::string& what) {
  std::size_t n = 0;
  for (auto at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size())) ++n;
  return n;
}

const PdfInfo kInfo{1760000000, "fixture"};

TEST(ScenePdf, EmptySceneIsAValidDocument) {
  const std::string pdf = scene_pdf(Scene{}, {});
  const PdfCheck c = check_pdf(pdf);
  EXPECT_TRUE(c.ok) << c.why;
  EXPECT_NE(pdf.find("(No data) Tj"), std::string::npos);
}

TEST(ScenePdf, XrefOffsetsAreRight) {
  const PdfCheck c = check_pdf(scene_pdf(testing::fixture_scene(), kInfo));
  EXPECT_TRUE(c.ok) << c.why;
}

TEST(ScenePdf, SameSceneSameBytesAndNoId) {
  const Scene s = testing::fixture_scene();
  const std::string pdf = scene_pdf(s, kInfo);
  EXPECT_EQ(pdf, scene_pdf(s, kInfo));
  EXPECT_EQ(pdf.find("/ID"), std::string::npos);
  EXPECT_EQ(pdf.find("/Filter"), std::string::npos);  // uncompressed
}

TEST(ScenePdf, OnePageOfTheSceneSize) {
  const std::string pdf = scene_pdf(testing::fixture_scene(), kInfo);
  EXPECT_EQ(count(pdf, "/Type /Page "), 1u);
  EXPECT_NE(pdf.find("/MediaBox [0 0 869 653]"), std::string::npos);
}

TEST(ScenePdf, CreationDateIsTheGivenTime) {
  const std::string pdf = scene_pdf(Scene{}, kInfo);
  EXPECT_NE(pdf.find("/CreationDate (D:20251009085320Z)"), std::string::npos);
  EXPECT_NE(pdf.find("/Title (fixture)"), std::string::npos);
  EXPECT_NE(pdf.find("/Producer (pychron-cpp)"), std::string::npos);
}

TEST(ScenePdf, UsesTheStandardFonts) {
  const std::string pdf = scene_pdf(testing::fixture_scene(), kInfo);
  EXPECT_NE(pdf.find("/BaseFont /Helvetica /Encoding /WinAnsiEncoding"), std::string::npos);
  EXPECT_NE(pdf.find("/BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding"), std::string::npos);
  EXPECT_NE(pdf.find("/BaseFont /Symbol"), std::string::npos);
  EXPECT_EQ(pdf.find("/FontFile"), std::string::npos);  // nothing embedded
}

TEST(ScenePdf, EncodesTextForTheStandardFonts) {
  Scene s;
  Graph g;
  g.title = "(a\\b) \xc2\xb1 \xcf\x83 \xc3\xa9 \xe4\xb8\xad";  // (a\b) ± σ é 中
  g.panels.emplace_back();
  s.graphs.push_back(g);
  const std::string pdf = scene_pdf(s, {});
  EXPECT_NE(pdf.find("/F2 12 Tf (\\(a\\\\b\\) \\261 ) Tj /F3 12 Tf (s) Tj /F2 12 Tf ( \\351 ?) Tj"), std::string::npos)
      << pdf.substr(0, 4000);
}

TEST(ScenePdf, AlphaUsesOneGraphicsStatePerValue) {
  const std::string pdf = scene_pdf(testing::fixture_scene(), kInfo);
  EXPECT_EQ(count(pdf, "/Fa60 "), 1u + count(pdf, "/Fa60 gs"));
  EXPECT_NE(pdf.find("<< /Type /ExtGState /ca 0.235 >>"), std::string::npos);
  EXPECT_NE(pdf.find("/Fa120 gs"), std::string::npos);
}

TEST(ScenePdf, SameTextsAsTheSvg) {
  const Scene s = testing::fixture_scene();
  EXPECT_EQ(count(scene_pdf(s, kInfo), "BT "), count(scene_svg(s), "<text "));
}

TEST(ScenePdf, MatchesTheCommittedFixture) {
  const std::string why = testing::compare_with_fixture("figure.pdf", scene_pdf(testing::fixture_scene(), kInfo));
  EXPECT_TRUE(why.empty()) << why;
}

TEST(ScenePdf, SaveWritesTheBytes) {
  const auto path = pychron::testing::unique_temp_path("scene_pdf", ".pdf");
  ASSERT_TRUE(save_scene_pdf(testing::fixture_scene(), kInfo, path));
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  EXPECT_EQ(ss.str(), scene_pdf(testing::fixture_scene(), kInfo));
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ScenePdf, SaveToAMissingDirectoryIsAnIoError) {
  const auto r = save_scene_pdf(Scene{}, {}, pychron::testing::unique_temp_path("no_dir") / "x" / "f.pdf");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
}

TEST(ScenePdf, ACreationDateOutOfRangeIsClamped) {
  EXPECT_NE(scene_pdf(Scene{}, {-5, ""}).find("/CreationDate (D:19700101000000Z)"), std::string::npos);
  EXPECT_NE(scene_pdf(Scene{}, {INT64_MAX, ""}).find("/CreationDate (D:99991231235959Z)"), std::string::npos);
}

}  // namespace
}  // namespace pychron::processing
