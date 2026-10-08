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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "ooxml.hpp"
#include "pychron/core/error.hpp"

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

std::uint32_t crc32(std::string_view bytes) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char c : bytes) {
    crc ^= static_cast<unsigned char>(c);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void put16(std::string& out, std::uint32_t v) {
  out.push_back(static_cast<char>(v & 0xFFu));
  out.push_back(static_cast<char>((v >> 8) & 0xFFu));
}

void put32(std::string& out, std::uint32_t v) {
  put16(out, v & 0xFFFFu);
  put16(out, v >> 16);
}

struct Part {
  std::string name;
  std::string data;
  // The uncompressed size the headers state. With one, the entry is written
  // as a deflate stream (of stored blocks), as a reader takes a stored entry
  // whose two sizes differ for damage before it looks at either.
  std::optional<std::uint32_t> claimed_size = {};
};

// `data` as a deflate stream that compresses nothing: stored blocks.
std::string deflate_stored(std::string_view data) {
  std::string out;
  do {
    const std::size_t length = std::min<std::size_t>(data.size(), 0xFFFF);
    const bool last = length == data.size();
    out.push_back(last ? '\x01' : '\x00');
    out.push_back(static_cast<char>(length & 0xFFu));
    out.push_back(static_cast<char>((length >> 8) & 0xFFu));
    out.push_back(static_cast<char>(~length & 0xFFu));
    out.push_back(static_cast<char>((~length >> 8) & 0xFFu));
    out += data.substr(0, length);
    data.remove_prefix(length);
  } while (!data.empty());
  return out;
}

std::string make_zip(const std::vector<Part>& parts) {
  std::string out;
  std::string directory;
  for (const Part& part : parts) {
    const auto offset = static_cast<std::uint32_t>(out.size());
    const bool deflated = part.claimed_size.has_value();
    const std::string body = deflated ? deflate_stored(part.data) : part.data;
    const std::uint32_t method = deflated ? 8 : 0;
    const auto size = static_cast<std::uint32_t>(body.size());
    const auto claimed = part.claimed_size.value_or(static_cast<std::uint32_t>(part.data.size()));
    const std::uint32_t crc = crc32(part.data);
    const auto name = static_cast<std::uint32_t>(part.name.size());
    put32(out, 0x04034b50u);
    put16(out, 20);
    put16(out, 0);
    put16(out, method);
    put16(out, 0);
    put16(out, 0x0021);
    put32(out, crc);
    put32(out, size);
    put32(out, claimed);
    put16(out, name);
    put16(out, 0);
    out += part.name;
    out += body;

    put32(directory, 0x02014b50u);
    put16(directory, 20);
    put16(directory, 20);
    put16(directory, 0);
    put16(directory, method);
    put16(directory, 0);
    put16(directory, 0x0021);
    put32(directory, crc);
    put32(directory, size);
    put32(directory, claimed);
    put16(directory, name);
    put16(directory, 0);
    put16(directory, 0);
    put16(directory, 0);
    put16(directory, 0);
    put32(directory, 0);
    put32(directory, offset);
    directory += part.name;
  }
  const auto start = static_cast<std::uint32_t>(out.size());
  const auto count = static_cast<std::uint32_t>(parts.size());
  out += directory;
  put32(out, 0x06054b50u);
  put16(out, 0);
  put16(out, 0);
  put16(out, count);
  put16(out, count);
  put32(out, static_cast<std::uint32_t>(directory.size()));
  put32(out, start);
  put16(out, 0);
  return out;
}

// The DOS time and date of each entry, read from the central directory of an
// archive without a comment.
struct Stamp {
  std::string name;
  unsigned time = 0;
  unsigned date = 0;
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
    out.push_back({std::string(zip.substr(at + 46, name)), u16(at + 12), u16(at + 14)});
    at += 46 + name + u16(at + 30) + u16(at + 32);
  }
  return out;
}

// A workbook of one sheet, "Data", with the sheet XML given.
const char* const kSheetHead =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";

