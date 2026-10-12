// The workbook templates' zip and sheet XML (libs/earthbank/src/ooxml.hpp):
// what is read from the two vendor templates, what a written row looks like,
// and that everything else in a saved copy is the template's, byte for byte.
// Damaged and hostile files are made here, in the test.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "ooxml.hpp"
#include "pychron/core/error.hpp"
#include "zip_support.hpp"

namespace pychron::earthbank::ooxml {
namespace {

namespace fs = std::filesystem;

const char* const kArAr = "ArArDataPoint_Template2026.xlsx";
const char* const kSample = "Sample_Template_v2025-04-16.xlsx";

fs::path template_file(const char* name) { return fs::path(PYCHRON_EARTHBANK_TEMPLATE_DIR) / name; }

std::string slurp(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void spit(const fs::path& file, std::string_view bytes) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

long process_id() {
#ifdef _WIN32
  return static_cast<long>(_getpid());
#else
  return static_cast<long>(getpid());
#endif
}

void set_time_zone(const char* zone) {
#ifdef _WIN32
  _putenv_s("TZ", zone);
  _tzset();
#else
  setenv("TZ", zone, 1);
  tzset();
#endif
}

// ---- A zip made by hand, and a size in it that can be a lie ----------------

using testing::make_zip;
using testing::Part;

// The DOS time and date of each entry and its general-purpose flags, read
// from the central directory of an archive without a comment, and the flags
// its local header states.
struct Stamp {
  std::string name;
  unsigned time = 0;
  unsigned date = 0;
  unsigned flags = 0;
  unsigned local_flags = 0;
};

std::vector<Stamp> stamps(std::string_view zip) {
  const auto u16 = [&](std::size_t at) {
    return static_cast<unsigned>(static_cast<unsigned char>(zip[at])) |
           (static_cast<unsigned>(static_cast<unsigned char>(zip[at + 1])) << 8);
  };
  const auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16); };
  std::vector<Stamp> out;
  if (zip.size() < 22) return out;
  const std::size_t end = zip.size() - 22;
  const unsigned count = u16(end + 10);
  std::size_t at = u32(end + 16);
  for (unsigned i = 0; i < count && at + 46 <= zip.size(); ++i) {
    const std::size_t name = u16(at + 28);
    const std::size_t local = u32(at + 42);
    const unsigned local_flags = local + 30 <= zip.size() ? u16(local + 6) : 0xFFFFu;
    out.push_back({std::string(zip.substr(at + 46, name)), u16(at + 12), u16(at + 14), u16(at + 8), local_flags});
    at += 46 + name + u16(at + 30) + u16(at + 32);
  }
  return out;
}

// A workbook of one sheet, "Data", with the sheet XML given.
const char* const kSheetHead =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";

std::vector<Part> small_workbook(const std::string& sheet_body, const std::string& shared_strings = {}) {
  const std::string strings_relationship =
      shared_strings.empty()
          ? std::string()
          : "<Relationship Id=\"rId9\" "
            "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" "
            "Target=\"sharedStrings.xml\"/>";
  std::vector<Part> parts = {
      {"[Content_Types].xml", "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"/>"},
      {"xl/workbook.xml",
       "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
       "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
       "<sheets><sheet name=\"Data\" sheetId=\"1\" r:id=\"rId7\"/></sheets></workbook>"},
      {"xl/_rels/workbook.xml.rels",
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
       "<Relationship Id=\"rId7\" "
       "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
       "Target=\"worksheets/data.xml\"/>" +
           strings_relationship + "</Relationships>"},
      {"xl/worksheets/data.xml", std::string(kSheetHead) + sheet_body + "</worksheet>"},
  };
  if (!shared_strings.empty()) parts.push_back({"xl/sharedStrings.xml", shared_strings});
  return parts;
}

std::string text_cell(const std::string& ref, const std::string& text) {
  return "<c r=\"" + ref + R"(" t="inlineStr"><is><t xml:space="preserve">)" + text + "</t></is></c>";
}

class Ooxml : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    // Per process as well as per test: two build trees may run at once.
    dir_ = fs::temp_directory_path() /
           (std::string("pychron_ooxml_") + info->name() + "_" + std::to_string(process_id()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  fs::path file(const std::string& name) const { return dir_ / name; }

  fs::path zip_file(const std::string& name, const std::vector<Part>& parts) const {
    spit(file(name), make_zip(parts));
    return file(name);
  }

  // A workbook that must open: the test ends here when it does not.
  static Workbook open(const fs::path& path) {
    auto book = Workbook::open(path);
    if (!book) throw std::runtime_error("cannot open " + path.string() + ": " + to_string(book.error()));
    return std::move(book.value());
  }

  static std::vector<ZipEntry> entries(const fs::path& path) {
    auto list = read_zip(path);
    EXPECT_TRUE(list.has_value()) << (list ? "" : to_string(list.error()));
    return list ? std::move(list.value()) : std::vector<ZipEntry>{};
  }

  static std::string part(const std::vector<ZipEntry>& list, std::string_view name) {
    for (const ZipEntry& entry : list) {
      if (entry.name == name) return entry.data;
    }
    ADD_FAILURE() << "no part " << name;
    return {};
  }

  // The sheet XML of a one-sheet workbook after `rows` were written to it.
  std::string written(const std::string& sheet_body, int first_row, const std::vector<std::vector<Cell>>& rows) {
    Workbook book = open(zip_file("in.xlsx", small_workbook(sheet_body)));
    const auto wrote = book.write_rows("Data", first_row, rows);
    EXPECT_TRUE(wrote.has_value()) << (wrote ? "" : to_string(wrote.error()));
    const auto saved = book.save(file("out.xlsx"));
    EXPECT_TRUE(saved.has_value()) << (saved ? "" : to_string(saved.error()));
    std::string xml = part(entries(file("out.xlsx")), "xl/worksheets/data.xml");
    const std::string_view head = kSheetHead;
    const std::string_view tail = "</worksheet>";
    EXPECT_EQ(xml.substr(0, head.size()), head);
    EXPECT_EQ(xml.substr(xml.size() - tail.size()), tail);
    return xml.substr(head.size(), xml.size() - head.size() - tail.size());
  }

  fs::path dir_;
};

// ---- Reading the templates -------------------------------------------------

TEST_F(Ooxml, OpensBothTemplatesAndNamesTheirSheets) {
  EXPECT_EQ(open(template_file(kArAr)).sheet_names(),
            (std::vector<std::string>{"ArArDataPoints", "ArArMeasurement", "ArArAliquot", "ArArAgeSummary",
                                      "ArArAgeCalc", "Field Definitions", "Datapoint Props", "Lookup Tables"}));
  EXPECT_EQ(open(template_file(kSample)).sheet_names(),
            (std::vector<std::string>{"Samples", "Sample Props", "Column Descriptions", "Lookup tables"}));
}

TEST_F(Ooxml, Row4IsTheApiFieldRow) {
  const Workbook arar = open(template_file(kArAr));
  const auto contains = [](const std::vector<std::string>& row, std::string_view name) {
    return std::find(row.begin(), row.end(), name) != row.end();
  };

  const auto measurement = arar.row_values("ArArMeasurement", 4);
  ASSERT_TRUE(measurement.has_value()) << to_string(measurement.error());
  EXPECT_EQ(measurement->size(), 68u);
  EXPECT_EQ(measurement->front(), "datapointName");
  EXPECT_EQ(measurement->back(), "measurementComment");
  EXPECT_TRUE(contains(*measurement, "ar40Corr"));
  EXPECT_TRUE(contains(*measurement, "apparentAge"));
  EXPECT_TRUE(contains(*measurement, "stepNumber"));

  EXPECT_EQ(arar.row_values("ArArAliquot", 4).value(),
            (std::vector<std::string>{"datapointName", "aliquotName", "aliquotSize", "aliquotUnits",
                                      "aliquotComment"}));
  EXPECT_EQ(arar.row_values("ArArAgeCalc", 4).value(),
            (std::vector<std::string>{"datapointName", "decayConstant", "airRatio", "fluxMonitor"}));
  EXPECT_EQ(arar.row_values("ArArDataPoints", 4).value().size(), 22u);
  // 13 named columns: the 53 styled, empty cells after them are not values.
  EXPECT_EQ(arar.row_values("ArArAgeSummary", 4).value().size(), 13u);

  // The property sheets name their fields in row 1.
  EXPECT_EQ(arar.row_values("Datapoint Props", 1).value(),
            (std::vector<std::string>{"datapointName", "propName", "propValue"}));
  const Workbook sample = open(template_file(kSample));
  EXPECT_EQ(sample.row_values("Sample Props", 1).value(),
            (std::vector<std::string>{"sampleName", "propName", "propValue"}));
  const auto samples = sample.row_values("Samples", 4);
  ASSERT_TRUE(samples.has_value());
  ASSERT_EQ(samples->size(), 30u);
  EXPECT_EQ(samples->front(), "sampleName");
  EXPECT_EQ(samples->back(), "Tag");
}

TEST_F(Ooxml, APlaceholderRowAndAMissingRowHaveNoValues) {
  const Workbook arar = open(template_file(kArAr));
  EXPECT_TRUE(arar.row_values("ArArMeasurement", 5).value().empty());      // styled, empty cells
  EXPECT_TRUE(arar.row_values("ArArMeasurement", 5000).value().empty());   // not stored
  EXPECT_TRUE(arar.row_values("Datapoint Props", 2).value().empty());
  // A gap in a row is an empty string: Lookup Tables row 4 has no B.
  const auto lookup = arar.row_values("Lookup Tables", 4);
  ASSERT_TRUE(lookup.has_value());
  ASSERT_GE(lookup->size(), 3u);
  EXPECT_FALSE((*lookup)[0].empty());
  EXPECT_TRUE((*lookup)[1].empty());
  EXPECT_FALSE((*lookup)[2].empty());
}

TEST_F(Ooxml, TheRunsOfARichTextAreJoined) {
  // Samples!E1 is three runs in the template's shared strings, the middle one
  // a link's text.
  const auto row = open(template_file(kSample)).row_values("Samples", 1);
  ASSERT_TRUE(row.has_value()) << to_string(row.error());
  ASSERT_GE(row->size(), 5u);
  EXPECT_EQ((*row)[4],
            "The composition or type of rock sample, e,g. sandstone, limestone, chert, coal.  Selectable from "
            "list. Supported by mindat.org (If desired material is not available in current list, more options "
            "may be available during upload)");
}

// ---- Saving ----------------------------------------------------------------

TEST_F(Ooxml, SaveWithNoChangeKeepsEveryPartByteIdentical) {
  for (const char* name : {kArAr, kSample}) {
    SCOPED_TRACE(name);
    const Workbook book = open(template_file(name));
    ASSERT_TRUE(book.save(file("copy.xlsx")).has_value());
    const auto before = entries(template_file(name));
    const auto after = entries(file("copy.xlsx"));
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
      SCOPED_TRACE(before[i].name);
      EXPECT_EQ(after[i].name, before[i].name);
      EXPECT_TRUE(after[i].data == before[i].data);
      EXPECT_TRUE(after[i].stored == before[i].stored);  // copied, not recompressed
    }
  }
}

TEST_F(Ooxml, WrittenRowsReadBack) {
  Workbook book = open(template_file(kArAr));
  const std::string awkward = "Caf\xC3\xA9 & <Co> \"x\"";
  const std::vector<std::vector<Cell>> rows = {
      {Cell{"dp-1"}, Cell{}, Cell{" leading and trailing "}, Cell{12.5}, Cell{awkward}},
      {Cell{"dp-2"}, Cell{"line one\nline two\ttabbed\rreturned"}, Cell{-0.001}, Cell{}, Cell{std::string()}},
  };
  ASSERT_TRUE(book.write_rows("ArArMeasurement", 5, rows).has_value());

  const std::vector<std::string> first = {"dp-1", "", " leading and trailing ", "12.5", awkward};
  const std::vector<std::string> second = {"dp-2", "line one\nline two\ttabbed\rreturned", "-0.001"};
  EXPECT_EQ(book.row_values("ArArMeasurement", 5).value(), first);
  EXPECT_EQ(book.row_values("ArArMeasurement", 6).value(), second);
  EXPECT_TRUE(book.row_values("ArArMeasurement", 7).value().empty());

  // And from the file, by a workbook that never saw the rows.
  ASSERT_TRUE(book.save(file("out.xlsx")).has_value());
  const Workbook again = open(file("out.xlsx"));
  EXPECT_EQ(again.row_values("ArArMeasurement", 5).value(), first);
  EXPECT_EQ(again.row_values("ArArMeasurement", 6).value(), second);
  EXPECT_EQ(again.row_values("ArArMeasurement", 4).value(),
            open(template_file(kArAr)).row_values("ArArMeasurement", 4).value());
}

TEST_F(Ooxml, OnlyTheTouchedSheetChanges) {
  Workbook book = open(template_file(kArAr));
  const std::vector<std::vector<Cell>> rows = {{Cell{"a"}, Cell{1.0}}, {Cell{"b"}}, {Cell{"c"}, Cell{}, Cell{3.0}}};
  ASSERT_TRUE(book.write_rows("ArArMeasurement", 5, rows).has_value());
  ASSERT_TRUE(book.save(file("out.xlsx")).has_value());

  const auto before = entries(template_file(kArAr));
  const auto after = entries(file("out.xlsx"));
  ASSERT_EQ(after.size(), before.size());
  const std::string sheet = "xl/worksheets/sheet2.xml";  // ArArMeasurement
  for (std::size_t i = 0; i < before.size(); ++i) {
    SCOPED_TRACE(before[i].name);
    EXPECT_EQ(after[i].name, before[i].name);
    if (before[i].name == sheet) continue;
    EXPECT_TRUE(after[i].data == before[i].data);
    EXPECT_TRUE(after[i].stored == before[i].stored);
  }

  // In the sheet: everything before row 5's start tag (the four header rows
  // among it) and everything from row 8's start tag on. The written cells lie
  // inside the template's <dimension>, so that is unchanged too.
  const std::string old_xml = part(before, sheet);
  const std::string new_xml = part(after, sheet);
  ASSERT_NE(old_xml, new_xml);
  const std::size_t head = old_xml.find("<row r=\"5\"");
  ASSERT_NE(head, std::string::npos);
  EXPECT_NE(old_xml.substr(0, head).find("<row r=\"4\""), std::string::npos);
  EXPECT_TRUE(new_xml.substr(0, head) == old_xml.substr(0, head));
  EXPECT_EQ(new_xml.compare(head, 10, "<row r=\"5\""), 0);
  const std::size_t old_tail = old_xml.find("<row r=\"8\"");
  const std::size_t new_tail = new_xml.find("<row r=\"8\"");
  ASSERT_NE(old_tail, std::string::npos);
  ASSERT_NE(new_tail, std::string::npos);
  EXPECT_TRUE(new_xml.substr(new_tail) == old_xml.substr(old_tail));
}

TEST_F(Ooxml, AWrittenRowFillsThePlaceholderRow) {
  // The template's row: <row r="5" spans="1:22" ht="15"><c r="B5" s="3"/>
  // <c r="C5" s="4"/><c r="D5" s="4"/><c r="E5" s="4"/></row>
  Workbook book = open(template_file(kArAr));
  const std::vector<Cell> row = {Cell{"dp"}, Cell{}, Cell{}, Cell{2.0}, Cell{}, Cell{}, Cell{"x"}};
  ASSERT_TRUE(book.write_rows("ArArDataPoints", 5, {row}).has_value());
  ASSERT_TRUE(book.save(file("out.xlsx")).has_value());
  const std::string xml = part(entries(file("out.xlsx")), "xl/worksheets/sheet1.xml");
  const std::size_t begin = xml.find("<row r=\"5\"");
  const std::size_t end = xml.find("<row r=\"6\"");
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  EXPECT_EQ(xml.substr(begin, end - begin),
            "<row r=\"5\" ht=\"15\">" + text_cell("A5", "dp") +
                "<c r=\"B5\" s=\"3\"/><c r=\"C5\" s=\"4\"/><c r=\"D5\" s=\"4\"><v>2</v></c><c r=\"E5\" s=\"4\"/>" +
                text_cell("G5", "x") + "</row>");
  // Row 6 was not written to: it keeps its spans.
  const std::string next = R"(<row r="6" spans="1:22" ht="15">)";
  EXPECT_EQ(xml.substr(end, next.size()), next);
}

TEST_F(Ooxml, RowsPastTheLastStoredRowAreAddedAndTheDimensionGrows) {
  // ArArAgeSummary stores rows 1 to 7 and says <dimension ref="A1:BN7"/>.
  Workbook book = open(template_file(kArAr));
  std::vector<std::vector<Cell>> rows;
  rows.reserve(6);
  for (int i = 0; i < 6; ++i) rows.push_back({Cell{"dp-" + std::to_string(i)}, Cell{}, Cell{static_cast<double>(i)}});
  ASSERT_TRUE(book.write_rows("ArArAgeSummary", 5, rows).has_value());
  const std::vector<std::vector<Cell>> props = {{Cell{"dp-0"}, Cell{"k"}, Cell{"v"}}, {Cell{"dp-1"}}};
  ASSERT_TRUE(book.write_rows("Datapoint Props", 2, props).has_value());
  ASSERT_TRUE(book.save(file("out.xlsx")).has_value());

  const auto before = entries(template_file(kArAr));
  const auto after = entries(file("out.xlsx"));
  {
    const std::string old_xml = part(before, "xl/worksheets/sheet4.xml");
    const std::string new_xml = part(after, "xl/worksheets/sheet4.xml");
    const std::string old_dim = "<dimension ref=\"A1:BN7\"/>";
    const std::string new_dim = "<dimension ref=\"A1:BN10\"/>";
    const std::size_t dim = old_xml.find(old_dim);
    ASSERT_NE(dim, std::string::npos);
    EXPECT_TRUE(new_xml.substr(0, dim) == old_xml.substr(0, dim));
    ASSERT_EQ(new_xml.compare(dim, new_dim.size(), new_dim), 0);
    // Between the dimension and row 5, and from </sheetData> on.
    const std::size_t old_row = old_xml.find("<row r=\"5\"");
    EXPECT_TRUE(new_xml.substr(dim + new_dim.size(), old_row - dim - old_dim.size()) ==
                old_xml.substr(dim + old_dim.size(), old_row - dim - old_dim.size()));
    const std::size_t old_close = old_xml.find("</sheetData>");
    const std::size_t new_close = new_xml.find("</sheetData>");
    EXPECT_TRUE(new_xml.substr(new_close) == old_xml.substr(old_close));
    // Rows 8 to 10 are bare rows, straight before </sheetData>.
    const std::string added = "<row r=\"8\">" + text_cell("A8", "dp-3") + "<c r=\"C8\"><v>3</v></c></row>" +
                              "<row r=\"9\">" + text_cell("A9", "dp-4") + "<c r=\"C9\"><v>4</v></c></row>" +
                              "<row r=\"10\">" + text_cell("A10", "dp-5") + "<c r=\"C10\"><v>5</v></c></row>";
    ASSERT_GT(new_close, added.size());
    EXPECT_EQ(new_xml.substr(new_close - added.size(), added.size()), added);
  }
  {
    const std::string old_xml = part(before, "xl/worksheets/sheet7.xml");
    const std::string new_xml = part(after, "xl/worksheets/sheet7.xml");
    EXPECT_NE(new_xml.find("<dimension ref=\"A1:C3\"/>"), std::string::npos);
    const std::size_t old_close = old_xml.find("</sheetData>");
    const std::size_t new_close = new_xml.find("</sheetData>");
    EXPECT_TRUE(new_xml.substr(new_close) == old_xml.substr(old_close));
    EXPECT_EQ(new_xml.substr(old_close, new_close - old_close),
              "<row r=\"2\">" + text_cell("A2", "dp-0") + text_cell("B2", "k") + text_cell("C2", "v") + "</row>" +
                  "<row r=\"3\">" + text_cell("A3", "dp-1") + "</row>");
  }
  const Workbook again = open(file("out.xlsx"));
  EXPECT_EQ(again.row_values("ArArAgeSummary", 10).value(), (std::vector<std::string>{"dp-5", "", "5"}));
  EXPECT_EQ(again.row_values("Datapoint Props", 2).value(), (std::vector<std::string>{"dp-0", "k", "v"}));
}

TEST_F(Ooxml, ASheetWithoutADimensionGetsNone) {
  // Neither sheet of the Sample template has a <dimension>.
  Workbook book = open(template_file(kSample));
  ASSERT_TRUE(book.write_rows("Samples", 5, {{Cell{"S-1"}, Cell{}, Cell{"Rock"}}}).has_value());
  ASSERT_TRUE(book.write_rows("Sample Props", 2, {{Cell{"S-1"}, Cell{"k"}, Cell{"v"}}}).has_value());
  ASSERT_TRUE(book.save(file("out.xlsx")).has_value());
  const auto before = entries(template_file(kSample));
  const auto after = entries(file("out.xlsx"));
  ASSERT_EQ(after.size(), before.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    SCOPED_TRACE(before[i].name);
    const bool touched = before[i].name == "xl/worksheets/sheet1.xml" || before[i].name == "xl/worksheets/sheet2.xml";
    EXPECT_EQ(after[i].data == before[i].data, !touched);
    EXPECT_EQ(after[i].data.find("<dimension"), std::string::npos);
  }
  const std::string old_xml = part(before, "xl/worksheets/sheet1.xml");
  const std::string new_xml = part(after, "xl/worksheets/sheet1.xml");
  const std::size_t head = old_xml.find("<row r=\"5\"");
  EXPECT_TRUE(new_xml.substr(0, head) == old_xml.substr(0, head));
  EXPECT_TRUE(new_xml.substr(new_xml.find("<row r=\"6\"")) == old_xml.substr(old_xml.find("<row r=\"6\"")));
  // The template's styles stay on the filled cells (A5 is s="17", C5 s="18").
  EXPECT_NE(new_xml.find("<c r=\"A5\" s=\"17\" t=\"inlineStr\"><is><t xml:space=\"preserve\">S-1</t></is></c>"
                         "<c r=\"B5\" s=\"17\"/>"
                         "<c r=\"C5\" s=\"18\" t=\"inlineStr\"><is><t xml:space=\"preserve\">Rock</t></is></c>"
                         "<c r=\"D5\" s=\"17\"/>"),
            std::string::npos);
  EXPECT_EQ(open(file("out.xlsx")).row_values("Samples", 5).value(), (std::vector<std::string>{"S-1", "", "Rock"}));
}

TEST_F(Ooxml, TheResultIsAWellFormedZip) {
  for (const char* name : {kArAr, kSample}) {
    SCOPED_TRACE(name);
    Workbook book = open(template_file(name));
    const std::string sheet = book.sheet_names().front();
    ASSERT_TRUE(book.write_rows(sheet, 5, {{Cell{"x"}, Cell{1.0}}}).has_value());
    ASSERT_TRUE(book.save(file("out.xlsx")).has_value());
    EXPECT_EQ(entries(file("out.xlsx")).size(), entries(template_file(name)).size());
    EXPECT_EQ(stamps(slurp(file("out.xlsx"))).size(), stamps(slurp(template_file(name))).size());
    const Workbook again = open(file("out.xlsx"));
    EXPECT_EQ(again.sheet_names(), book.sheet_names());
    EXPECT_EQ(again.row_values(sheet, 5).value(), (std::vector<std::string>{"x", "1"}));
  }
}

TEST_F(Ooxml, TheSameInputGivesTheSameBytes) {
  const auto write = [&](const std::string& out) {
    Workbook book = open(template_file(kSample));
    ASSERT_TRUE(book.write_rows("Samples", 5, {{Cell{"S-1"}, Cell{2.5}}, {Cell{"S-2"}}}).has_value());
    ASSERT_TRUE(book.save(file(out)).has_value());
  };
  // Not the wall clock, and not the time zone it is read in either.
  set_time_zone("UTC");
  write("one.xlsx");
  set_time_zone("Australia/Melbourne");
  write("two.xlsx");
  set_time_zone("UTC");
  const std::string one = slurp(file("one.xlsx"));
  ASSERT_FALSE(one.empty());
  EXPECT_TRUE(one == slurp(file("two.xlsx")));

  // An untouched entry keeps the template's time; the rewritten sheet has
  // the first moment a zip can state, 1980-01-01 00:00.
  const auto before = stamps(slurp(template_file(kSample)));
  const auto after = stamps(one);
  ASSERT_EQ(after.size(), before.size());
  ASSERT_FALSE(after.empty());
  for (std::size_t i = 0; i < before.size(); ++i) {
    SCOPED_TRACE(before[i].name);
    EXPECT_EQ(after[i].name, before[i].name);
    if (before[i].name == "xl/worksheets/sheet1.xml") {
      EXPECT_EQ(after[i].time, 0u);
      EXPECT_EQ(after[i].date, 0x0021u);
      EXPECT_NE(before[i].date, 0x0021u);
    } else {
      EXPECT_EQ(after[i].time, before[i].time);
      EXPECT_EQ(after[i].date, before[i].date);
    }
  }
}

// ---- Cells -----------------------------------------------------------------

TEST_F(Ooxml, ACellReferenceForAnyColumn) {
  EXPECT_EQ(cell_reference(1, 1), "A1");
  EXPECT_EQ(cell_reference(26, 5), "Z5");
  EXPECT_EQ(cell_reference(27, 5), "AA5");
  EXPECT_EQ(cell_reference(52, 5), "AZ5");
  EXPECT_EQ(cell_reference(53, 5), "BA5");
  EXPECT_EQ(cell_reference(68, 4), "BP4");
  EXPECT_EQ(cell_reference(702, 9), "ZZ9");
  EXPECT_EQ(cell_reference(703, 9), "AAA9");
  EXPECT_EQ(cell_reference(16384, 1048576), "XFD1048576");
  EXPECT_EQ(cell_reference(16385, 1), "");
  EXPECT_EQ(cell_reference(0, 1), "");
  EXPECT_EQ(cell_reference(1, 0), "");
  EXPECT_EQ(cell_reference(1, 1048577), "");
}

TEST_F(Ooxml, ColumnsPastZAndPastZZAreWrittenAndReadBack) {
  std::vector<Cell> row(704);
  row[25] = Cell{"z"};
  row[26] = Cell{"aa"};
  row[701] = Cell{"zz"};
  row[703] = Cell{7.0};
  const std::string body = written(R"(<sheetData><row r="1"><c r="A1"><v>1</v></c></row></sheetData>)", 2, {row});
  EXPECT_EQ(body, "<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row><row r=\"2\">" + text_cell("Z2", "z") +
                      text_cell("AA2", "aa") + text_cell("ZZ2", "zz") + "<c r=\"AAB2\"><v>7</v></c></row></sheetData>");
  const auto values = open(file("out.xlsx")).row_values("Data", 2);
  ASSERT_TRUE(values.has_value());
  ASSERT_EQ(values->size(), 704u);
  EXPECT_EQ((*values)[25], "z");
  EXPECT_EQ((*values)[26], "aa");
  EXPECT_EQ((*values)[700], "");
  EXPECT_EQ((*values)[701], "zz");
  EXPECT_EQ((*values)[703], "7");
}

TEST_F(Ooxml, ANumberIsWrittenPlainlyAndReadsBack) {
  // Fixed notation while the decimal exponent is between -5 and 15, the
  // shortest digits that read back as the same double; scientific outside,
  // with no '+'. Never the locale's.
  const std::vector<std::pair<double, std::string>> numbers = {
      {100000.0, "100000"},
      {2000000.0, "2000000"},
      {0.0001, "0.0001"},
      {-0.0, "0"},
      {0.0, "0"},
      {1e21, "1e21"},
      {1.5e-7, "1.5e-07"},
      {123456789012345.0, "123456789012345"},
      {0.1, "0.1"},
      {5.0, "5"},
      {-1.5, "-1.5"},
      {40.01, "40.01"},
      {1e15, "1000000000000000"},
      {1e16, "1e16"},
      {0.00001, "0.00001"},
      {0.000001, "1e-06"},
      {-2.5e-7, "-2.5e-07"},
      {5.463e-10, "5.463e-10"},
      {1.0 / 3.0, "0.3333333333333333"},
      {1234.5678, "1234.5678"},
      {1e-300, "1e-300"},
      {std::numeric_limits<double>::max(), "1.7976931348623157e308"},
  };
  std::vector<Cell> row;
  row.reserve(numbers.size());
  for (const auto& number : numbers) row.push_back(Cell{number.first});
  written("<sheetData/>", 2, {row});
  const auto values = open(file("out.xlsx")).row_values("Data", 2);
  ASSERT_TRUE(values.has_value());
  ASSERT_EQ(values->size(), numbers.size());
  for (std::size_t i = 0; i < numbers.size(); ++i) {
    EXPECT_EQ((*values)[i], numbers[i].second);
    EXPECT_EQ(std::strtod((*values)[i].c_str(), nullptr), numbers[i].first) << (*values)[i];
  }
}

TEST_F(Ooxml, TextThatLooksLikeAnEscapeIsNotTakenForOne) {
  // Excel, openpyxl and POI read _xHHHH_ in a string as the character HHHH.
  // Text that happens to hold such a sequence has its underscore escaped
  // (as _x005F_), and what is read is decoded, so it comes back as it was.
  const std::vector<std::string> texts = {"_x000D_", "a_x0041_b", "_x005F_", "__x00e9__x0041_",
                                          "no _x here, nor _x12_ or _x12345",
                                          "_xD83D__xDE00_", "tail_x004"};
  std::vector<Cell> row;
  row.reserve(texts.size());
  for (const std::string& text : texts) row.push_back(Cell{text});
  const std::string body = written("<sheetData/>", 2, {row});
  EXPECT_EQ(body, "<sheetData><row r=\"2\">" + text_cell("A2", "_x005F_x000D_") + text_cell("B2", "a_x005F_x0041_b") +
                      text_cell("C2", "_x005F_x005F_") + text_cell("D2", "__x005F_x00e9__x005F_x0041_") +
                      text_cell("E2", "no _x here, nor _x12_ or _x12345") +
                      text_cell("F2", "_x005F_xD83D__x005F_xDE00_") + text_cell("G2", "tail_x004") +
                      "</row></sheetData>");
  EXPECT_EQ(open(file("out.xlsx")).row_values("Data", 2).value(), texts);

  // Read: an escape in a shared or an inline string is the character.
  const std::string sheet = "<sheetData><row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c r=\"B1\" t=\"s\"><v>1</v></c>"
                            "<c r=\"C1\" t=\"inlineStr\"><is><t>x_x0041_y_x005F_z</t></is></c>"
                            "<c r=\"D1\" t=\"s\"><v>2</v></c><c r=\"E1\" t=\"s\"><v>3</v></c></row></sheetData>";
  const std::string strings = "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                              "<si><t>line_x000D__x000A_two</t></si><si><t>caf_x00E9_</t></si>"
                              "<si><t>_xD83D__xDE00_ and _xD83D_ alone</t></si><si><t>_x00zz_ _x41_</t></si></sst>";
  const Workbook book = open(zip_file("escapes.xlsx", small_workbook(sheet, strings)));
  EXPECT_EQ(book.row_values("Data", 1).value(),
            (std::vector<std::string>{"line\r\ntwo", "caf\xC3\xA9", "xAy_z", "\xF0\x9F\x98\x80 and _xD83D_ alone",
                                      "_x00zz_ _x41_"}));
}

TEST_F(Ooxml, TextIsEscapedAndItsSpacesKept) {
  const std::string body =
      written("<sheetData/>", 2, {{Cell{"a & b < c > d \"e\" 'f'"}, Cell{"  two  "}, Cell{"x\r\ny"}, Cell{"]]>"}}});
  EXPECT_EQ(body, "<sheetData><row r=\"2\">" + text_cell("A2", "a &amp; b &lt; c &gt; d \"e\" 'f'") +
                      text_cell("B2", "  two  ") + text_cell("C2", "x&#13;\ny") + text_cell("D2", "]]&gt;") +
                      "</row></sheetData>");
  EXPECT_EQ(open(file("out.xlsx")).row_values("Data", 2).value(),
            (std::vector<std::string>{"a & b < c > d \"e\" 'f'", "  two  ", "x\r\ny", "]]>"}));
}

// ---- Shapes of sheet the templates do not have, made here -----------------

TEST_F(Ooxml, AnEmptySheetDataIsOpened) {
  EXPECT_EQ(written("<dimension ref=\"A1\"/><sheetData/><pageMargins left=\"0.7\"/>", 2, {{Cell{}, Cell{1.5}}}),
            "<dimension ref=\"A1:B2\"/><sheetData><row r=\"2\"><c r=\"B2\"><v>1.5</v></c></row></sheetData>"
            "<pageMargins left=\"0.7\"/>");
  EXPECT_EQ(written("<sheetData></sheetData>", 3, {{Cell{"a"}}}),
            "<sheetData><row r=\"3\">" + text_cell("A3", "a") + "</row></sheetData>");
}

TEST_F(Ooxml, RowsAreFilledInsertedAndAddedInOrder) {
  // Rows 2 and 6 are stored (6 as an empty element), 3 to 5 and 7 are not.
  const std::string body = written(
      "<dimension ref=\"B1:D6\"/><sheetData>"
      "<row r=\"1\"><c r=\"B1\" t=\"str\"><v>head</v></c></row>"
      "<row r=\"2\" spans=\"2:4\" ht=\"15\" customHeight=\"1\"><c r=\"B2\" s=\"3\"></c><c r=\"D2\" s=\"4\"/></row>"
      "<row r=\"6\" spans=\"2:4\"/>"
      "<row r=\"9\"><c r=\"A9\"><v>9</v></c></row>"
      "</sheetData>",
      2,
      {{Cell{}, Cell{"two"}, Cell{2.0}},
       {Cell{}},
       {Cell{"four"}},
       {},
       {Cell{"six"}},
       {Cell{}, Cell{}, Cell{}, Cell{}, Cell{7.0}}});
  EXPECT_EQ(body,
            "<dimension ref=\"A1:E7\"/><sheetData>"
            "<row r=\"1\"><c r=\"B1\" t=\"str\"><v>head</v></c></row>"
            "<row r=\"2\" ht=\"15\" customHeight=\"1\"><c r=\"B2\" s=\"3\" t=\"inlineStr\"><is>"
            "<t xml:space=\"preserve\">two</t></is></c><c r=\"C2\"><v>2</v></c><c r=\"D2\" s=\"4\"/></row>"
            "<row r=\"4\">" + text_cell("A4", "four") + "</row>"
            "<row r=\"6\">" + text_cell("A6", "six") + "</row>"
            "<row r=\"7\"><c r=\"E7\"><v>7</v></c></row>"
            "<row r=\"9\"><c r=\"A9\"><v>9</v></c></row>"
            "</sheetData>");
}

TEST_F(Ooxml, ASecondWriteGoesOnFromTheFirst) {
  Workbook book = open(zip_file("in.xlsx", small_workbook("<sheetData/>")));
  ASSERT_TRUE(book.write_rows("Data", 2, {{Cell{"a"}}}).has_value());
  ASSERT_TRUE(book.write_rows("Data", 3, {{Cell{"b"}}}).has_value());
  const auto again = book.write_rows("Data", 3, {{Cell{}, Cell{"c"}}});
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(book.row_values("Data", 2).value(), (std::vector<std::string>{"a"}));
  EXPECT_EQ(book.row_values("Data", 3).value(), (std::vector<std::string>{"b"}));
}

// ---- Refusals --------------------------------------------------------------

TEST_F(Ooxml, RefusesARowThatIsNotEmpty) {
  Workbook book = open(template_file(kArAr));
  // Row 4 is the field-name row; the second of these rows would land on it.
  const auto header = book.write_rows("ArArMeasurement", 3, {{}, {Cell{"x"}}});
  ASSERT_FALSE(header.has_value());
  EXPECT_EQ(header.error().kind, ErrorKind::Config);
  EXPECT_NE(header.error().what.find("ArArMeasurement"), std::string::npos) << header.error().what;
  EXPECT_NE(header.error().what.find("A4"), std::string::npos) << header.error().what;

  // Nothing was written, not even the rows before the one refused.
  const auto partial = book.write_rows("ArArMeasurement", 862, {{Cell{"kept out"}}, {Cell{"x"}}, {Cell{"y"}}});
  ASSERT_TRUE(partial.has_value());
  const auto second = book.write_rows("ArArMeasurement", 860, {{Cell{"a"}}, {Cell{"b"}}, {Cell{"c"}}});
  ASSERT_FALSE(second.has_value());
  EXPECT_NE(second.error().what.find("A862"), std::string::npos) << second.error().what;
  EXPECT_TRUE(book.row_values("ArArMeasurement", 860).value().empty());

  // A formula or an inline string counts as a value; a styled cell does not.
  const std::string body = "<sheetData><row r=\"2\"><c r=\"A2\" s=\"1\"/><c r=\"C2\"><f>1+1</f></c></row>"
                           "<row r=\"3\"><c r=\"B3\" t=\"inlineStr\"><is><t>x</t></is></c></row>"
                           "<row r=\"4\"><c r=\"A4\" s=\"1\"/></row></sheetData>";
  Workbook small = open(zip_file("in.xlsx", small_workbook(body)));
  const auto formula = small.write_rows("Data", 2, {{Cell{"x"}}});
  ASSERT_FALSE(formula.has_value());
  EXPECT_NE(formula.error().what.find("C2"), std::string::npos) << formula.error().what;
  const auto inline_text = small.write_rows("Data", 3, {{Cell{"x"}}});
  ASSERT_FALSE(inline_text.has_value());
  EXPECT_NE(inline_text.error().what.find("B3"), std::string::npos) << inline_text.error().what;
  EXPECT_TRUE(small.write_rows("Data", 4, {{Cell{"x"}}}).has_value());
}

TEST_F(Ooxml, RefusesAControlCharacter) {
  Workbook book = open(template_file(kArAr));
  const std::vector<std::string> forbidden = {
      std::string("bell\x07"),       // a C0 control
      std::string("nul\0l", 5),      // NUL
      "\xEF\xBF\xBE",                // U+FFFE
      "\xEF\xBF\xBF",                // U+FFFF
      "\xED\xA0\x80",                // a surrogate, encoded
      "caf\xE9",                     // Latin-1, not UTF-8
      "\xC0\xAF",                    // overlong
      "\xF4\x90\x80\x80",            // past U+10FFFF
      "cut\xE2\x82",                 // truncated sequence
  };
  for (const std::string& text : forbidden) {
    const auto wrote = book.write_rows("ArArMeasurement", 5, {{Cell{"fine"}}, {Cell{}, Cell{text}}});
    ASSERT_FALSE(wrote.has_value()) << text;
    EXPECT_EQ(wrote.error().kind, ErrorKind::Config);
    EXPECT_NE(wrote.error().what.find("ArArMeasurement"), std::string::npos) << wrote.error().what;
    EXPECT_NE(wrote.error().what.find("B6"), std::string::npos) << wrote.error().what;
  }
  EXPECT_TRUE(book.row_values("ArArMeasurement", 5).value().empty());
  // What XML allows goes through: tab, line feed, return, astral characters.
  EXPECT_TRUE(book.write_rows("ArArMeasurement", 5, {{Cell{"\t\n\r \xF0\x9F\x9C\x82 \xEF\xBF\xBD"}}}).has_value());
}

TEST_F(Ooxml, RefusesANumberThatIsNotFinite) {
  Workbook book = open(template_file(kArAr));
  for (const double number : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity()}) {
    const auto wrote = book.write_rows("ArArAliquot", 5, {{Cell{"dp"}, Cell{}, Cell{number}}});
    ASSERT_FALSE(wrote.has_value());
    EXPECT_EQ(wrote.error().kind, ErrorKind::Config);
    EXPECT_NE(wrote.error().what.find("ArArAliquot"), std::string::npos) << wrote.error().what;
    EXPECT_NE(wrote.error().what.find("C5"), std::string::npos) << wrote.error().what;
  }
}

