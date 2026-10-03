#include "pychron/vision/pgm.hpp"

#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace pychron::vision {
namespace {

struct Header {
  int width = 0, height = 0, maxval = 0;
  std::size_t data_offset = 0;
};

bool is_space(unsigned char c) { return std::isspace(c) != 0; }

// Next whitespace-delimited token; '#' starts a comment to end of line.
// Leaves `pos` on the byte after the token (the single separator byte).
bool next_token(const std::vector<unsigned char>& b, std::size_t& pos, std::string& tok) {
  tok.clear();
  while (pos < b.size()) {
    if (b[pos] == '#') {
      while (pos < b.size() && b[pos] != '\n' && b[pos] != '\r') ++pos;
    } else if (is_space(b[pos])) {
      ++pos;
    } else {
      break;
    }
  }
  while (pos < b.size() && !is_space(b[pos]) && b[pos] != '#') tok.push_back(static_cast<char>(b[pos++]));
  return !tok.empty();
}

bool parse_int(const std::string& s, int& out) {
  if (s.empty() || s.size() > 9) return false;
  int v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = v;
  return true;
}

}  // namespace

Result<Frame> read_pgm(const std::filesystem::path& path) {
  const std::string name = path.string();
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot open " + name);
  const std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  // Running out of bytes is truncation (Io); a token that is present but wrong is a bad file (Config).
  std::size_t pos = 0;
  std::string tok;
  if (!next_token(b, pos, tok)) return fail(ErrorKind::Io, "truncated PGM (no magic) in " + name);
  if (tok != "P5") return fail(ErrorKind::Config, "not a binary P5 PGM: " + name);

  int vals[3] = {0, 0, 0};
  for (int& v : vals) {
    if (!next_token(b, pos, tok)) return fail(ErrorKind::Io, "truncated PGM header in " + name);
    if (!parse_int(tok, v)) return fail(ErrorKind::Config, "non-numeric PGM header token '" + tok + "' in " + name);
  }
  const int w = vals[0], h = vals[1], maxval = vals[2];
  if (w <= 0 || h <= 0) return fail(ErrorKind::Config, "PGM has zero dimension: " + name);
  if (maxval < 1 || maxval > 65535) return fail(ErrorKind::Config, "PGM maxval out of range in " + name);
  // Exactly one whitespace byte separates maxval from the data.
  if (pos >= b.size()) return fail(ErrorKind::Io, "truncated PGM header (no data) in " + name);
  if (b[pos] == '#')
    return fail(ErrorKind::Config, "PGM comment after maxval is not allowed (header ends after one whitespace byte) in " + name);
  if (!is_space(b[pos])) return fail(ErrorKind::Config, "PGM maxval not followed by whitespace in " + name);
  ++pos;

  const unsigned long long bytes_per = maxval > 255 ? 2 : 1;
  // 64-bit, and compared before any allocation, so absurd dimensions cannot allocate or overflow.
  const unsigned long long count = static_cast<unsigned long long>(w) * static_cast<unsigned long long>(h);
  if (static_cast<unsigned long long>(b.size() - pos) / bytes_per < count)
    return fail(ErrorKind::Io, "truncated PGM data in " + name);

  Frame f = Frame::make(w, h, static_cast<std::uint16_t>(maxval));
  for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
    const std::size_t o = pos + i * static_cast<std::size_t>(bytes_per);
    f.data[i] = bytes_per == 1 ? static_cast<std::uint16_t>(b[o])
                               : static_cast<std::uint16_t>((b[o] << 8) | b[o + 1]);
  }
  return f;
}

Result<void> write_pgm(const std::filesystem::path& path, const FrameView& v) {
  if (v.width <= 0 || v.height <= 0 || v.pixel_depth < 1)
    return fail(ErrorKind::Config, "cannot write an empty frame to " + path.string());
  const bool wide = v.pixel_depth > 255;
  std::string out = "P5\n" + std::to_string(v.width) + " " + std::to_string(v.height) + "\n" +
                    std::to_string(v.pixel_depth) + "\n";
  out.reserve(out.size() + static_cast<std::size_t>(v.width) * static_cast<std::size_t>(v.height) * (wide ? 2 : 1));
  for (int y = 0; y < v.height; ++y)
    for (int x = 0; x < v.width; ++x) {
      const std::uint16_t p = v.at(x, y);
      if (wide) out.push_back(static_cast<char>(p >> 8));
      out.push_back(static_cast<char>(p & 0xff));
    }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return fail(ErrorKind::Io, "cannot open " + path.string() + " for writing");
  f.write(out.data(), static_cast<std::streamsize>(out.size()));
  f.close();
  if (!f) return fail(ErrorKind::Io, "write failed for " + path.string());
  return {};
}

}  // namespace pychron::vision