std::vector<Part> small_workbook(const std::string& sheet_body) {
  return {
      {"[Content_Types].xml", "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"/>"},
      {"xl/workbook.xml",
       "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
       "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
       "<sheets><sheet name=\"Data\" sheetId=\"1\" r:id=\"rId7\"/></sheets></workbook>"},
      {"xl/_rels/workbook.xml.rels",
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
       "<Relationship Id=\"rId7\" "
       "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
       "Target=\"worksheets/data.xml\"/></Relationships>"},
      {"xl/worksheets/data.xml", std::string(kSheetHead) + sheet_body + "</worksheet>"},
  };
}

std::string text_cell(const std::string& ref, const std::string& text) {
  return "<c r=\"" + ref + "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">" + text + "</t></is></c>";
}

class Ooxml : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / (std::string("pychron_ooxml_") + info->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  fs::path file(const std::string& name) const { return dir_ / name; }

  fs::path zip_file(const std::string& name, const std::vector<Part>& parts) const {
    spit(file(name), make_zip(parts));
    return file(name);
  }

  static Workbook open(const fs::path& path) {
    auto book = Workbook::open(path);
    EXPECT_TRUE(book.has_value()) << (book ? "" : to_string(book.error()));
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
  const std::string next = "<row r=\"6\" spans=\"1:22\" ht=\"15\">";
  EXPECT_EQ(xml.substr(end, next.size()), next);
}

TEST_F(Ooxml, RowsPastTheLastStoredRowAreAddedAndTheDimensionGrows) {
  // ArArAgeSummary stores rows 1 to 7 and says <dimension ref="A1:BN7"/>.
  Workbook book = open(template_file(kArAr));
  std::vector<std::vector<Cell>> rows;
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
  const std::string body = written("<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData>", 2, {row});
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

TEST_F(Ooxml, ANumberIsItsShortestDecimalThatReadsBack) {
  const std::vector<double> numbers = {0.1,  5.0,   -1.5,       1.0 / 3.0, 1e22,  2.5e-7,
                                       0.0,  1e-300, 123456789.0, 5.463e-10, 40.01, std::numeric_limits<double>::max()};
  std::vector<Cell> row;
  for (const double number : numbers) row.push_back(Cell{number});
  const std::string body = written("<sheetData/>", 2, {row});
  EXPECT_NE(body.find("<c r=\"A2\"><v>0.1</v></c><c r=\"B2\"><v>5</v></c><c r=\"C2\"><v>-1.5</v></c>"),
            std::string::npos);
  EXPECT_NE(body.find("<c r=\"K2\"><v>40.01</v></c>"), std::string::npos);
  const auto values = open(file("out.xlsx")).row_values("Data", 2);
  ASSERT_TRUE(values.has_value());
  ASSERT_EQ(values->size(), numbers.size());
  for (std::size_t i = 0; i < numbers.size(); ++i) {
    SCOPED_TRACE((*values)[i]);
    EXPECT_EQ((*values)[i].find(','), std::string::npos);
    EXPECT_EQ(std::strtod((*values)[i].c_str(), nullptr), numbers[i]);
  }
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
  expect_protocol(with(1, "<workbook><sheets><sheet name=\"Data\" sheetId=\"1\"/></sheets></workbook>"));  // no r:id
  expect_protocol(with(2, "<Relationships/>"));                                    // the id leads nowhere
  expect_protocol(with(2, "<Relationships><Relationship Id=\"rId7\" Target=\"worksheets/gone.xml\"/></Relationships>"));
  expect_protocol(with(2, "<Relationships><Relationship Id=\"rId7\" Target=\"../../../etc/passwd\"/></Relationships>"));

  // No relationships part at all.
  std::vector<Part> parts = small_workbook("<sheetData/>");
  parts.erase(parts.begin() + 2);
  expect_protocol(Workbook::open(zip_file("norels.xlsx", parts)));

  // A sheet that is not a sheet: the workbook opens, the sheet does not read.
  for (const std::string sheet : {"<worksheet><sheetData><row r=\"1\">", "", "<worksheet/>", "<chartsheet/>",
                                  "<worksheet><sheetData><row r=\"2\"/><row r=\"2\"/></sheetData></worksheet>",
                                  "<worksheet><sheetData><row/></sheetData></worksheet>",
                                  "<worksheet><sheetData><row r=\"2\"><c r=\"B2\"/><c r=\"A2\"/></row></sheetData>"
                                  "</worksheet>",
                                  "<worksheet><sheetData><row r=\"2\"><c r=\"A3\"/></row></sheetData></worksheet>",
                                  "<worksheet><sheetData><row r=\"2\"><c r=\"2A\"/></row></sheetData></worksheet>",
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

}  // namespace
}  // namespace pychron::earthbank::ooxml