TEST_F(Ooxml, RefusesAnUnknownSheet) {
  Workbook book = open(template_file(kArAr));
  const auto read = book.row_values("ArArMeasurements", 4);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error().kind, ErrorKind::Config);
  EXPECT_NE(read.error().what.find("ArArMeasurements"), std::string::npos);
  const auto wrote = book.write_rows("arardatapoints", 5, {{Cell{"x"}}});  // names are matched exactly
  ASSERT_FALSE(wrote.has_value());
  EXPECT_EQ(wrote.error().kind, ErrorKind::Config);
}

TEST_F(Ooxml, RefusesARowOrAColumnOutsideTheSheet) {
  Workbook book = open(template_file(kArAr));
  EXPECT_FALSE(book.row_values("ArArMeasurement", 0).has_value());
  EXPECT_FALSE(book.row_values("ArArMeasurement", -3).has_value());
  EXPECT_FALSE(book.row_values("ArArMeasurement", kMaxRow + 1).has_value());
  EXPECT_FALSE(book.write_rows("ArArMeasurement", 1, {{Cell{"x"}}}).has_value());  // row 1 is a header
  EXPECT_FALSE(book.write_rows("ArArMeasurement", 0, {{Cell{"x"}}}).has_value());
  EXPECT_FALSE(book.write_rows("ArArMeasurement", kMaxRow, {{Cell{"x"}}, {Cell{"y"}}}).has_value());
  EXPECT_FALSE(book.write_rows("ArArMeasurement", 5, {std::vector<Cell>(kMaxColumn + 1, Cell{1.0})}).has_value());
  EXPECT_TRUE(book.write_rows("ArArMeasurement", 5, {}).has_value());  // nothing to write
  EXPECT_TRUE(book.write_rows("ArArMeasurement", kMaxRow, {{Cell{"last"}}}).has_value());
}

