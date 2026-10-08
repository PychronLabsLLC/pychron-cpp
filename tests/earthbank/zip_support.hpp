// A zip written by hand, for tests that need a workbook the vendor did not
// make: a damaged or hostile archive, or a template with something changed.
// Entries are stored, not compressed, unless a size is claimed for them.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pychron::earthbank::testing {

inline std::uint32_t crc32(std::string_view bytes) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char c : bytes) {
    crc ^= static_cast<unsigned char>(c);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

inline void put16(std::string& out, std::uint32_t v) {
  out.push_back(static_cast<char>(v & 0xFFu));
  out.push_back(static_cast<char>((v >> 8) & 0xFFu));
}

inline void put32(std::string& out, std::uint32_t v) {
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
inline std::string deflate_stored(std::string_view data) {
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

inline std::string make_zip(const std::vector<Part>& parts) {
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

}  // namespace pychron::earthbank::testing
