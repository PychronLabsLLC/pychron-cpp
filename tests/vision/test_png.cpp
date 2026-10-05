// The PNG writer (live camera design, section 5): grey, stored deflate. Read
// back here by a reader of exactly that subset, CRCs and checksum included.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "pychron/vision/png.hpp"

using namespace pychron;
using namespace pychron::vision;

namespace {

struct TempDir {
  std::filesystem::path path;
  TempDir() {
    static int n = 0;
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    path = std::filesystem::temp_directory_path() /
           (std::string("pychron_png_") + info->name() + "_" + std::to_string(++n));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

using Bytes = std::vector<std::uint8_t>;

std::uint32_t be32(const Bytes& b, std::size_t at) {
  return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) | (std::uint32_t{b[at + 2]} << 8) | b[at + 3];
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
  std::uint32_t c = 0xffffffffu;
  for (std::size_t i = 0; i < n; ++i) {
    c ^= data[i];
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xffffffffu;
}

struct Png {
  int width = 0, height = 0, depth = 0, colour = -1;
  Bytes rows;  // the filtered scanlines, inflated
};

// nullopt, with why, for anything that is not a well-formed grey PNG of
// stored blocks.
std::optional<Png> read(const std::filesystem::path& file, std::string& why) {
  std::ifstream in(file, std::ios::binary);
  const Bytes b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const Bytes magic{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  if (b.size() < 8 || !std::equal(magic.begin(), magic.end(), b.begin())) return why = "signature", std::nullopt;
  Png png;
  Bytes zlib;
  bool ended = false;
  for (std::size_t at = 8; at + 12 <= b.size();) {
    const std::uint32_t n = be32(b, at);
    if (at + 12 + n > b.size()) return why = "chunk runs off the file", std::nullopt;
    const std::string type(b.begin() + static_cast<std::ptrdiff_t>(at + 4), b.begin() + static_cast<std::ptrdiff_t>(at + 8));
    if (crc32(&b[at + 4], n + 4) != be32(b, at + 8 + n)) return why = "crc of " + type, std::nullopt;
    if (type == "IHDR") {
      png.width = static_cast<int>(be32(b, at + 8));
      png.height = static_cast<int>(be32(b, at + 12));
      png.depth = b[at + 16];
      png.colour = b[at + 17];
      if (b[at + 18] != 0 || b[at + 19] != 0 || b[at + 20] != 0) return why = "compression, filter or interlace", std::nullopt;
    } else if (type == "IDAT") {
      zlib.insert(zlib.end(), b.begin() + static_cast<std::ptrdiff_t>(at + 8), b.begin() + static_cast<std::ptrdiff_t>(at + 8 + n));
    } else if (type == "IEND") {
      ended = true;
    }
    at += 12 + n;
  }
  if (!ended) return why = "no IEND", std::nullopt;
  if (zlib.size() < 6 || (zlib[0] * 256 + zlib[1]) % 31 != 0 || (zlib[0] & 0x0f) != 8) return why = "zlib header", std::nullopt;
  std::size_t at = 2;
  for (bool last = false; !last;) {
    if (at + 5 > zlib.size()) return why = "block header", std::nullopt;
    last = (zlib[at] & 1) != 0;
    if ((zlib[at] >> 1) != 0) return why = "not a stored block", std::nullopt;
    const std::uint16_t len = static_cast<std::uint16_t>(zlib[at + 1] | (zlib[at + 2] << 8));
    const std::uint16_t nlen = static_cast<std::uint16_t>(zlib[at + 3] | (zlib[at + 4] << 8));
    if (static_cast<std::uint16_t>(~len) != nlen) return why = "block length check", std::nullopt;
    at += 5;
    if (at + len > zlib.size()) return why = "block runs off", std::nullopt;
    png.rows.insert(png.rows.end(), zlib.begin() + static_cast<std::ptrdiff_t>(at), zlib.begin() + static_cast<std::ptrdiff_t>(at + len));
    at += len;
  }
  std::uint32_t a = 1, s = 0;
  for (const std::uint8_t v : png.rows) {
    a = (a + v) % 65521;
    s = (s + a) % 65521;
  }
  if (at + 4 != zlib.size() || be32(zlib, at) != ((s << 16) | a)) return why = "adler32", std::nullopt;
  return png;
}

Frame ramp(int w, int h, std::uint16_t depth) {
  Frame f = Frame::make(w, h, depth);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) f.at(x, y) = static_cast<std::uint16_t>((x * 7 + y * 131) % (depth + 1));
  return f;
}

}  // namespace

TEST(Png, EightBitGreyReadsBack) {
  TempDir d;
  const Frame f = ramp(5, 3, 255);
  ASSERT_TRUE(write_png(d.path / "a.png", f.view()));
  std::string why;
  const auto png = read(d.path / "a.png", why);
  ASSERT_TRUE(png) << why;
  EXPECT_EQ(png->width, 5);
  EXPECT_EQ(png->height, 3);
  EXPECT_EQ(png->depth, 8);
  EXPECT_EQ(png->colour, 0);
  ASSERT_EQ(png->rows.size(), 3u * (1 + 5));
  for (int y = 0; y < 3; ++y) {
    EXPECT_EQ(png->rows[static_cast<std::size_t>(y) * 6], 0) << "filter none";
    for (int x = 0; x < 5; ++x) EXPECT_EQ(png->rows[static_cast<std::size_t>(y) * 6 + 1 + static_cast<std::size_t>(x)], f.view().at(x, y));
  }
}

TEST(Png, SixteenBitIsBigEndian) {
  TempDir d;
  const Frame f = ramp(4, 2, 4095);
  ASSERT_TRUE(write_png(d.path / "a.png", f.view()));
  std::string why;
  const auto png = read(d.path / "a.png", why);
  ASSERT_TRUE(png) << why;
  EXPECT_EQ(png->depth, 16);
  ASSERT_EQ(png->rows.size(), 2u * (1 + 8));
  // scaled to the full 16 bits, so a viewer shows it as bright as it was
  const std::uint16_t v = f.view().at(3, 1);
  const auto want = static_cast<std::uint16_t>(std::uint32_t{v} * 65535u / 4095u);
  const std::size_t at = 9 + 1 + 3 * 2;
  EXPECT_EQ((png->rows[at] << 8) | png->rows[at + 1], want);
}

TEST(Png, AViewNarrowerThanItsStride) {
  TempDir d;
  const Frame f = ramp(10, 6, 255);
  FrameView v = f.view();
  v.data += 10 + 2;  // from (2, 1)
  v.width = 4;
  v.height = 3;
  ASSERT_TRUE(write_png(d.path / "a.png", v));
  std::string why;
  const auto png = read(d.path / "a.png", why);
  ASSERT_TRUE(png) << why;
  EXPECT_EQ(png->width, 4);
  EXPECT_EQ(png->rows[1 * 5 + 1 + 2], f.view().at(4, 2));
}

TEST(Png, ALargeImageTakesSeveralBlocks) {
  TempDir d;
  const Frame f = ramp(400, 300, 255);  // 120 300 bytes: more than one 65 535-byte block
  ASSERT_TRUE(write_png(d.path / "a.png", f.view()));
  std::string why;
  const auto png = read(d.path / "a.png", why);
  ASSERT_TRUE(png) << why;
  ASSERT_EQ(png->rows.size(), 300u * 401u);
  EXPECT_EQ(png->rows[299u * 401u + 1 + 399], f.view().at(399, 299));
}

TEST(Png, RefusesWhatCannotBeWritten) {
  TempDir d;
  const Frame f = ramp(2, 2, 255);
  const auto nowhere = write_png(d.path / "no" / "such" / "a.png", f.view());
  ASSERT_FALSE(nowhere);
  EXPECT_EQ(nowhere.error().kind, ErrorKind::Io);
  const Frame empty;
  const auto nothing = write_png(d.path / "b.png", empty.view());
  ASSERT_FALSE(nothing);
  EXPECT_EQ(nothing.error().kind, ErrorKind::Config);
  EXPECT_FALSE(std::filesystem::exists(d.path / "b.png"));
}