// ---- Files that are not workbooks -----------------------------------------

TEST_F(Ooxml, AFileThatIsNotAWorkbookIsAnError) {
  const auto missing = Workbook::open(file("nothing.xlsx"));
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().kind, ErrorKind::Io);
  EXPECT_FALSE(read_zip(file("nothing.xlsx")).has_value());
  EXPECT_FALSE(Workbook::open(dir_).has_value());  // a directory

  spit(file("text.xlsx"), "datapointName,sampleName\n");
  const auto text = Workbook::open(file("text.xlsx"));
  ASSERT_FALSE(text.has_value());
  EXPECT_EQ(text.error().kind, ErrorKind::Protocol);
  EXPECT_NE(text.error().what.find("text.xlsx"), std::string::npos) << text.error().what;
  EXPECT_FALSE(read_zip(file("text.xlsx")).has_value());

  spit(file("empty.xlsx"), "");
  EXPECT_FALSE(Workbook::open(file("empty.xlsx")).has_value());

  // The template, cut short at several places.
  const std::string whole = slurp(template_file(kArAr));
  for (const std::size_t keep :
       {std::size_t{4}, std::size_t{100}, whole.size() / 2, whole.size() - 30, whole.size() - 1}) {
    spit(file("cut.xlsx"), std::string_view(whole).substr(0, keep));
    const auto cut = Workbook::open(file("cut.xlsx"));
    ASSERT_FALSE(cut.has_value()) << keep;
    EXPECT_EQ(cut.error().kind, ErrorKind::Protocol);
  }

  // A zip, but of something else.
  const auto other = Workbook::open(zip_file("other.xlsx", {{"readme.txt", "hello"}}));
  ASSERT_FALSE(other.has_value());
  EXPECT_EQ(other.error().kind, ErrorKind::Protocol);
  EXPECT_NE(other.error().what.find("xl/workbook.xml"), std::string::npos) << other.error().what;
  EXPECT_EQ(entries(file("other.xlsx")).size(), 1u);
}

