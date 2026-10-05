#include "pychron/vision/png.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace pychron::vision {

namespace {

using Bytes = std::vector<std::uint8_t>;

std::uint32_t crc32(std::uint32_t c, const std::uint8_t* data, std::size_t n) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = (v & 1u) ? 0xedb88320u ^ (v >> 1) : v >> 1;
      t[i] = v;
    }
    return t;
  }();
  for (std::size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xffu] ^ (c >> 8);
  return c;
}

void put32(Bytes& out, std::uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(v >> shift));
}

void chunk(Bytes& out, const char type[4], const Bytes& data) {
  put32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t from = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  put32(out, crc32(0xffffffffu, &out[from], out.size() - from) ^ 0xffffffffu);
}

}  // namespace

Result<void> write_png(const std::filesystem::path& file, const FrameView& view) {
  if (view.data == nullptr || view.width <= 0 || view.height <= 0) {
    return fail(ErrorKind::Config, "no picture to write to " + file.string());
  }
  const bool wide = view.pixel_depth > 255;
  const std::uint32_t depth = view.pixel_depth > 0 ? view.pixel_depth : 255;

  // The scanlines, each behind its filter byte (0: none).
  Bytes rows;
  rows.reserve(static_cast<std::size_t>(view.height) * (1 + static_cast<std::size_t>(view.width) * (wide ? 2 : 1)));
  for (int y = 0; y < view.height; ++y) {
    rows.push_back(0);
    for (int x = 0; x < view.width; ++x) {
      const std::uint32_t v = view.at(x, y);
      if (wide) {
        const std::uint32_t full = v >= depth ? 65535u : v * 65535u / depth;
        rows.push_back(static_cast<std::uint8_t>(full >> 8));
        rows.push_back(static_cast<std::uint8_t>(full));
      } else {
        rows.push_back(static_cast<std::uint8_t>(v > 255 ? 255 : v));
      }
    }
  }

  // zlib: a header, the rows in stored blocks of at most 65 535 bytes, adler32.
  Bytes zlib{0x78, 0x01};
  constexpr std::size_t kBlock = 65535;
  for (std::size_t at = 0; at < rows.size(); at += kBlock) {
    const std::size_t n = std::min(kBlock, rows.size() - at);
    zlib.push_back(at + n == rows.size() ? 1 : 0);
    zlib.push_back(static_cast<std::uint8_t>(n));
    zlib.push_back(static_cast<std::uint8_t>(n >> 8));
    zlib.push_back(static_cast<std::uint8_t>(~n));
    zlib.push_back(static_cast<std::uint8_t>(~n >> 8));
    zlib.insert(zlib.end(), rows.begin() + static_cast<std::ptrdiff_t>(at), rows.begin() + static_cast<std::ptrdiff_t>(at + n));
  }
  std::uint32_t a = 1, b = 0;
  for (const std::uint8_t v : rows) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  put32(zlib, (b << 16) | a);

  Bytes out{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  Bytes header;
  put32(header, static_cast<std::uint32_t>(view.width));
  put32(header, static_cast<std::uint32_t>(view.height));
  header.insert(header.end(), {static_cast<std::uint8_t>(wide ? 16 : 8), 0, 0, 0, 0});  // grey, deflate, no filter set, not interlaced
  chunk(out, "IHDR", header);
  chunk(out, "IDAT", zlib);
  chunk(out, "IEND", {});

  // Beside the target, then renamed: nobody opens half a picture.
  const std::filesystem::path tmp = std::filesystem::path(file).concat(".tmp");
  std::error_code ec;
  {
    std::ofstream stream(tmp, std::ios::binary | std::ios::trunc);
    if (stream) stream.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    stream.flush();
    if (!stream) {
      std::filesystem::remove(tmp, ec);
      return fail(ErrorKind::Io, "cannot write " + file.string());
    }
  }
  std::filesystem::rename(tmp, file, ec);
  if (ec) {
    const std::string why = ec.message();
    std::filesystem::remove(tmp, ec);
    return fail(ErrorKind::Io, "cannot write " + file.string() + ": " + why);
  }
  return {};
}

}  // namespace pychron::vision
