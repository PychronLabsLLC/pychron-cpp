#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "pychron/vision/pgm.hpp"

using namespace pychron;
using namespace pychron::vision;

namespace {

// Unique scratch directory, removed on destruction.
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    static int n = 0;
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    path = std::filesystem::temp_directory_path() /
           (std::string("pychron_vision_") + info->test_suite_name() + "_" + info->name() + "_" + std::to_string(++n));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

void write_bytes(const std::filesystem::path& p, const std::string& bytes) {
  std::ofstream(p, std::ios::binary) << bytes;
}

}  // namespace

TEST(Pgm, RoundTripEightBit) {
  TempDir d;
  Frame f = Frame::make(5, 3, 255);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 5; ++x) f.at(x, y) = static_cast<std::uint16_t>(y * 50 + x);
  ASSERT_TRUE(write_pgm(d.path / "a.pgm", f.view()).has_value());
  EXPECT_EQ(std::filesystem::file_size(d.path / "a.pgm"), std::string("P5\n5 3\n255\n").size() + 15);
  auto r = read_pgm(d.path / "a.pgm");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->width, 5);
  EXPECT_EQ(r->height, 3);
  EXPECT_EQ(r->pixel_depth, 255);
  EXPECT_EQ(r->data, f.data);
}

TEST(Pgm, RoundTripSixteenBitBigEndian) {
  TempDir d;
  Frame f = Frame::make(2, 1, 4095);
  f.at(0, 0) = 0x0102;
  f.at(1, 0) = 0x0fff;
  ASSERT_TRUE(write_pgm(d.path / "a.pgm", f.view()).has_value());
  std::ifstream in(d.path / "a.pgm", std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string header = "P5\n2 1\n4095\n";
  ASSERT_EQ(bytes.size(), header.size() + 4);
  EXPECT_EQ(static_cast<unsigned char>(bytes[header.size()]), 0x01);
  EXPECT_EQ(static_cast<unsigned char>(bytes[header.size() + 1]), 0x02);
  auto r = read_pgm(d.path / "a.pgm");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->pixel_depth, 4095);
  EXPECT_EQ(r->data, f.data);
}

TEST(Pgm, ReadsHeaderWithComments) {
  TempDir d;
  // Comments after the magic and between tokens; mixed whitespace. (A comment after
  // maxval cannot be told from pixel data: the one separator byte ends the header.)
  write_bytes(d.path / "c.pgm", std::string("P5 # made by hand\n# full line\n2\t2 # size\n#x\n 255\n") +
                                    std::string("\x01\x02\x03\x04", 4));
  auto r = read_pgm(d.path / "c.pgm");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->width, 2);
  EXPECT_EQ(r->height, 2);
  EXPECT_EQ(r->data, (std::vector<std::uint16_t>{1, 2, 3, 4}));
}

TEST(Pgm, DataStartingWithWhitespaceByteIsNotSwallowed) {
  TempDir d;
  // Exactly one separator byte follows maxval; a pixel of value 10 ('\n') is data.
  write_bytes(d.path / "w.pgm", std::string("P5\n2 1\n255\n\x0a\x20", 13));
  auto r = read_pgm(d.path / "w.pgm");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->data, (std::vector<std::uint16_t>{10, 32}));
}

TEST(Pgm, TruncatedDataIsIoError) {
  TempDir d;
  write_bytes(d.path / "t.pgm", std::string("P5\n4 4\n255\n\x01\x02", 13));
  auto r = read_pgm(d.path / "t.pgm");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_NE(r.error().what.find("t.pgm"), std::string::npos);
}

TEST(Pgm, WrongMagicIsConfigError) {
  TempDir d;
  for (const char* magic : {"P2", "P6"}) {
    write_bytes(d.path / "m.pgm", std::string(magic) + "\n1 1\n255\n\x01");
    auto r = read_pgm(d.path / "m.pgm");
    ASSERT_FALSE(r.has_value()) << magic;
    EXPECT_EQ(r.error().kind, ErrorKind::Config) << magic;
  }
}

TEST(Pgm, MissingFileIsIoError) {
  TempDir d;
  auto r = read_pgm(d.path / "nope.pgm");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_NE(r.error().what.find("nope.pgm"), std::string::npos);
}

TEST(Pgm, ZeroDimensionIsConfigError) {
  TempDir d;
  write_bytes(d.path / "z.pgm", "P5\n0 4\n255\n");
  auto r = read_pgm(d.path / "z.pgm");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}

TEST(Pgm, MaxvalOutOfRangeIsConfigError) {
  TempDir d;
  write_bytes(d.path / "v.pgm", "P5\n1 1\n65536\n\x01\x01");
  auto r = read_pgm(d.path / "v.pgm");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}

TEST(Pgm, TruncatedHeadersAreIoErrorsNamingTheFile) {
  TempDir d;
  const std::vector<std::string> cases = {"", "P5", "P5\n4 4", "P5\n4 4\n255", "P5\n4 4\n255\n"};
  int i = 0;
  for (const auto& text : cases) {
    const auto p = d.path / ("h" + std::to_string(i++) + ".pgm");
    write_bytes(p, text);
    auto r = read_pgm(p);
    ASSERT_FALSE(r.has_value()) << "'" << text << "'";
    EXPECT_EQ(r.error().kind, ErrorKind::Io) << "'" << text << "'";
    EXPECT_NE(r.error().what.find(p.filename().string()), std::string::npos) << r.error().what;
  }
}

TEST(Pgm, MalformedHeaderIsConfigError) {
  TempDir d;
  const std::vector<std::string> cases = {
      "P5\n1 1\n0\n\x01",            // maxval 0
      "P5\n1 1\n70000\n\x01\x01",    // maxval too large
      "P5\nabc 1\n255\n\x01",        // non-numeric
      "P5\n1 1\n255# late comment\n\x01",  // comment right after maxval
      "P5\n1 1\n255x\x01",           // maxval not followed by whitespace
  };
  int i = 0;
  for (const auto& text : cases) {
    const auto p = d.path / ("m" + std::to_string(i++) + ".pgm");
    write_bytes(p, text);
    auto r = read_pgm(p);
    ASSERT_FALSE(r.has_value()) << i;
    EXPECT_EQ(r.error().kind, ErrorKind::Config) << i << ": " << r.error().what;
    EXPECT_NE(r.error().what.find(p.filename().string()), std::string::npos) << r.error().what;
  }
}

TEST(Pgm, HugeDimensionsFailWithoutAllocating) {
  TempDir d;
  write_bytes(d.path / "big.pgm", std::string("P5\n999999999 999999999\n255\n\x01\x02\x03", 29));
  auto r = read_pgm(d.path / "big.pgm");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
}