TEST_F(Ooxml, ADamagedTemplateIsAnError) {
  // A byte of a sheet's compressed data changed: found when the sheet is read.
  std::string bytes = slurp(template_file(kArAr));
  const std::size_t name = bytes.find("xl/worksheets/sheet2.xml");
  ASSERT_NE(name, std::string::npos);
  bytes[name + 24 + 200] = static_cast<char>(bytes[name + 24 + 200] ^ 0x5A);
  spit(file("flipped.xlsx"), bytes);
  auto flipped = Workbook::open(file("flipped.xlsx"));
  ASSERT_TRUE(flipped.has_value()) << to_string(flipped.error());
  EXPECT_FALSE(flipped->row_values("ArArMeasurement", 4).has_value());
  EXPECT_FALSE(flipped->write_rows("ArArMeasurement", 5, {{Cell{"x"}}}).has_value());
  EXPECT_TRUE(flipped->row_values("ArArAliquot", 4).has_value());
  EXPECT_FALSE(read_zip(file("flipped.xlsx")).has_value());
}

TEST_F(Ooxml, AWorkbookThatDoesNotHoldTogetherIsAnError) {
  const auto with = [&](std::size_t index, const std::string& data) {
    std::vector<Part> parts = small_workbook("<sheetData/>");
    parts[index].data = data;
    return Workbook::open(zip_file("bad.xlsx", parts));
  };
  const auto expect_protocol = [](const Result<Workbook>& book) {
    ASSERT_FALSE(book.has_value());
    EXPECT_EQ(book.error().kind, ErrorKind::Protocol);
  };
  expect_protocol(with(1, "<workbook><sheets><sheet name=\"Data\""));              // cut short
  expect_protocol(with(1, "not xml at all"));
  expect_protocol(with(1, "<workbook/>"));                                         // no sheets
  expect_protocol(with(1, "<notAWorkbook><sheets/></notAWorkbook>"));
  expect_protocol(with(1, R"(<workbook><sheets><sheet name="Data" sheetId="1"/></sheets></workbook>)"));  // no r:id
  expect_protocol(with(2, "<Relationships/>"));                                    // the id leads nowhere
  expect_protocol(with(2, R"(<Relationships><Relationship Id="rId7" Target="worksheets/gone.xml"/></Relationships>)"));
  expect_protocol(with(2, R"(<Relationships><Relationship Id="rId7" Target="../../../etc/passwd"/></Relationships>)"));

  // No relationships part at all.
  std::vector<Part> parts = small_workbook("<sheetData/>");
  parts.erase(parts.begin() + 2);
  expect_protocol(Workbook::open(zip_file("norels.xlsx", parts)));

  // A sheet that is not a sheet: the workbook opens, the sheet does not read.
  for (const std::string sheet : {"<worksheet><sheetData><row r=\"1\">", "", "<worksheet/>", "<chartsheet/>",
                                  R"(<worksheet><sheetData><row r="2"/><row r="2"/></sheetData></worksheet>)",
                                  "<worksheet><sheetData><row/></sheetData></worksheet>",
                                  // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): one sheet, over two lines
                                  "<worksheet><sheetData><row r=\"2\"><c r=\"B2\"/><c r=\"A2\"/></row></sheetData>"
                                  "</worksheet>",
                                  R"(<worksheet><sheetData><row r="2"><c r="A3"/></row></sheetData></worksheet>)",
                                  R"(<worksheet><sheetData><row r="2"><c r="2A"/></row></sheetData></worksheet>)",
                                  "<worksheet><sheetData><row r=\"2\"><x/></row></sheetData></worksheet>"}) {
    SCOPED_TRACE(sheet);
    auto book = with(3, sheet);
    ASSERT_TRUE(book.has_value());
    const auto wrote = book->write_rows("Data", 2, {{Cell{"x"}}});
    ASSERT_FALSE(wrote.has_value());
    EXPECT_EQ(wrote.error().kind, ErrorKind::Protocol);
    EXPECT_TRUE(book->save(file("still.xlsx")).has_value());
  }
}

TEST_F(Ooxml, AnEntryThatClaimsAHugeSizeIsRefusedNotAllocated) {
  // The workbook part says it unpacks to 4 GB.
  std::vector<Part> parts = small_workbook("<sheetData/>");
  parts[1].claimed_size = 0xFFFFFFF0u;
  const auto book = Workbook::open(zip_file("huge.xlsx", parts));
  ASSERT_FALSE(book.has_value());
  EXPECT_EQ(book.error().kind, ErrorKind::Protocol);
  EXPECT_NE(book.error().what.find("xl/workbook.xml"), std::string::npos) << book.error().what;
  EXPECT_FALSE(read_zip(file("huge.xlsx")).has_value());

  // A sheet that does: the workbook opens, the sheet is refused when read.
  parts = small_workbook("<sheetData/>");
  parts[3].claimed_size = 0x7FFFFFFFu;
  auto lazy = Workbook::open(zip_file("huge-sheet.xlsx", parts));
  ASSERT_TRUE(lazy.has_value()) << to_string(lazy.error());
  EXPECT_FALSE(lazy->row_values("Data", 1).has_value());
  EXPECT_FALSE(lazy->write_rows("Data", 2, {{Cell{"x"}}}).has_value());

  // And one whose stated size is merely wrong, either way.
  for (const std::uint32_t claimed : {std::uint32_t{10}, static_cast<std::uint32_t>(parts[3].data.size() + 100)}) {
    parts = small_workbook("<sheetData/>");
    parts[3].claimed_size = claimed;
    auto wrong = Workbook::open(zip_file("wrong.xlsx", parts));
    ASSERT_TRUE(wrong.has_value()) << to_string(wrong.error());
    EXPECT_FALSE(wrong->row_values("Data", 1).has_value());
    EXPECT_FALSE(wrong->write_rows("Data", 2, {{Cell{"x"}}}).has_value());
  }
  // The honest size, written the same way, reads: the lie is what is refused.
  parts = small_workbook("<sheetData/>");
  parts[3].claimed_size = static_cast<std::uint32_t>(parts[3].data.size());
  auto honest = Workbook::open(zip_file("honest.xlsx", parts));
  ASSERT_TRUE(honest.has_value()) << to_string(honest.error());
  EXPECT_TRUE(honest->row_values("Data", 1).has_value());
}

TEST_F(Ooxml, SaveSaysWhenItCannotWrite) {
  const Workbook book = open(template_file(kArAr));
  const auto saved = book.save(dir_ / "no-such-directory" / "out.xlsx");
  ASSERT_FALSE(saved.has_value());
  EXPECT_EQ(saved.error().kind, ErrorKind::Io);
}

// ---- Review round 1 --------------------------------------------------------

TEST_F(Ooxml, ARewrittenEntryIsFlaggedAsTheTemplatesEntriesAre) {
  // General-purpose flags. The Ar-Ar template's entries are 0x0006: no data
  // descriptor (bit 3), names not flagged UTF-8 (bit 11); bits 1 and 2 only
  // say how hard the writer compressed, which miniz states as 0 (normal) and
  // offers no way to set. The Sample template's are 0x0808: a descriptor and
  // UTF-8 names. A rewritten sheet's entry follows the template's in both
  // bits, in its local header and in the directory.
  const unsigned kHints = 0x0006;
  struct Case {
    const char* file;
    const char* sheet;
    const char* part;
    unsigned flags;
  };
  const auto cases = std::to_array<Case>({{kArAr, "ArArMeasurement", "xl/worksheets/sheet2.xml", 0x0006},
                                          {kSample, "Samples", "xl/worksheets/sheet1.xml", 0x0808}});
  for (const auto& each : cases) {
    SCOPED_TRACE(each.file);
    Workbook book = open(template_file(each.file));
    ASSERT_TRUE(book.write_rows(each.sheet, 5, {{Cell{"x"}, Cell{1.0}}}).has_value());
    ASSERT_TRUE(book.save(file("out.xlsx")).has_value());
    const auto before = stamps(slurp(template_file(each.file)));
    const auto after = stamps(slurp(file("out.xlsx")));
    ASSERT_EQ(after.size(), before.size());
    bool seen = false;
    for (std::size_t i = 0; i < before.size(); ++i) {
      SCOPED_TRACE(before[i].name);
      EXPECT_EQ(before[i].flags, each.flags);
      EXPECT_EQ(before[i].local_flags, each.flags);
      if (before[i].name == each.part) {
        seen = true;
        EXPECT_EQ(after[i].flags, each.flags & ~kHints);
        EXPECT_EQ(after[i].local_flags, each.flags & ~kHints);
      } else {
        EXPECT_EQ(after[i].flags, before[i].flags);
        EXPECT_EQ(after[i].local_flags, before[i].local_flags);
      }
    }
    EXPECT_TRUE(seen);
    EXPECT_EQ(open(file("out.xlsx")).row_values(each.sheet, 5).value(), (std::vector<std::string>{"x", "1"}));
  }
}

TEST_F(Ooxml, ARowOrCellNumberedPastTheSheetIsAnError) {
  const auto read = [&](const std::string& body, int row) {
    return open(zip_file("limits.xlsx", small_workbook(body))).row_values("Data", row);
  };
  const auto expect_protocol = [](const Result<std::vector<std::string>>& values) {
    ASSERT_FALSE(values.has_value());
    EXPECT_EQ(values.error().kind, ErrorKind::Protocol);
  };
  // The row after the last an int can number; after the last of a sheet.
  expect_protocol(read("<sheetData><row r=\"2147483647\"/><row><c><v>1</v></c></row></sheetData>", 5));
  expect_protocol(read("<sheetData><row r=\"1048576\"/><row><c><v>1</v></c></row></sheetData>", 5));
  expect_protocol(read("<sheetData><row r=\"1048577\"/></sheetData>", 5));
  expect_protocol(read("<sheetData><row r=\"0\"/></sheetData>", 5));
  expect_protocol(read("<sheetData><row r=\"-4\"/></sheetData>", 5));
  expect_protocol(read("<sheetData><row r=\"99999999999999999999\"/></sheetData>", 5));
  expect_protocol(read("<sheetData><row r=\"five\"/></sheetData>", 5));
  // The cell after XFD; a reference that is none.
  expect_protocol(read(R"(<sheetData><row r="5"><c r="XFD5"/><c><v>1</v></c></row></sheetData>)", 5));
  expect_protocol(read(R"(<sheetData><row r="5"><c r="XFE5"><v>1</v></c></row></sheetData>)", 5));
  expect_protocol(read(R"(<sheetData><row r="5"><c r="AAAA5"><v>1</v></c></row></sheetData>)", 5));
  expect_protocol(read(R"(<sheetData><row r="5"><c r="5A"><v>1</v></c></row></sheetData>)", 5));
  expect_protocol(read(R"(<sheetData><row r="5"><c r="a5"><v>1</v></c></row></sheetData>)", 5));
  expect_protocol(read(R"(<sheetData><row r="5"><c r="A"><v>1</v></c></row></sheetData>)", 5));
  expect_protocol(read(R"(<sheetData><row r="5"><c r="A0"><v>1</v></c></row></sheetData>)", 5));
  expect_protocol(read(R"(<sheetData><row r="5"><c r="A99999999999"><v>1</v></c></row></sheetData>)", 5));
  expect_protocol(read(R"(<sheetData><row r="5"><c r=""><v>1</v></c></row></sheetData>)", 5));

  // Rows and cells that number themselves by position are read.
  const auto counted = read("<sheetData><row r=\"3\"/><row/><row><c><v>1</v></c><c r=\"D5\"><v>4</v></c><c><v>5</v></c>"
                            "</row></sheetData>", 5);
  ASSERT_TRUE(counted.has_value()) << to_string(counted.error());
  EXPECT_EQ(*counted, (std::vector<std::string>{"1", "", "", "4", "5"}));
  const auto last = read(R"(<sheetData><row r="1048576"><c r="XFD1048576"><v>9</v></c></row></sheetData>)", kMaxRow);
  ASSERT_TRUE(last.has_value()) << to_string(last.error());
  ASSERT_EQ(last->size(), static_cast<std::size_t>(kMaxColumn));
  EXPECT_EQ(last->back(), "9");
}

TEST_F(Ooxml, ASharedStringTheWorkbookHasNotIsAnError) {
  const std::string strings = "<sst><si><t>only</t></si></sst>";
  const auto read = [&](const std::string& index) {
    const std::string body = R"(<sheetData><row r="1"><c r="A1" t="s"><v>)" + index + "</v></c></row></sheetData>";
    return open(zip_file("strings.xlsx", small_workbook(body, strings))).row_values("Data", 1);
  };
  EXPECT_EQ(read("0").value(), (std::vector<std::string>{"only"}));
  for (const char* index : {"1", "-1", "4294967296", "99999999999999999999", "zero", "", "0.5"}) {
    const auto values = read(index);
    ASSERT_FALSE(values.has_value()) << index;
    EXPECT_EQ(values.error().kind, ErrorKind::Protocol);
    EXPECT_NE(values.error().what.find("A1"), std::string::npos) << values.error().what;
  }
  // And with no shared strings at all.
  const std::string body = R"(<sheetData><row r="1"><c r="A1" t="s"><v>0</v></c></row></sheetData>)";
  EXPECT_FALSE(open(zip_file("none.xlsx", small_workbook(body))).row_values("Data", 1).has_value());
}

TEST_F(Ooxml, ARelationshipTargetMayBeAbsoluteOrClimb) {
  const std::string body = R"(<sheetData><row r="1"><c r="A1"><v>7</v></c></row></sheetData>)";
  const auto with_target = [&](const std::string& target) {
    std::vector<Part> parts = small_workbook(body);
    parts[2].data = R"(<Relationships><Relationship Id="rId7" Target=")" + target + "\"/></Relationships>";
    return Workbook::open(zip_file("target.xlsx", parts));
  };
  for (const char* target : {"/xl/worksheets/data.xml", "../xl/worksheets/data.xml", "./worksheets/data.xml",
                             "worksheets/../worksheets/data.xml", "worksheets//data.xml"}) {
    SCOPED_TRACE(target);
    auto book = with_target(target);
    ASSERT_TRUE(book.has_value()) << to_string(book.error());
    EXPECT_EQ(book->row_values("Data", 1).value(), (std::vector<std::string>{"7"}));
  }
  // Out of the archive, or to nothing in it.
  for (const char* target : {"../../xl/worksheets/data.xml", "/../xl/worksheets/data.xml", "/worksheets/data.xml",
                             "../worksheets/data.xml", "", "/", R"(C:\xl\worksheets\data.xml)",
                             "file:///etc/passwd"}) {
    SCOPED_TRACE(target);
    const auto book = with_target(target);
    ASSERT_FALSE(book.has_value());
    EXPECT_EQ(book.error().kind, ErrorKind::Protocol);
  }
}

TEST_F(Ooxml, AnArchiveWithTooManyEntriesOrANameTwiceIsAnError) {
  std::vector<Part> parts = small_workbook("<sheetData/>");
  while (parts.size() <= kMaxEntries) parts.push_back({"filler/" + std::to_string(parts.size()) + ".xml", "<x/>"});
  ASSERT_EQ(parts.size(), kMaxEntries + 1);
  const auto crowded = Workbook::open(zip_file("crowded.xlsx", parts));
  ASSERT_FALSE(crowded.has_value());
  EXPECT_EQ(crowded.error().kind, ErrorKind::Protocol);
  EXPECT_NE(crowded.error().what.find("entries"), std::string::npos) << crowded.error().what;
  EXPECT_FALSE(read_zip(file("crowded.xlsx")).has_value());
  parts.pop_back();
  EXPECT_TRUE(Workbook::open(zip_file("full.xlsx", parts)).has_value());

  // Two entries of one name: which is the sheet depends on who reads it.
  parts = small_workbook(R"(<sheetData><row r="1"><c r="A1"><v>1</v></c></row></sheetData>)");
  parts.push_back({"xl/worksheets/data.xml", std::string(kSheetHead) + "<sheetData/></worksheet>"});
  const auto twice = Workbook::open(zip_file("twice.xlsx", parts));
  ASSERT_FALSE(twice.has_value());
  EXPECT_EQ(twice.error().kind, ErrorKind::Protocol);
  EXPECT_NE(twice.error().what.find("xl/worksheets/data.xml"), std::string::npos) << twice.error().what;
}

TEST_F(Ooxml, APartJustOverTheLimitIsRefused) {
  EXPECT_EQ(kMaxPartBytes, std::size_t{32} << 20);
  std::vector<Part> parts = small_workbook("<sheetData/>");
  parts[3].claimed_size = static_cast<std::uint32_t>(kMaxPartBytes + 1);
  auto over = Workbook::open(zip_file("over.xlsx", parts));
  ASSERT_TRUE(over.has_value()) << to_string(over.error());
  const auto read = over->row_values("Data", 1);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error().kind, ErrorKind::Protocol);
  EXPECT_NE(read.error().what.find("more than a workbook part can have"), std::string::npos) << read.error().what;
  // At the limit the claim is believed, and found false when unpacked.
  parts[3].claimed_size = static_cast<std::uint32_t>(kMaxPartBytes);
  auto at = Workbook::open(zip_file("at.xlsx", parts));
  ASSERT_TRUE(at.has_value());
  const auto unpacked = at->row_values("Data", 1);
  ASSERT_FALSE(unpacked.has_value());
  EXPECT_EQ(unpacked.error().what.find("more than a workbook part can have"), std::string::npos)
      << unpacked.error().what;
}

TEST_F(Ooxml, MarkupHiddenInACommentIsNotWrittenInto) {
  const std::string row1 = R"(<row r="1"><c r="A1"><v>1</v></c></row>)";
  // Before the sheet's data: no matter.
  EXPECT_EQ(written("<!-- </sheetData> <row r=\"2\"/> --><sheetData>" + row1 + "</sheetData>", 2, {{Cell{"a"}}}),
            "<!-- </sheetData> <row r=\"2\"/> --><sheetData>" + row1 + "<row r=\"2\">" + text_cell("A2", "a") +
                "</row></sheetData>");

  // Inside it, where the end of the data or of a row would be looked for:
  // refused, never a file with the rows in the comment.
  for (const std::string& body :
       {"<sheetData>" + row1 + "<!-- </sheetData> --></sheetData>",
        "<sheetData><!-- </sheetData> -->" + row1 + "</sheetData>",
        "<sheetData>" + row1 + "<![CDATA[</sheetData>]]></sheetData>",
        "<sheetData>" + row1 + R"(<row r="2"><!-- </row> --><c r="A2" s="1"/></row></sheetData>)",
        "<sheetData>" + row1 + R"(<row r="2"><c r="A2" s="1"><!-- </c> --></c><c r="B2"/></row></sheetData>)",
        "<sheetData>" + row1 + R"(<row r="2"><c r="A2" s="1"/><!-- </row><row r="2"> --></row></sheetData>)"}) {
    SCOPED_TRACE(body);
    Workbook book = open(zip_file("comment.xlsx", small_workbook(body)));
    const auto wrote = book.write_rows("Data", 2, {{Cell{}, Cell{"b"}}});
    if (wrote.has_value()) {
      // Handled, then: the value is where it was asked to be, and row 1 too.
      ASSERT_TRUE(book.save(file("out.xlsx")).has_value());
      const Workbook again = open(file("out.xlsx"));
      EXPECT_EQ(again.row_values("Data", 2).value(), (std::vector<std::string>{"", "b"}));
      EXPECT_EQ(again.row_values("Data", 1).value(), (std::vector<std::string>{"1"}));
    } else {
      EXPECT_EQ(wrote.error().kind, ErrorKind::Protocol);
      EXPECT_NE(wrote.error().what.find("Data"), std::string::npos) << wrote.error().what;
      EXPECT_TRUE(book.row_values("Data", 2).value().empty());
    }
  }
  // The plain case of the first of those is refused, to be exact about it.
  const std::string hidden = "<sheetData>" + row1 + "<!-- </sheetData> --></sheetData>";
  Workbook book = open(zip_file("comment.xlsx", small_workbook(hidden)));
  EXPECT_FALSE(book.write_rows("Data", 2, {{Cell{"b"}}}).has_value());
}

TEST_F(Ooxml, AFailedSaveLeavesNoFile) {
  Workbook book = open(template_file(kArAr));
  ASSERT_TRUE(book.write_rows("ArArAliquot", 5, {{Cell{"x"}}}).has_value());

  const fs::path missing = dir_ / "no-such-directory";
  EXPECT_FALSE(book.save(missing / "out.xlsx").has_value());
  EXPECT_FALSE(fs::exists(missing));

  // A good save leaves the workbook and nothing else.
  ASSERT_TRUE(book.save(file("out.xlsx")).has_value());
  ASSERT_TRUE(book.save(file("out.xlsx")).has_value());  // and replaces it
  std::size_t count = 0;
  for (const auto& entry : fs::directory_iterator(dir_)) {
    ++count;
    EXPECT_EQ(entry.path().filename(), "out.xlsx");
  }
  EXPECT_EQ(count, 1u);

  // The destination is a directory: refused, the directory untouched.
  fs::create_directories(dir_ / "taken.xlsx" / "inside");
  EXPECT_FALSE(book.save(dir_ / "taken.xlsx").has_value());
  EXPECT_TRUE(fs::is_directory(dir_ / "taken.xlsx" / "inside"));
  count = 0;
  for (const auto& entry : fs::directory_iterator(dir_)) {
    ++count;
    EXPECT_TRUE(entry.path().filename() == "out.xlsx" || entry.path().filename() == "taken.xlsx") << entry.path();
  }
  EXPECT_EQ(count, 2u);

#ifndef _WIN32
  // A directory that cannot be written to: the workbook already there stays
  // as it was, and nothing is left beside it. (Root writes anywhere.)
  if (geteuid() != 0) {
    const fs::path locked = dir_ / "locked";
    fs::create_directories(locked);
    spit(locked / "out.xlsx", "the old workbook");
    ASSERT_EQ(chmod(locked.c_str(), 0555), 0);
    const auto saved = book.save(locked / "out.xlsx");
    count = 0;
    for (const auto& entry : fs::directory_iterator(locked)) {
      ++count;
      EXPECT_EQ(entry.path().filename(), "out.xlsx");
    }
    ASSERT_EQ(chmod(locked.c_str(), 0755), 0);
    ASSERT_FALSE(saved.has_value());
    EXPECT_EQ(saved.error().kind, ErrorKind::Io);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(slurp(locked / "out.xlsx"), "the old workbook");
  }
#endif
}

// ---- What the templates say about their columns ---------------------------------

// A list validation as the tests write one: the columns, then the source.
struct Listed {
  std::vector<int> columns;
  std::string sheet;
  int column = 0;
  int first_row = 0;
  int last_row = 0;
  bool operator==(const Listed&) const = default;
};

std::vector<Listed> ranges_of(const Workbook& book, const std::string& sheet) {
  const auto found = book.list_validations(sheet);
  EXPECT_TRUE(found.has_value()) << (found ? "" : to_string(found.error()));
  std::vector<Listed> out;
  if (!found) return out;
  for (const ListValidation& one : *found) {
    EXPECT_EQ(one.source, ListValidation::Source::Range) << sheet << ": " << one.formula;
    out.push_back({one.columns, one.sheet, one.column, one.first_row, one.last_row});
  }
  std::sort(out.begin(), out.end(), [](const Listed& a, const Listed& b) { return a.columns < b.columns; });
  return out;
}

TEST_F(Ooxml, TheTemplatesListValidations) {
  // The Ar-Ar template keeps them in the sheet's extension list
  // (x14:dataValidation, with xm:f and xm:sqref) and for row 5 only; the
  // Sample template in the sheet's own dataValidations, for rows 5 to 883.
  // Either way: these columns, those lists.
  const Workbook arar = open(template_file(kArAr));
  const char* const lists = "Lookup Tables";
  EXPECT_EQ(ranges_of(arar, "ArArDataPoints"),
            (std::vector<Listed>{{{4}, lists, 4, 2, 13},             // arArMethod: Analytical Method
                                 {{8}, lists, 5, 2, 8},              // analysisScale: Analysis Scale
                                 {{14, 16, 18}, lists, 6, 2, 21},    // the three uncertainty types
                                 {{15, 17, 19}, lists, 7, 2, 3}}));  // the three uncertainty units
  EXPECT_EQ(ranges_of(arar, "ArArMeasurement"), (std::vector<Listed>{{{55, 56, 57}, lists, 9, 2, 4}}));  // Yes/No
  EXPECT_EQ(ranges_of(arar, "ArArAliquot"), std::vector<Listed>{});
  EXPECT_EQ(ranges_of(arar, "ArArAgeSummary"),
            (std::vector<Listed>{{{2}, lists, 10, 2, 11},     // dataInterpretationTool: Data Interpretation
                                 {{4}, lists, 8, 2, 9},       // ageCalcType: Age Type
                                 {{7}, lists, 6, 2, 21},      // calculatedAgeUncertaintyType
                                 {{8}, lists, 7, 2, 3},       // calculatedAgeUncertaintyTypeUnits
                                 {{12}, lists, 11, 2, 20}}));  // interpretation: Age Interpretation
  EXPECT_EQ(ranges_of(arar, "ArArAgeCalc"), (std::vector<Listed>{{{2}, lists, 1, 2, 5},      // decayConstant
                                                                 {{3}, lists, 2, 2, 3},      // airRatio
                                                                 {{4}, lists, 3, 2, 17}}));  // fluxMonitor
  EXPECT_EQ(ranges_of(arar, "Field Definitions"), std::vector<Listed>{});
  EXPECT_EQ(ranges_of(arar, "Datapoint Props"), std::vector<Listed>{});

  const Workbook sample = open(template_file(kSample));
  const char* const tables = "Lookup tables";
  EXPECT_EQ(ranges_of(sample, "Samples"),
            (std::vector<Listed>{{{3}, tables, 1, 2, 16},       // sampleKind: Sample Kind
                                 {{4}, tables, 2, 2, 12},       // sampleMethod: Sampling Method
                                 {{5}, tables, 3, 2, 10185},    // material: Lithology
                                 {{15}, tables, 4, 2, 10},      // locationKind: Location Kind
                                 {{25}, tables, 5, 2, 12},      // personRole: Person Role
                                 {{26}, tables, 6, 2, 28}}));   // lastKnownArchive: Last Known Sample Archive
  EXPECT_EQ(ranges_of(sample, "Sample Props"), std::vector<Listed>{});

  const auto none = arar.list_validations("No Such Sheet");
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().kind, ErrorKind::Config);
}

// A one-sheet workbook whose sheet has `validations` after its (empty) data
// and whose workbook part has `defined_names`.
std::vector<Part> validated_workbook(const std::string& validations, const std::string& defined_names = {}) {
  std::vector<Part> parts = small_workbook("<sheetData/>" + validations);
  const std::string close = "</workbook>";
  parts[1].data.insert(parts[1].data.size() - close.size(), defined_names);
  return parts;
}

TEST_F(Ooxml, AListValidationInAnyOfItsForms) {
  const std::string validations =
      "<dataValidations count=\"9\">"
      // Several areas, one of them of two columns; the list is on this sheet.
      "<dataValidation type=\"list\" sqref=\"C5 A2:A9 E5:F6\"><formula1>$K$2:$K$7</formula1></dataValidation>"
      // A sheet whose name needs quotes, an apostrophe in it, and no `$`.
      "<dataValidation type=\"list\" sqref=\"B5\"><formula1>'Bob''s lists'!C3:C4</formula1></dataValidation>"
      // A bare sheet name and one cell.
      "<dataValidation type=\"list\" sqref=\"G5:G10\"><formula1>Lists!$D$9</formula1></dataValidation>"
      // The entries themselves.
      "<dataValidation type=\"list\" sqref=\"H5\"><formula1>\"Yes, No ,N/A,,say \"\"so\"\"\"</formula1></dataValidation>"
      // A name the workbook defines, and one it does not.
      "<dataValidation type=\"list\" sqref=\"I5\"><formula1>Monitors</formula1></dataValidation>"
      "<dataValidation type=\"list\" sqref=\"J5\"><formula1>Nobody</formula1></dataValidation>"
      // A list this does not follow: a block of two columns, a formula.
      "<dataValidation type=\"list\" sqref=\"L5\"><formula1>Lists!$A$1:$B$4</formula1></dataValidation>"
      "<dataValidation type=\"list\" sqref=\"M5\"><formula1>INDIRECT(\"x\")</formula1></dataValidation>"
      // Not a list at all.
      "<dataValidation type=\"whole\" sqref=\"N5\"><formula1>1</formula1><formula2>9</formula2></dataValidation>"
      "</dataValidations>"
      // And one in the extension list, as Excel 2010 and later write those
      // whose list is on another sheet.
      "<extLst><ext xmlns:x14=\"http://schemas.microsoft.com/office/spreadsheetml/2009/9/main\">"
      "<x14:dataValidations xmlns:xm=\"http://schemas.microsoft.com/office/excel/2006/main\">"
      "<x14:dataValidation type=\"list\"><x14:formula1><xm:f>Lists!$E$2:$E$3</xm:f></x14:formula1>"
      "<xm:sqref>AA5 AC5:AD9</xm:sqref></x14:dataValidation>"
      "</x14:dataValidations></ext></extLst>";
  const std::string names = "<definedNames><definedName name=\"Monitors\">'Lookup Tables'!$C$2:$C$17</definedName>"
                            "</definedNames>";
  const Workbook book = open(zip_file("validated.xlsx", validated_workbook(validations, names)));
  const auto found = book.list_validations("Data");
  ASSERT_TRUE(found.has_value()) << to_string(found.error());
  ASSERT_EQ(found->size(), 9u);
  using Source = ListValidation::Source;
  const auto expect_range = [&](std::size_t i, const std::vector<int>& columns, const std::string& sheet, int column,
                                int first_row, int last_row) {
    const ListValidation& one = (*found)[i];
    EXPECT_EQ(one.source, Source::Range) << i;
    EXPECT_EQ(one.columns, columns) << i;
    EXPECT_EQ(one.sheet, sheet) << i;
    EXPECT_EQ(one.column, column) << i;
    EXPECT_EQ(one.first_row, first_row) << i;
    EXPECT_EQ(one.last_row, last_row) << i;
  };
  expect_range(0, {1, 3, 5, 6}, "Data", 11, 2, 7);
  expect_range(1, {2}, "Bob's lists", 3, 3, 4);
  expect_range(2, {7}, "Lists", 4, 9, 9);
  EXPECT_EQ((*found)[3].source, Source::Inline);
  EXPECT_EQ((*found)[3].columns, std::vector<int>{8});
  EXPECT_EQ((*found)[3].entries, (std::vector<std::string>{"Yes", "No", "N/A", "say \"so\""}));
  expect_range(4, {9}, "Lookup Tables", 3, 2, 17);
  EXPECT_EQ((*found)[4].formula, "Monitors");
  // What cannot be followed is said to be so, with what was written; it is
  // not an error.
  for (const auto& [i, formula, column] :
       {std::tuple{5, "Nobody", 10}, std::tuple{6, "Lists!$A$1:$B$4", 12}, std::tuple{7, "INDIRECT(\"x\")", 13}}) {
    const ListValidation& one = (*found)[static_cast<std::size_t>(i)];
    EXPECT_EQ(one.source, Source::Unresolved) << formula;
    EXPECT_EQ(one.formula, formula);
    EXPECT_EQ(one.columns, std::vector<int>{column}) << formula;
    EXPECT_TRUE(one.sheet.empty() && one.entries.empty()) << formula;
  }
  expect_range(8, {27, 29, 30}, "Lists", 5, 2, 3);

  // A sheet with none, and one whose validation says nothing usable.
  EXPECT_EQ(open(zip_file("plain.xlsx", small_workbook("<sheetData/>"))).list_validations("Data")->size(), 0u);
  const Workbook odd = open(zip_file(
      "odd.xlsx", validated_workbook("<dataValidations><dataValidation type=\"list\" sqref=\"not cells\"/>"
                                     "<dataValidation type=\"list\"><formula1>Lists!$A$1:$A$2</formula1>"
                                     "</dataValidation></dataValidations>")));
  const auto empty = odd.list_validations("Data");
  ASSERT_TRUE(empty.has_value());
  ASSERT_EQ(empty->size(), 2u);
  EXPECT_TRUE((*empty)[0].columns.empty());
  EXPECT_EQ((*empty)[0].source, Source::Unresolved);
  EXPECT_TRUE((*empty)[1].columns.empty());
}

TEST_F(Ooxml, TheValuesOfAColumn) {
  const Workbook arar = open(template_file(kArAr));
  // The flux monitors: the range the ArArAgeCalc sheet's validation names.
  const auto monitors = arar.column_values("Lookup Tables", 3, 2, 17);
  ASSERT_TRUE(monitors.has_value()) << to_string(monitors.error());
  ASSERT_EQ(monitors->size(), 16u);
  EXPECT_EQ(monitors->front(), "Alder Creek Rhyolite Sanidine (ACs) from Phillips et al. 2022 (1.1834 Ma)");
  EXPECT_EQ((*monitors)[2], "Fish Canyon Tuff Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)");
  EXPECT_EQ(monitors->back(), "WA1ms (Western Australia Muscovite) from Jourdan et al. (2014) (2613 Ma)");
  // One string per row asked for: empty where the column holds nothing, and
  // for rows the sheet does not store. Text is as the cell has it, spaces
  // too.
  EXPECT_EQ(*arar.column_values("Lookup Tables", 2, 1, 5),
            (std::vector<std::string>{"Air Ratio", "Lee et al. 2006", "Nier 1950", "", ""}));
  EXPECT_EQ(*arar.column_values("Lookup Tables", 1, 4, 4), std::vector<std::string>{"Renne et al. 2011 "});
  EXPECT_EQ(*arar.column_values("Lookup Tables", 1, 5000, 5002), (std::vector<std::string>{"", "", ""}));
  EXPECT_EQ(*arar.column_values("Lookup Tables", 300, 2, 3), (std::vector<std::string>{"", ""}));

  // The long one: every rock and mineral the service knows.
  const Workbook sample = open(template_file(kSample));
  const auto materials = sample.column_values("Lookup tables", 3, 2, 10185);
  ASSERT_TRUE(materials.has_value());
  ASSERT_EQ(materials->size(), 10184u);
  EXPECT_EQ(materials->front(), "Unknown");
  EXPECT_EQ((*materials)[1243 - 2], "Biotite");
  EXPECT_EQ((*materials)[8433 - 2], "Sanidine");
  EXPECT_EQ(materials->back(), "Zygadite");

  for (const auto& bad : {arar.column_values("No Such Sheet", 1, 1, 2), arar.column_values("Lookup Tables", 0, 1, 2),
                          arar.column_values("Lookup Tables", kMaxColumn + 1, 1, 2),
                          arar.column_values("Lookup Tables", 1, 0, 2), arar.column_values("Lookup Tables", 1, 3, 2),
                          arar.column_values("Lookup Tables", 1, 1, kMaxRow + 1)}) {
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().kind, ErrorKind::Config);
  }
}

TEST_F(Ooxml, TheFillOfACell) {
  // The legend of each template's definitions, and a header under it.
  const Workbook arar = open(template_file(kArAr));
  EXPECT_EQ(*arar.row_values("Field Definitions", 5), std::vector<std::string>{"Required"});
  EXPECT_EQ(*arar.cell_fill("Field Definitions", 5, 1), "FFFAD9D6");
  EXPECT_EQ(*arar.cell_fill("Field Definitions", 4, 1), "FFFEF1CC");  // Recommended
  EXPECT_EQ(*arar.cell_fill("Field Definitions", 3, 1), "FFDAF1F3");  // Optional
  EXPECT_EQ(*arar.cell_fill("Field Definitions", 2, 1), "");          // styled, not filled
  EXPECT_EQ(*arar.cell_fill("ArArDataPoints", 3, 3), "FFFAD9D6");     // Analytical Key
  EXPECT_EQ(*arar.cell_fill("ArArDataPoints", 3, 6), "FFDAF1F3");     // Comment
  EXPECT_EQ(*arar.cell_fill("ArArDataPoints", 3, 9), "FFFEF1CC");     // Rock Type
  EXPECT_EQ(*arar.cell_fill("ArArDataPoints", 4, 3), "");
  // A cell the row has not, a row the sheet has not.
  EXPECT_EQ(*arar.cell_fill("ArArDataPoints", 3, 500), "");
  EXPECT_EQ(*arar.cell_fill("ArArDataPoints", 100000, 1), "");

  const auto fills = arar.row_fills("ArArDataPoints", 3);
  ASSERT_TRUE(fills.has_value());
  ASSERT_EQ(fills->size(), 22u);
  for (std::size_t i = 0; i < fills->size(); ++i) {
    EXPECT_EQ((*fills)[i], *arar.cell_fill("ArArDataPoints", 3, static_cast<int>(i) + 1)) << i;
  }
  EXPECT_TRUE(arar.row_fills("ArArDataPoints", 100000)->empty());

  const Workbook sample = open(template_file(kSample));
  EXPECT_EQ(*sample.row_values("Column Descriptions", 1), std::vector<std::string>{"Required field"});
  EXPECT_EQ(*sample.cell_fill("Column Descriptions", 1, 1), "FFFAD9D6");
  EXPECT_EQ(*sample.cell_fill("Samples", 3, 1), "FFFAD9D6");  // Sample Name
  EXPECT_EQ(*sample.cell_fill("Samples", 3, 2), "FFD9EAD3");  // IGSN
  EXPECT_EQ(*sample.cell_fill("Samples", 4, 1), "");

  for (const auto& bad : {arar.cell_fill("No Such Sheet", 1, 1), arar.cell_fill("ArArDataPoints", 0, 1),
                          arar.cell_fill("ArArDataPoints", 1, 0), arar.cell_fill("ArArDataPoints", 1, kMaxColumn + 1)}) {
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().kind, ErrorKind::Config);
  }
}

// A one-sheet workbook with a styles part: row 1 has a cell of each style.
std::vector<Part> styled_workbook(const std::string& styles, int style_count) {
  std::string row = "<sheetData><row r=\"1\">";
  for (int i = 0; i < style_count; ++i) row += "<c r=\"" + cell_reference(i + 1, 1) + "\" s=\"" + std::to_string(i) + "\"/>";
  row += "<c r=\"" + cell_reference(style_count + 1, 1) + "\"/></row></sheetData>";
  std::vector<Part> parts = small_workbook(row);
  const std::string close = "</Relationships>";
  parts[2].data.insert(parts[2].data.size() - close.size(),
                       "<Relationship Id=\"rId8\" "
                       "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
                       "Target=\"styles.xml\"/>");
  if (!styles.empty()) parts.push_back({"xl/styles.xml", styles});
  return parts;
}

TEST_F(Ooxml, AFillIsASolidOneAndItsColourAsWritten) {
  const std::string styles =
      "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><fills>"
      "<fill><patternFill patternType=\"none\"/></fill>"
      "<fill><patternFill patternType=\"gray125\"><fgColor rgb=\"FF111111\"/></patternFill></fill>"
      "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFABCDEF\"/><bgColor indexed=\"64\"/></patternFill></fill>"
      "<fill><patternFill patternType=\"solid\"><fgColor theme=\"4\" tint=\"0.5\"/></patternFill></fill>"
      "<fill><patternFill patternType=\"solid\"><fgColor indexed=\"13\"/></patternFill></fill>"
      "<fill><gradientFill/></fill>"
      "<fill><patternFill patternType=\"solid\"/></fill>"
      "</fills><cellXfs>"
      "<xf fillId=\"0\"/><xf fillId=\"1\"/><xf fillId=\"2\"/><xf fillId=\"3\"/><xf fillId=\"4\"/><xf fillId=\"5\"/>"
      "<xf fillId=\"6\"/><xf fillId=\"99\"/><xf/></cellXfs></styleSheet>";
  const Workbook book = open(zip_file("styled.xlsx", styled_workbook(styles, 10)));
  // A pattern is not a fill of one colour; a theme or indexed colour is
  // given as it is written, which is all two cells need to be compared.
  EXPECT_EQ(*book.row_fills("Data", 1),
            (std::vector<std::string>{"", "", "FFABCDEF", "theme 4 tint 0.5", "indexed 13", "", "", "", "", "", ""}));
  EXPECT_EQ(*book.cell_fill("Data", 1, 3), "FFABCDEF");

  // A workbook without a styles part has no fills; one whose styles do not
  // parse says so when asked, and still opens.
  EXPECT_EQ(*open(zip_file("unstyled.xlsx", small_workbook("<sheetData><row r=\"1\"><c r=\"A1\" s=\"2\"/></row></sheetData>")))
                 .cell_fill("Data", 1, 1),
            "");
  const Workbook broken = open(zip_file("broken.xlsx", styled_workbook("<styleSheet><fills>", 2)));
  const auto fill = broken.cell_fill("Data", 1, 1);
  ASSERT_FALSE(fill.has_value());
  EXPECT_EQ(fill.error().kind, ErrorKind::Protocol);
  EXPECT_TRUE(broken.row_values("Data", 1).has_value());
}

}  // namespace
}  // namespace pychron::earthbank::ooxml
