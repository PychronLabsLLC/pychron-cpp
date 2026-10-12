#include "ooxml.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <miniz.h>
#include <pugixml.hpp>

#include "pychron/core/user_file.hpp"

namespace pychron::earthbank::ooxml {
namespace {

// ===========================================================================
// The zip
// ===========================================================================

Result<std::string> read_file(const std::filesystem::path& file) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) {
    return fail(ErrorKind::Io, "cannot read " + file.string() + ": not a file");
  }
  const std::uintmax_t size = std::filesystem::file_size(file, ec);
  if (ec) return fail(ErrorKind::Io, "cannot read " + file.string() + ": " + ec.message());
  if (size > kMaxArchiveBytes) {
    return fail(ErrorKind::Protocol, file.string() + " is too large to be a workbook template");
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot open " + file.string());
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) return fail(ErrorKind::Io, "cannot read " + file.string());
  if (bytes.size() > kMaxArchiveBytes) {
    return fail(ErrorKind::Protocol, file.string() + " is too large to be a workbook template");
  }
  return bytes;
}

// miniz reads bytes held in memory through this: an archive's, or those of
// an entry to add. `opaque` is a std::string_view.
std::size_t read_view(void* opaque, mz_uint64 offset, void* buffer, std::size_t count) {
  const std::string_view bytes = *static_cast<const std::string_view*>(opaque);
  if (offset >= bytes.size() || count == 0) return 0;
  const auto from = static_cast<std::size_t>(offset);
  count = std::min(count, bytes.size() - from);
  std::memcpy(buffer, bytes.data() + from, count);
  return count;
}

// A miniz reader over bytes held elsewhere. miniz is handed a pointer to
// this, so it neither moves nor copies. The bytes are read through `read_at`
// and not miniz's own in-memory reader: that one forms a pointer from an
// offset before checking it, and a cut-off archive gives it offsets far
// outside the buffer (undefined behaviour, caught by UBSan).
class ZipReader {
 public:
  ZipReader() = default;
  ZipReader(const ZipReader&) = delete;
  ZipReader& operator=(const ZipReader&) = delete;
  ~ZipReader() {
    if (open_) mz_zip_reader_end(&zip_);
  }

  Result<void> open(std::string_view bytes) {
    bytes_ = bytes;
    zip_.m_pRead = &ZipReader::read_at;
    zip_.m_pIO_opaque = this;
    if (!mz_zip_reader_init(&zip_, bytes.size(), 0)) {
      return fail(ErrorKind::Protocol, std::string("not a zip archive (") + last_error() + ")");
    }
    open_ = true;
    if (mz_zip_reader_get_num_files(&zip_) > kMaxEntries) {
      return fail(ErrorKind::Protocol, "the archive has too many entries");
    }
    return {};
  }

  std::size_t count() { return mz_zip_reader_get_num_files(&zip_); }

  Result<std::string> name(std::size_t index) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip_, static_cast<mz_uint>(index), &stat)) {
      return fail(ErrorKind::Protocol, std::string("the archive's directory is damaged (") + last_error() + ")");
    }
    return std::string(stat.m_filename);
  }

  // `stored`: the entry's bytes as they lie in the archive.
  Result<std::string> extract(std::size_t index, bool stored = false) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip_, static_cast<mz_uint>(index), &stat)) {
      return fail(ErrorKind::Protocol, std::string("the archive's directory is damaged (") + last_error() + ")");
    }
    // The size is what the archive claims: checked before anything is
    // allocated for it. miniz then refuses data that does not fit the claim.
    const mz_uint64 size = stored ? stat.m_comp_size : stat.m_uncomp_size;
    if (size > kMaxPartBytes) {
      return fail(ErrorKind::Protocol, std::string(stat.m_filename) + " claims " + std::to_string(size) +
                                           " bytes, more than a workbook part can have");
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    if (stat.m_is_directory) return out;
    const mz_uint flags = stored ? static_cast<mz_uint>(MZ_ZIP_FLAG_COMPRESSED_DATA) : 0u;
    if (!mz_zip_reader_extract_to_mem(&zip_, static_cast<mz_uint>(index), out.data(), out.size(), flags)) {
      return fail(ErrorKind::Protocol, std::string(stat.m_filename) + " cannot be unpacked (" + last_error() + ")");
    }
    return out;
  }

  mz_zip_archive* raw() { return &zip_; }

 private:
  const char* last_error() { return mz_zip_get_error_string(mz_zip_get_last_error(&zip_)); }

  static std::size_t read_at(void* opaque, mz_uint64 offset, void* buffer, std::size_t count) {
    return read_view(&static_cast<ZipReader*>(opaque)->bytes_, offset, buffer, count);
  }

  std::string_view bytes_;
  mz_zip_archive zip_{};
  bool open_ = false;
};

// The moment stamped on a rewritten entry: 1980-01-01 00:00, the first a zip
// can state (and what the Ar-Ar template's own entries carry). miniz turns a
// time_t into the entry's fields through local time, so the time_t is made
// from local time as well and the fields come out the same in any zone.
MZ_TIME_T fixed_entry_time() {
  std::tm tm{};
  tm.tm_year = 80;
  tm.tm_mon = 0;
  tm.tm_mday = 1;
  tm.tm_isdst = -1;
  return static_cast<MZ_TIME_T>(std::mktime(&tm));
}

// The archive `source` again, entry for entry: an entry in `rewritten` is
// compressed anew from its bytes there, every other one is copied as it lies
// (its compressed bytes, its name, its time), without being unpacked.
Result<std::string> rebuild_zip(std::string_view source, const std::map<std::size_t, std::string>& rewritten) {
  ZipReader reader;
  if (auto opened = reader.open(source); !opened) return fail(opened.error());

  mz_zip_archive writer{};
  if (!mz_zip_writer_init_heap(&writer, 0, source.size() + 4096)) {
    return fail(ErrorKind::Io, "cannot start the workbook archive");
  }
  struct End {
    mz_zip_archive* zip;
    ~End() { mz_zip_writer_end(zip); }
  } end{&writer};
  const auto failed = [&](const std::string& what) {
    return fail(ErrorKind::Io, what + " (" + mz_zip_get_error_string(mz_zip_get_last_error(&writer)) + ")");
  };

  MZ_TIME_T stamp = fixed_entry_time();
  const std::size_t count = reader.count();
  for (std::size_t i = 0; i < count; ++i) {
    const auto changed = rewritten.find(i);
    if (changed == rewritten.end()) {
      if (!mz_zip_writer_add_from_zip_reader(&writer, reader.raw(), static_cast<mz_uint>(i))) {
        return failed("cannot copy entry " + std::to_string(i) + " of the template");
      }
      continue;
    }
    auto name = reader.name(i);
    if (!name) return fail(name.error());
    mz_zip_archive_file_stat was;
    if (!mz_zip_reader_file_stat(reader.raw(), static_cast<mz_uint>(i), &was)) {
      return failed("cannot read entry " + std::to_string(i) + " of the template");
    }
    const std::string& data = changed->second;
    // The entry is flagged as the template's was, so that an archive does
    // not mix kinds: sizes in the local header unless the template's entry
    // had a data descriptor after its data (general-purpose bit 3), and the
    // name marked UTF-8 only if it was (bit 11). Bits 1 and 2, which only say
    // how hard a writer compressed, are 0 (normal) from miniz.
    constexpr unsigned kHasDescriptor = 1u << 3;
    constexpr unsigned kUtf8Name = 1u << 11;
    auto flags = static_cast<mz_uint>(MZ_DEFAULT_LEVEL);
    if ((was.m_bit_flag & kHasDescriptor) == 0) flags |= static_cast<mz_uint>(MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE);
    if ((was.m_bit_flag & kUtf8Name) == 0) flags |= static_cast<mz_uint>(MZ_ZIP_FLAG_ASCII_FILENAME);
    std::string_view bytes = data;
    if (!mz_zip_writer_add_read_buf_callback(&writer, name->c_str(), &read_view, &bytes, data.size(), &stamp, nullptr,
                                             0, flags, nullptr, 0, nullptr, 0)) {
      return failed("cannot add " + *name);
    }
  }
  void* buffer = nullptr;
  std::size_t size = 0;
  if (!mz_zip_writer_finalize_heap_archive(&writer, &buffer, &size)) return failed("cannot finish the archive");
  std::string out(static_cast<const char*>(buffer), size);
  mz_free(buffer);
  return out;
}

// ===========================================================================
// XML: reading
// ===========================================================================

// Whitespace is kept: a string of spaces is a value. Elements are found by
// name, so the whitespace between them does no harm.
constexpr unsigned kParse = pugi::parse_default | pugi::parse_ws_pcdata;

Result<void> parse(pugi::xml_document& doc, std::string_view xml, const std::string& what) {
  const pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size(), kParse, pugi::encoding_utf8);
  if (!result) {
    return fail(ErrorKind::Protocol, what + " is not well-formed XML (" + result.description() + " at byte " +
                                         std::to_string(result.offset) + ")");
  }
  return {};
}

// The name without a namespace prefix: workbooks written by other programs
// may say <x:row>.
std::string_view local_name(std::string_view name) {
  const std::size_t colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

bool is_element(const pugi::xml_node& node, std::string_view local) {
  return node.type() == pugi::node_element && local_name(node.name()) == local;
}

pugi::xml_node child(const pugi::xml_node& node, std::string_view local) {
  for (const pugi::xml_node& each : node.children()) {
    if (is_element(each, local)) return each;
  }
  return {};
}

// The text of an element: its character data, CDATA included.
std::string text_of(const pugi::xml_node& node) {
  std::string out;
  for (const pugi::xml_node& each : node.children()) {
    if (each.type() == pugi::node_pcdata || each.type() == pugi::node_cdata) out += each.value();
  }
  return out;
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// The UTF-16 code unit HHHH when `text` begins with _xHHHH_: the escape a
// workbook's strings use for characters XML cannot hold, which its readers
// decode wherever they meet it.
std::optional<unsigned> string_escape(std::string_view text) {
  if (text.size() < 7 || text[0] != '_' || text[1] != 'x' || text[6] != '_') return std::nullopt;
  unsigned unit = 0;
  for (std::size_t i = 2; i < 6; ++i) {
    const int digit = hex_digit(text[i]);
    if (digit < 0) return std::nullopt;
    unit = unit * 16 + static_cast<unsigned>(digit);
  }
  return unit;
}

void append_utf8(std::string& out, char32_t code) {
  if (code < 0x80) {
    out += static_cast<char>(code);
  } else if (code < 0x800) {
    out += static_cast<char>(0xC0 | (code >> 6));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else if (code < 0x10000) {
    out += static_cast<char>(0xE0 | (code >> 12));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (code >> 18));
    out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  }
}

// A stored string with its _xHHHH_ escapes decoded. Two escapes that are a
// surrogate pair are one character; half a pair is left as it is written.
std::string decode_escapes(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const auto unit = text[i] == '_' ? string_escape(text.substr(i)) : std::nullopt;
    if (!unit) {
      out += text[i++];
      continue;
    }
    const bool high = *unit >= 0xD800 && *unit <= 0xDBFF;
    const bool low = *unit >= 0xDC00 && *unit <= 0xDFFF;
    if (high) {
      const auto next = string_escape(text.substr(i + 7));
      if (next && *next >= 0xDC00 && *next <= 0xDFFF) {
        append_utf8(out, 0x10000 + ((static_cast<char32_t>(*unit) - 0xD800) << 10) + (*next - 0xDC00));
        i += 14;
        continue;
      }
    }
    if (high || low) {
      out += text[i++];
      continue;
    }
    append_utf8(out, *unit);
    i += 7;
  }
  return out;
}

// The text of a string item (<si> or <is>): its <t>, or the <t> of each run
// of a rich text, in order. Phonetic runs (<rPh>) are not part of the text.
std::string string_item(const pugi::xml_node& item) {
  std::string out;
  for (const pugi::xml_node& each : item.children()) {
    if (is_element(each, "t")) {
      out += decode_escapes(text_of(each));
    } else if (is_element(each, "r")) {
      for (const pugi::xml_node& part : each.children()) {
        if (is_element(part, "t")) out += decode_escapes(text_of(part));
      }
    }
  }
  return out;
}

std::optional<int> parse_int(std::string_view text) {
  int value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

struct CellRef {
  int column = 0;
  int row = 0;
};

// "BP4" -> (68, 4). None for anything that is not a cell of a sheet.
std::optional<CellRef> parse_cell_ref(std::string_view text) {
  std::size_t i = 0;
  int column = 0;
  while (i < text.size() && text[i] >= 'A' && text[i] <= 'Z') {
    if (i == 3) return std::nullopt;
    column = column * 26 + (text[i] - 'A' + 1);
    ++i;
  }
  if (i == 0 || i == text.size() || text[i] == '0') return std::nullopt;
  for (std::size_t j = i; j < text.size(); ++j) {
    if (text[j] < '0' || text[j] > '9') return std::nullopt;
  }
  const auto row = parse_int(text.substr(i));
  if (!row || *row < 1 || *row > kMaxRow || column > kMaxColumn) return std::nullopt;
  return CellRef{column, *row};
}

// "worksheets/sheet1.xml", as xl/workbook.xml's relationships give it, to the
// entry's name in the archive. Only ever looked up among the archive's
// entries: nothing is read from or written to the file system by a part's
// name. None when the target climbs out of the archive.
std::optional<std::string> part_name(std::string_view target) {
  std::vector<std::string_view> segments;
  if (!target.empty() && target.front() == '/') {
    target.remove_prefix(1);
  } else {
    segments.emplace_back("xl");
  }
  while (!target.empty()) {
    const std::size_t slash = target.find('/');
    const std::string_view segment = target.substr(0, slash);
    target.remove_prefix(slash == std::string_view::npos ? target.size() : slash + 1);
    if (segment.empty() || segment == ".") continue;
    if (segment == "..") {
      if (segments.empty()) return std::nullopt;
      segments.pop_back();
      continue;
    }
    segments.push_back(segment);
  }
  std::string out;
  for (const std::string_view segment : segments) {
    if (!out.empty()) out += '/';
    out += segment;
  }
  return out;
}

// ===========================================================================
// XML: the bytes of a sheet
// ===========================================================================
//
// pugixml says where an element starts in the text it parsed; from there the
// tags are walked here, so that an edit can be made to the text itself. The
// text has already been parsed, so it is well-formed; the walkers still check
// every index, and the edited sheet is parsed once more before it is kept.

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

struct RawAttribute {
  std::size_t begin = 0;        // the whitespace before the name
  std::string_view name;
  std::size_t value_begin = 0;  // inside the quotes
  std::size_t value_end = 0;
  std::size_t end = 0;          // past the closing quote
};

struct RawElement {
  std::size_t begin = 0;      // '<'
  std::size_t tag_end = 0;    // past the start tag's '>'
  std::size_t close = 0;      // '<' of the end tag; tag_end when self-closing
  std::size_t end = 0;        // past the whole element
  bool self_closing = false;
};

// The start tag at `lt`: where it ends and whether it closes itself.
std::optional<RawElement> raw_tag(std::string_view xml, std::size_t lt) {
  if (lt >= xml.size() || xml[lt] != '<') return std::nullopt;
  char quote = 0;
  for (std::size_t i = lt + 1; i < xml.size(); ++i) {
    const char c = xml[i];
    if (quote != 0) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      RawElement out;
      out.begin = lt;
      out.tag_end = i + 1;
      out.self_closing = xml[i - 1] == '/';
      out.close = out.tag_end;
      out.end = out.tag_end;
      return out;
    }
  }
  return std::nullopt;
}

// The whole element `node` in `xml`. For an element with an end tag, that tag
// is the first of its name after the start tag: right for the elements this
// is asked about (sheetData, row, c, dimension), none of which nests in
// itself.
std::optional<RawElement> raw_element(std::string_view xml, const pugi::xml_node& node) {
  const std::ptrdiff_t offset = node.offset_debug();
  const std::string_view name = node.name();
  if (offset < 1 || name.empty()) return std::nullopt;
  const auto lt = static_cast<std::size_t>(offset) - 1;
  if (lt + 1 + name.size() > xml.size() || xml.substr(lt + 1, name.size()) != name) return std::nullopt;
  auto element = raw_tag(xml, lt);
  if (!element || element->self_closing) return element;

  const std::string closing = "</" + std::string(name);
  std::size_t at = element->tag_end;
  while (true) {
    at = xml.find(closing, at);
    if (at == std::string_view::npos) return std::nullopt;
    std::size_t i = at + closing.size();
    while (i < xml.size() && is_space(xml[i])) ++i;
    if (i < xml.size() && xml[i] == '>') {
      element->close = at;
      element->end = i + 1;
      return element;
    }
    at += closing.size();
  }
}

// The attributes of the start tag `[element.begin, element.tag_end)`.
std::vector<RawAttribute> raw_attributes(std::string_view xml, const RawElement& element) {
  std::vector<RawAttribute> out;
  const std::size_t stop = element.tag_end;
  std::size_t i = element.begin + 1;
  while (i < stop && !is_space(xml[i]) && xml[i] != '/' && xml[i] != '>') ++i;  // the element's name
  while (i < stop) {
    RawAttribute attribute;
    attribute.begin = i;
    while (i < stop && is_space(xml[i])) ++i;
    if (i >= stop || xml[i] == '/' || xml[i] == '>') break;
    const std::size_t name_begin = i;
    while (i < stop && !is_space(xml[i]) && xml[i] != '=') ++i;
    attribute.name = xml.substr(name_begin, i - name_begin);
    while (i < stop && is_space(xml[i])) ++i;
    if (i >= stop || xml[i] != '=') break;
    ++i;
    while (i < stop && is_space(xml[i])) ++i;
    if (i >= stop || (xml[i] != '"' && xml[i] != '\'')) break;
    const char quote = xml[i];
    attribute.value_begin = ++i;
    while (i < stop && xml[i] != quote) ++i;
    if (i >= stop) break;
    attribute.value_end = i;
    attribute.end = ++i;
    out.push_back(attribute);
  }
  return out;
}

// ===========================================================================
// XML: writing
// ===========================================================================

std::string hex(char32_t code) {
  constexpr std::string_view digits = "0123456789ABCDEF";
  std::string out;
  do {
    out.insert(out.begin(), digits[code & 0xF]);
    code >>= 4;
  } while (code != 0);
  while (out.size() < 4) out.insert(out.begin(), '0');
  return "U+" + out;
}

// Empty when `text` is UTF-8 of characters XML 1.0 allows (production [2]
// Char: tab, line feed, return, and everything from U+0020 except the
// surrogates, U+FFFE and U+FFFF); else what is wrong with it.
std::string forbidden_in_xml(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    std::size_t length = 1;
    char32_t code = lead;
    char32_t least = 0;
    if (lead >= 0x80) {
      if ((lead & 0xE0) == 0xC0) {
        length = 2;
        code = lead & 0x1Fu;
        least = 0x80;
      } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        code = lead & 0x0Fu;
        least = 0x800;
      } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        code = lead & 0x07u;
        least = 0x10000;
      } else {
        return "text that is not UTF-8";
      }
      if (i + length > text.size()) return "text that is not UTF-8";
      for (std::size_t k = 1; k < length; ++k) {
        const auto next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xC0) != 0x80) return "text that is not UTF-8";
        code = (code << 6) | (next & 0x3Fu);
      }
      if (code < least || code > 0x10FFFF) return "text that is not UTF-8";
    }
    const bool allowed = code == 0x9 || code == 0xA || code == 0xD || (code >= 0x20 && code <= 0xD7FF) ||
                         (code >= 0xE000 && code <= 0xFFFD) || code >= 0x10000;
    if (!allowed) {
      if (code >= 0xD800 && code <= 0xDFFF) return "text that is not UTF-8";
      return "a character XML does not allow (" + hex(code) + ")";
    }
    i += length;
  }
  return {};
}

// Element content. A return is written as a reference: a parser turns a
// literal one into a line feed. An underscore that begins what a reader
// would take for an escape (_xHHHH_) is written as the escape of itself.
void append_text(std::string& out, std::string_view text) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '_' && string_escape(text.substr(i))) {
      out += "_x005F_";
      continue;
    }
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\r': out += "&#13;"; break;
      default: out += c;
    }
  }
}

// An attribute value in double quotes.
void append_attribute_value(std::string& out, std::string_view text) {
  for (const char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '"': out += "&quot;"; break;
      case '\t': out += "&#9;"; break;
      case '\n': out += "&#10;"; break;
      case '\r': out += "&#13;"; break;
      default: out += c;
    }
  }
}

// A finite number as a cell's <v>: the shortest digits that read back as
// `value`, never the locale's. Plain decimal notation while the decimal
// exponent is between -5 and 15 (100000, 0.0001, 123456789012345), and
// scientific outside, without a '+' (1e21, 1.5e-07). Zero of either sign
// is 0.
std::string number_text(double value) {
  if (value == 0.0) return "0";
  std::array<char, 64> buffer{};
  char* const stop = buffer.data() + buffer.size();
  const auto scientific = std::to_chars(buffer.data(), stop, value, std::chars_format::scientific);
  if (scientific.ec != std::errc{}) return {};
  std::string text(buffer.data(), scientific.ptr);
  const std::size_t e = text.find('e');
  if (e == std::string::npos) return {};
  std::string_view digits = std::string_view(text).substr(e + 1);
  if (!digits.empty() && digits.front() == '+') digits.remove_prefix(1);
  const auto exponent = parse_int(digits);
  if (!exponent) return {};
  if (*exponent >= -5 && *exponent <= 15) {
    const auto fixed = std::to_chars(buffer.data(), stop, value, std::chars_format::fixed);
    return fixed.ec == std::errc{} ? std::string(buffer.data(), fixed.ptr) : std::string();
  }
  if (text[e + 1] == '+') text.erase(e + 1, 1);
  return text;
}

std::string column_letters(int column) {
  std::string out;
  while (column > 0) {
    out.insert(out.begin(), static_cast<char>('A' + (column - 1) % 26));
    column = (column - 1) / 26;
  }
  return out;
}

// A value to write, checked: its column and what goes inside its <c>.
struct NewCell {
  int column = 0;
  bool text = false;     // an inline string; else a number
  std::string body;      // <is>..</is> or <v>..</v>, in the sheet's prefix
  std::string expected;  // what the cell reads as once written
};

struct NewRow {
  int row = 0;
  std::vector<NewCell> cells;  // in column order, none empty
};

// The element names of a sheet, which carry the sheet's namespace prefix
// when it has one.
struct Names {
  std::string row, c, v, is, t;
  explicit Names(std::string_view prefix)
      : row(std::string(prefix) + "row"),
        c(std::string(prefix) + "c"),
        v(std::string(prefix) + "v"),
        is(std::string(prefix) + "is"),
        t(std::string(prefix) + "t") {}
};

std::string cell_body(const Cell& cell, const Names& names) {
  std::string out;
  if (const auto* number = std::get_if<double>(&cell.value)) {
    out = "<" + names.v + ">" + number_text(*number) + "</" + names.v + ">";
  } else if (const auto* text = std::get_if<std::string>(&cell.value)) {
    out = "<" + names.is + "><" + names.t + " xml:space=\"preserve\">";
    append_text(out, *text);
    out += "</" + names.t + "></" + names.is + ">";
  }
  return out;
}

// <c r="B5" [the template cell's other attributes] [t="inlineStr"]>body</c>
void append_cell(std::string& out, const NewCell& cell, int row, const Names& names, const pugi::xml_node& styled) {
  out += "<" + names.c + " r=\"" + cell_reference(cell.column, row) + "\"";
  for (const pugi::xml_attribute& attribute : styled.attributes()) {
    const std::string_view name = attribute.name();
    if (name == "r" || name == "t") continue;
    out += ' ';
    out += name;
    out += "=\"";
    append_attribute_value(out, attribute.value());
    out += '"';
  }
  if (cell.text) out += " t=\"inlineStr\"";
  out += ">" + cell.body + "</" + names.c + ">";
}

std::string new_row(const NewRow& row, const Names& names) {
  std::string out = "<" + names.row + " r=\"" + std::to_string(row.row) + "\">";
  for (const NewCell& cell : row.cells) append_cell(out, cell, row.row, names, {});
  out += "</" + names.row + ">";
  return out;
}

bool has_value(const pugi::xml_node& cell) {
  return child(cell, "v") || child(cell, "is") || child(cell, "f");
}

// "A1:V927" or "A1".
struct Area {
  int left = 0, top = 0, right = 0, bottom = 0;
};

std::optional<Area> parse_area(std::string_view text) {
  const std::size_t colon = text.find(':');
  const auto first = parse_cell_ref(text.substr(0, colon));
  const auto last = colon == std::string_view::npos ? first : parse_cell_ref(text.substr(colon + 1));
  if (!first || !last) return std::nullopt;
  return Area{first->column, first->row, last->column, last->row};
}

struct Edit {
  std::size_t at = 0;
  std::size_t length = 0;  // bytes of the template replaced
  std::string text;
};

// ===========================================================================
// XML: the values of a row
// ===========================================================================

// The number of the row `node`: its `r`, or one more than the row before it
// when it has none. A number no sheet has is an error, not a number.
Result<int> row_number(const pugi::xml_node& node, int before, const std::string& where) {
  if (const pugi::xml_attribute r = node.attribute("r"); r) {
    const auto number = parse_int(r.value());
    if (!number || *number < 1 || *number > kMaxRow) {
      return fail(ErrorKind::Protocol, where + " has a row numbered '" + std::string(r.value()) + "'");
    }
    return *number;
  }
  if (before < 0 || before >= kMaxRow) return fail(ErrorKind::Protocol, where + " has a row past the last of a sheet");
  return before + 1;
}

// One string per column of the row `node` (numbered `row`), from A to its
// last cell that holds something. A cell without a reference is the one
// after the cell before it.
Result<std::vector<std::string>> read_cells(const pugi::xml_node& node, int row,
                                            const std::vector<std::string>& shared_strings, const std::string& where) {
  std::vector<std::string> out;
  int column = 0;
  for (const pugi::xml_node& cell : node.children()) {
    if (!is_element(cell, "c")) continue;
    if (const pugi::xml_attribute r = cell.attribute("r"); r) {
      const auto ref = parse_cell_ref(r.value());
      if (!ref) {
        return fail(ErrorKind::Protocol, where + " row " + std::to_string(row) + " has a cell placed at '" +
                                             std::string(r.value()) + "'");
      }
      column = ref->column;
    } else {
      if (column >= kMaxColumn) {
        return fail(ErrorKind::Protocol, where + " row " + std::to_string(row) + " has a cell past the last column");
      }
      ++column;
    }

    std::string value;
    const std::string_view type = cell.attribute("t").value();
    if (type == "inlineStr") {
      value = string_item(child(cell, "is"));
    } else if (const pugi::xml_node v = child(cell, "v"); v) {
      value = text_of(v);
      if (type == "s") {
        const auto index = parse_int(value);
        if (!index || *index < 0 || static_cast<std::size_t>(*index) >= shared_strings.size()) {
          return fail(ErrorKind::Protocol, where + " cell " + cell_reference(column, row) +
                                               " names a shared string the workbook has not");
        }
        value = shared_strings[static_cast<std::size_t>(*index)];
      }
    }
    if (value.empty()) continue;
    if (out.size() < static_cast<std::size_t>(column)) out.resize(static_cast<std::size_t>(column));
    out[static_cast<std::size_t>(column) - 1] = std::move(value);
  }
  return out;
}

// ===========================================================================
// XML: what a sheet says about its columns
// ===========================================================================

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

// "$C$2:$C$17", "C2" or "A2:B9" as an area; the `$` only say how a reference
// behaves when it is copied.
std::optional<Area> parse_reference(std::string_view text) {
  std::string plain;
  for (const char c : text) {
    if (c != '$') plain += c;
  }
  return parse_area(plain);
}

// The columns of a validation's `sqref` ("N5 P5 R5", "BC5:BE5", "C5:C883"),
// each once and in order. What is not an area of cells counts for nothing.
std::vector<int> columns_of(std::string_view sqref) {
  std::vector<int> out;
  while (!sqref.empty()) {
    const std::size_t space = sqref.find_first_of(" \t\r\n");
    const auto area = parse_reference(sqref.substr(0, space));
    sqref.remove_prefix(space == std::string_view::npos ? sqref.size() : space + 1);
    if (!area) continue;
    for (int column = std::min(area->left, area->right); column <= std::max(area->left, area->right); ++column) {
      out.push_back(column);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// `"Yes,No,N/A"`: the entries between the quotes, a doubled quote being one.
std::vector<std::string> inline_entries(std::string_view quoted) {
  std::string text;
  for (std::size_t i = 1; i + 1 < quoted.size(); ++i) {
    text += quoted[i];
    if (quoted[i] == '"' && i + 2 < quoted.size() && quoted[i + 1] == '"') ++i;
  }
  std::vector<std::string> out;
  std::string_view rest = text;
  while (true) {
    const std::size_t comma = rest.find(',');
    const std::string_view entry = trimmed(rest.substr(0, comma));
    if (!entry.empty()) out.emplace_back(entry);
    if (comma == std::string_view::npos) break;
    rest.remove_prefix(comma + 1);
  }
  return out;
}

// Fills in where the list of `formula` is: cells of one column, on the sheet
// it names or on `own_sheet`; entries in quotes; or a name the workbook
// defines as either (followed a few names deep, so that a name defined as
// itself ends). Anything else leaves the validation unresolved.
void resolve_list(ListValidation& out, std::string_view formula, std::string_view own_sheet,
                  const std::map<std::string, std::string, std::less<>>& defined_names, int depth = 0) {
  formula = trimmed(formula);
  if (!formula.empty() && formula.front() == '=') formula = trimmed(formula.substr(1));
  if (formula.size() >= 2 && formula.front() == '"' && formula.back() == '"') {
    out.source = ListValidation::Source::Inline;
    out.entries = inline_entries(formula);
    return;
  }

  // 'Lookup Tables'!$C$2:$C$17, Lists!A1:A9, $K$2:$K$7.
  std::string sheet(own_sheet);
  std::string_view cells = formula;
  if (const std::size_t bang = formula.rfind('!'); bang != std::string_view::npos) {
    std::string_view name = formula.substr(0, bang);
    cells = formula.substr(bang + 1);
    sheet.clear();
    if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'') {
      name = name.substr(1, name.size() - 2);
      for (std::size_t i = 0; i < name.size(); ++i) {
        sheet += name[i];
        if (name[i] == '\'' && i + 1 < name.size() && name[i + 1] == '\'') ++i;
      }
    } else {
      sheet = std::string(name);
    }
  }
  if (const auto area = parse_reference(cells); area && area->left == area->right && !sheet.empty()) {
    out.source = ListValidation::Source::Range;
    out.sheet = std::move(sheet);
    out.column = area->left;
    out.first_row = std::min(area->top, area->bottom);
    out.last_row = std::max(area->top, area->bottom);
    return;
  }
  if (const auto named = defined_names.find(formula); named != defined_names.end() && depth < 4) {
    resolve_list(out, named->second, own_sheet, defined_names, depth + 1);
  }
}

// One <dataValidation> of either kind: `sqref` is an attribute or a child
// (xm:sqref), the formula the text of <formula1> or of the <xm:f> in it.
ListValidation list_validation(const pugi::xml_node& node, std::string_view own_sheet,
                               const std::map<std::string, std::string, std::less<>>& defined_names) {
  ListValidation out;
  std::string sqref = node.attribute("sqref").value();
  if (const pugi::xml_node element = child(node, "sqref"); element) sqref = text_of(element);
  out.columns = columns_of(sqref);
  const pugi::xml_node formula = child(node, "formula1");
  const pugi::xml_node inner = child(formula, "f");
  out.formula = std::string(trimmed(text_of(inner ? inner : formula)));
  resolve_list(out, out.formula, own_sheet, defined_names);
  return out;
}

// The fill colour of each cell style (cellXfs, by index), for cell_fill.
std::vector<std::string> fills_of(const pugi::xml_node& style_sheet) {
  std::vector<std::string> fills;
  for (const pugi::xml_node& fill : child(style_sheet, "fills").children()) {
    if (!is_element(fill, "fill")) continue;
    std::string colour;
    const pugi::xml_node pattern = child(fill, "patternFill");
    const pugi::xml_node fg = child(pattern, "fgColor");
    if (std::string_view(pattern.attribute("patternType").value()) == "solid" && fg) {
      if (const pugi::xml_attribute rgb = fg.attribute("rgb"); rgb) {
        colour = rgb.value();
      } else if (const pugi::xml_attribute theme = fg.attribute("theme"); theme) {
        colour = std::string("theme ") + theme.value();
        if (const pugi::xml_attribute tint = fg.attribute("tint"); tint) colour += std::string(" tint ") + tint.value();
      } else if (const pugi::xml_attribute indexed = fg.attribute("indexed"); indexed) {
        colour = std::string("indexed ") + indexed.value();
      }
    }
    fills.push_back(std::move(colour));
  }
  std::vector<std::string> out;
  for (const pugi::xml_node& xf : child(style_sheet, "cellXfs").children()) {
    if (!is_element(xf, "xf")) continue;
    const auto fill = parse_int(xf.attribute("fillId").value());
    out.push_back(fill && *fill >= 0 && static_cast<std::size_t>(*fill) < fills.size()
                      ? fills[static_cast<std::size_t>(*fill)]
                      : std::string());
  }
  return out;
}

}  // namespace

// ===========================================================================
// Public
// ===========================================================================

Result<std::vector<ZipEntry>> read_zip(const std::filesystem::path& file) {
  auto bytes = read_file(file);
  if (!bytes) return fail(bytes.error());
  ZipReader reader;
  if (auto opened = reader.open(*bytes); !opened) {
    return fail(ErrorKind::Protocol, file.string() + ": " + opened.error().what);
  }
  std::vector<ZipEntry> out;
  std::size_t total = 0;
  for (std::size_t i = 0; i < reader.count(); ++i) {
    auto name = reader.name(i);
    auto data = reader.extract(i);
    auto stored = reader.extract(i, true);
    for (const Error* error : {name ? nullptr : &name.error(), data ? nullptr : &data.error(),
                               stored ? nullptr : &stored.error()}) {
      if (error != nullptr) return fail(ErrorKind::Protocol, file.string() + ": " + error->what);
    }
    total += data->size();
    if (total > kMaxArchiveBytes) return fail(ErrorKind::Protocol, file.string() + ": the archive unpacks to too much");
    out.push_back({std::move(*name), std::move(*data), std::move(*stored)});
  }
  return out;
}

std::string cell_reference(int column, int row) {
  if (column < 1 || column > kMaxColumn || row < 1 || row > kMaxRow) return {};
  return column_letters(column) + std::to_string(row);
}

Result<Workbook> Workbook::open(const std::filesystem::path& template_file) {
  auto bytes = read_file(template_file);
  if (!bytes) return fail(bytes.error());
  const std::string where = template_file.string();
  const auto not_a_workbook = [&](const std::string& what) {
    return fail(ErrorKind::Protocol, where + " is not a workbook: " + what);
  };

  ZipReader reader;
  if (auto opened = reader.open(*bytes); !opened) return not_a_workbook(opened.error().what);
  std::map<std::string, std::size_t, std::less<>> index;  // the first entry of a name, as a reader takes it
  for (std::size_t i = 0; i < reader.count(); ++i) {
    auto name = reader.name(i);
    if (!name) return not_a_workbook(name.error().what);
    // Which of two entries of one name is the part depends on who reads the
    // archive: such a file is not taken.
    if (index.contains(*name)) return not_a_workbook("it has two entries named " + *name);
    index.emplace(std::move(*name), i);
  }
  const auto part = [&](std::string_view name) -> Result<std::string> {
    const auto found = index.find(name);
    if (found == index.end()) return fail(ErrorKind::Protocol, "it has no " + std::string(name));
    return reader.extract(found->second);
  };

  // xl/workbook.xml names the sheets and gives each a relationship id;
  // xl/_rels/workbook.xml.rels says which part an id is.
  const auto workbook_xml = part("xl/workbook.xml");
  if (!workbook_xml) return not_a_workbook(workbook_xml.error().what);
  const auto rels_xml = part("xl/_rels/workbook.xml.rels");
  if (!rels_xml) return not_a_workbook(rels_xml.error().what);
  pugi::xml_document workbook;
  if (auto parsed = parse(workbook, *workbook_xml, "xl/workbook.xml"); !parsed) {
    return not_a_workbook(parsed.error().what);
  }
  pugi::xml_document rels;
  if (auto parsed = parse(rels, *rels_xml, "xl/_rels/workbook.xml.rels"); !parsed) {
    return not_a_workbook(parsed.error().what);
  }

  std::map<std::string, std::string, std::less<>> targets;  // relationship id -> part
  std::string shared_strings_part, styles_part;
  for (const pugi::xml_node& relationship : rels.document_element().children()) {
    if (!is_element(relationship, "Relationship")) continue;
    const auto name = part_name(relationship.attribute("Target").value());
    if (!name) continue;
    targets.emplace(relationship.attribute("Id").value(), *name);
    const std::string_view type = relationship.attribute("Type").value();
    if (type.ends_with("/sharedStrings")) shared_strings_part = *name;
    if (type.ends_with("/styles")) styles_part = *name;
  }

  Workbook book;
  const pugi::xml_node root = workbook.document_element();
  if (!is_element(root, "workbook")) return not_a_workbook("xl/workbook.xml is not a workbook part");
  for (const pugi::xml_node& sheet : child(root, "sheets").children()) {
    if (!is_element(sheet, "sheet")) continue;
    std::string_view id;
    for (const pugi::xml_attribute& attribute : sheet.attributes()) {
      const std::string_view name = attribute.name();
      if (name != "id" && local_name(name) == "id") id = attribute.value();  // r:id, whatever the prefix
    }
    const std::string name = sheet.attribute("name").value();
    const auto target = targets.find(id);
    if (target == targets.end()) return not_a_workbook("sheet '" + name + "' has no part");
    const auto entry = index.find(target->second);
    if (entry == index.end()) return not_a_workbook("sheet '" + name + "' is " + target->second + ", which it has not");
    book.sheets_.push_back({name, entry->second});
  }
  if (book.sheets_.empty()) return not_a_workbook("it names no sheets");
  for (const pugi::xml_node& name : child(root, "definedNames").children()) {
    if (is_element(name, "definedName")) book.defined_names_.emplace(name.attribute("name").value(), text_of(name));
  }
  // The styles are read when a fill is asked for, not here: a workbook is
  // written to without them.
  if (const auto entry = index.find(styles_part); entry != index.end()) book.styles_entry_ = entry->second;

  if (!shared_strings_part.empty()) {
    const auto xml = part(shared_strings_part);
    if (!xml) return not_a_workbook(xml.error().what);
    pugi::xml_document strings;
    if (auto parsed = parse(strings, *xml, shared_strings_part); !parsed) return not_a_workbook(parsed.error().what);
    for (const pugi::xml_node& item : strings.document_element().children()) {
      if (is_element(item, "si")) book.shared_strings_.push_back(string_item(item));
    }
  }

  book.archive_ = std::make_shared<const std::string>(std::move(*bytes));
  return book;
}

std::vector<std::string> Workbook::sheet_names() const {
  std::vector<std::string> out;
  out.reserve(sheets_.size());
  for (const Sheet& sheet : sheets_) out.push_back(sheet.name);
  return out;
}

const Workbook::Sheet* Workbook::find_sheet(std::string_view name) const {
  for (const Sheet& sheet : sheets_) {
    if (sheet.name == name) return &sheet;
  }
  return nullptr;
}

Result<std::string> Workbook::sheet_xml(const Sheet& sheet) const {
  if (const auto found = rewritten_.find(sheet.entry); found != rewritten_.end()) return found->second;
  ZipReader reader;
  if (auto opened = reader.open(*archive_); !opened) return fail(opened.error());
  auto xml = reader.extract(sheet.entry);
  if (!xml) return fail(ErrorKind::Protocol, "sheet '" + sheet.name + "': " + xml.error().what);
  return xml;
}

Result<std::vector<std::string>> Workbook::row_values(std::string_view sheet_name, int row) const {
  const Sheet* sheet = find_sheet(sheet_name);
  if (sheet == nullptr) return fail(ErrorKind::Config, "the workbook has no sheet '" + std::string(sheet_name) + "'");
  if (row < 1 || row > kMaxRow) {
    return fail(ErrorKind::Config, "sheet '" + sheet->name + "' has no row " + std::to_string(row));
  }
  const auto xml = sheet_xml(*sheet);
  if (!xml) return fail(xml.error());
  pugi::xml_document doc;
  if (auto parsed = parse(doc, *xml, "sheet '" + sheet->name + "'"); !parsed) return fail(parsed.error());
  const pugi::xml_node data = child(doc.document_element(), "sheetData");
  if (!is_element(doc.document_element(), "worksheet") || !data) {
    return fail(ErrorKind::Protocol, "sheet '" + sheet->name + "' is not a worksheet");
  }

  const std::string where = "sheet '" + sheet->name + "'";
  int at_row = 0;
  for (const pugi::xml_node& each : data.children()) {
    if (!is_element(each, "row")) continue;
    const auto number = row_number(each, at_row, where);
    if (!number) return fail(number.error());
    at_row = *number;
    if (at_row == row) return read_cells(each, row, shared_strings_, where);
  }
  return std::vector<std::string>{};
}

Result<std::vector<ListValidation>> Workbook::list_validations(std::string_view sheet_name) const {
  const Sheet* sheet = find_sheet(sheet_name);
  if (sheet == nullptr) return fail(ErrorKind::Config, "the workbook has no sheet '" + std::string(sheet_name) + "'");
  const auto xml = sheet_xml(*sheet);
  if (!xml) return fail(xml.error());
  pugi::xml_document doc;
  if (auto parsed = parse(doc, *xml, "sheet '" + sheet->name + "'"); !parsed) return fail(parsed.error());

  std::vector<ListValidation> out;
  const auto take = [&](const pugi::xml_node& validations) {
    for (const pugi::xml_node& each : validations.children()) {
      if (!is_element(each, "dataValidation")) continue;
      if (std::string_view(each.attribute("type").value()) != "list") continue;
      out.push_back(list_validation(each, sheet->name, defined_names_));
    }
  };
  for (const pugi::xml_node& each : doc.document_element().children()) {
    if (is_element(each, "dataValidations")) take(each);
    if (!is_element(each, "extLst")) continue;
    for (const pugi::xml_node& ext : each.children()) {
      if (!is_element(ext, "ext")) continue;
      for (const pugi::xml_node& inner : ext.children()) {
        if (is_element(inner, "dataValidations")) take(inner);
      }
    }
  }
  return out;
}

Result<std::vector<std::string>> Workbook::column_values(std::string_view sheet_name, int column, int first_row,
                                                         int last_row) const {
  const Sheet* sheet = find_sheet(sheet_name);
  if (sheet == nullptr) return fail(ErrorKind::Config, "the workbook has no sheet '" + std::string(sheet_name) + "'");
  const std::string where = "sheet '" + sheet->name + "'";
  if (column < 1 || column > kMaxColumn) {
    return fail(ErrorKind::Config, where + " has no column " + std::to_string(column));
  }
  if (first_row < 1 || last_row > kMaxRow || first_row > last_row) {
    return fail(ErrorKind::Config,
                where + " has no rows " + std::to_string(first_row) + " to " + std::to_string(last_row));
  }
  const auto xml = sheet_xml(*sheet);
  if (!xml) return fail(xml.error());
  pugi::xml_document doc;
  if (auto parsed = parse(doc, *xml, where); !parsed) return fail(parsed.error());
  const pugi::xml_node data = child(doc.document_element(), "sheetData");
  if (!is_element(doc.document_element(), "worksheet") || !data) {
    return fail(ErrorKind::Protocol, where + " is not a worksheet");
  }

  std::vector<std::string> out(static_cast<std::size_t>(last_row - first_row) + 1);
  int at_row = 0;
  for (const pugi::xml_node& each : data.children()) {
    if (!is_element(each, "row")) continue;
    const auto number = row_number(each, at_row, where);
    if (!number) return fail(number.error());
    at_row = *number;
    if (at_row < first_row || at_row > last_row) continue;
    auto cells = read_cells(each, at_row, shared_strings_, where);
    if (!cells) return fail(cells.error());
    if (cells->size() >= static_cast<std::size_t>(column)) {
      out[static_cast<std::size_t>(at_row - first_row)] = std::move((*cells)[static_cast<std::size_t>(column) - 1]);
    }
  }
  return out;
}

Result<std::vector<std::string>> Workbook::style_fills() const {
  if (!styles_entry_) return std::vector<std::string>{};
  ZipReader reader;
  if (auto opened = reader.open(*archive_); !opened) return fail(opened.error());
  const auto xml = reader.extract(*styles_entry_);
  if (!xml) return fail(ErrorKind::Protocol, "the workbook's styles: " + xml.error().what);
  pugi::xml_document doc;
  if (auto parsed = parse(doc, *xml, "the workbook's styles"); !parsed) return fail(parsed.error());
  return fills_of(doc.document_element());
}

Result<std::vector<std::string>> Workbook::row_fills(std::string_view sheet_name, int row) const {
  const Sheet* sheet = find_sheet(sheet_name);
  if (sheet == nullptr) return fail(ErrorKind::Config, "the workbook has no sheet '" + std::string(sheet_name) + "'");
  const std::string where = "sheet '" + sheet->name + "'";
  if (row < 1 || row > kMaxRow) return fail(ErrorKind::Config, where + " has no row " + std::to_string(row));
  const auto fills = style_fills();
  if (!fills) return fail(fills.error());
  const auto xml = sheet_xml(*sheet);
  if (!xml) return fail(xml.error());
  pugi::xml_document doc;
  if (auto parsed = parse(doc, *xml, where); !parsed) return fail(parsed.error());
  const pugi::xml_node data = child(doc.document_element(), "sheetData");
  if (!is_element(doc.document_element(), "worksheet") || !data) {
    return fail(ErrorKind::Protocol, where + " is not a worksheet");
  }

  std::vector<std::string> out;
  int at_row = 0;
  for (const pugi::xml_node& each : data.children()) {
    if (!is_element(each, "row")) continue;
    const auto number = row_number(each, at_row, where);
    if (!number) return fail(number.error());
    at_row = *number;
    if (at_row != row) continue;
    int column = 0;
    for (const pugi::xml_node& cell : each.children()) {
      if (!is_element(cell, "c")) continue;
      // Placed as read_cells places it: by its reference, else after the
      // cell before it.
      const pugi::xml_attribute r = cell.attribute("r");
      const auto ref = r ? parse_cell_ref(r.value()) : std::nullopt;
      if ((r && !ref) || (!r && column >= kMaxColumn)) {
        return fail(ErrorKind::Protocol, where + " row " + std::to_string(row) + " has a cell that has no place");
      }
      column = ref ? ref->column : column + 1;
      if (out.size() < static_cast<std::size_t>(column)) out.resize(static_cast<std::size_t>(column));
      const auto style = parse_int(cell.attribute("s").value());
      if (style && *style >= 0 && static_cast<std::size_t>(*style) < fills->size()) {
        out[static_cast<std::size_t>(column) - 1] = (*fills)[static_cast<std::size_t>(*style)];
      }
    }
    break;
  }
  return out;
}

Result<std::string> Workbook::cell_fill(std::string_view sheet_name, int row, int column) const {
  if (column < 1 || column > kMaxColumn) {
    return fail(ErrorKind::Config, "sheet '" + std::string(sheet_name) + "' has no column " + std::to_string(column));
  }
  auto fills = row_fills(sheet_name, row);
  if (!fills) return fail(fills.error());
  if (fills->size() < static_cast<std::size_t>(column)) return std::string();
  return std::move((*fills)[static_cast<std::size_t>(column) - 1]);
}

Result<void> Workbook::write_rows(std::string_view sheet_name, int first_row,
                                  const std::vector<std::vector<Cell>>& rows) {
  const Sheet* sheet = find_sheet(sheet_name);
  if (sheet == nullptr) return fail(ErrorKind::Config, "the workbook has no sheet '" + std::string(sheet_name) + "'");
  const std::string where = "sheet '" + sheet->name + "'";
  if (first_row < 2) return fail(ErrorKind::Config, where + ": rows are written from row 2 at the earliest");
  if (first_row > kMaxRow || rows.size() > static_cast<std::size_t>(kMaxRow - first_row) + 1) {
    return fail(ErrorKind::Config, where + ": the rows run past the last row of a sheet");
  }
  for (const std::vector<Cell>& row : rows) {
    if (row.size() > static_cast<std::size_t>(kMaxColumn)) {
      return fail(ErrorKind::Config, where + ": a row has more columns than a sheet");
    }
  }

  const auto xml = sheet_xml(*sheet);
  if (!xml) return fail(xml.error());
  pugi::xml_document doc;
  if (auto parsed = parse(doc, *xml, where); !parsed) return fail(parsed.error());
  const pugi::xml_node root = doc.document_element();
  const pugi::xml_node data = child(root, "sheetData");
  if (!is_element(root, "worksheet") || !data) return fail(ErrorKind::Protocol, where + " is not a worksheet");
  const auto unreadable = [&](const std::string& what) {
    return fail(ErrorKind::Protocol, where + " is not laid out as this writer needs: " + what);
  };

  const std::string_view data_name = data.name();
  const Names names(data_name.substr(0, data_name.size() - local_name(data_name).size()));

  // ---- What is to be written, checked ----
  std::vector<NewRow> wanted;
  Area written{kMaxColumn, kMaxRow, 0, 0};
  for (std::size_t i = 0; i < rows.size(); ++i) {
    NewRow row;
    row.row = first_row + static_cast<int>(i);
    for (std::size_t j = 0; j < rows[i].size(); ++j) {
      const Cell& cell = rows[i][j];
      const int column = static_cast<int>(j) + 1;
      const auto refused = [&](const std::string& what) {
        return fail(ErrorKind::Config,
                    std::string(where).append(" cell ").append(cell_reference(column, row.row)).append(": ").append(what));
      };
      if (const auto* number = std::get_if<double>(&cell.value)) {
        if (!std::isfinite(*number)) return refused("a number that is not finite cannot be written");
        row.cells.push_back({column, false, cell_body(cell, names), number_text(*number)});
      } else if (const auto* text = std::get_if<std::string>(&cell.value)) {
        if (text->empty()) continue;
        if (const std::string wrong = forbidden_in_xml(*text); !wrong.empty()) return refused("it holds " + wrong);
        row.cells.push_back({column, true, cell_body(cell, names), *text});
      }
    }
    if (row.cells.empty()) continue;
    written.left = std::min(written.left, row.cells.front().column);
    written.right = std::max(written.right, row.cells.back().column);
    written.top = std::min(written.top, row.row);
    written.bottom = std::max(written.bottom, row.row);
    wanted.push_back(std::move(row));
  }
  if (wanted.empty()) return {};

  // ---- The rows the sheet stores ----
  struct Stored {
    int row = 0;
    pugi::xml_node node;
  };
  std::vector<Stored> stored;
  for (const pugi::xml_node& each : data.children()) {
    if (each.type() != pugi::node_element) continue;
    if (!is_element(each, "row")) return unreadable("its data holds something that is not a row");
    const auto number = parse_int(each.attribute("r").value());
    if (!number || *number < 1 || *number > kMaxRow) return unreadable("a row has no number");
    if (!stored.empty() && *number <= stored.back().row) return unreadable("its rows are not in order");
    stored.push_back({*number, each});
  }

  const auto data_element = raw_element(*xml, data);
  if (!data_element) return unreadable("its data cannot be found in its text");

  // ---- The edits, in the order of the text ----
  std::vector<Edit> edits;

  // <dimension ref="A1:V927"/>, where there is one and it has to grow. Only
  // the value of `ref` changes.
  if (const pugi::xml_node dimension = child(root, "dimension"); dimension) {
    const auto area = parse_area(dimension.attribute("ref").value());
    const auto element = raw_element(*xml, dimension);
    if (area && element && element->end <= data_element->begin) {
      const Area grown{std::min(area->left, written.left), std::min(area->top, written.top),
                       std::max(area->right, written.right), std::max(area->bottom, written.bottom)};
      if (grown.left != area->left || grown.top != area->top || grown.right != area->right ||
          grown.bottom != area->bottom) {
        for (const RawAttribute& attribute : raw_attributes(*xml, *element)) {
          if (attribute.name != "ref") continue;
          edits.push_back({attribute.value_begin, attribute.value_end - attribute.value_begin,
                           cell_reference(grown.left, grown.top) + ":" + cell_reference(grown.right, grown.bottom)});
        }
      }
    }
  }

  std::string appended;  // rows for an empty <sheetData/>
  std::size_t added = 0;  // rows the sheet did not store
  std::size_t next = 0;  // the first stored row not yet passed
  for (const NewRow& row : wanted) {
    while (next < stored.size() && stored[next].row < row.row) ++next;

    // A row the sheet does not store: before the next row it does, or
    // before </sheetData>.
    if (next == stored.size() || stored[next].row != row.row) {
      ++added;
      if (data_element->self_closing) {
        appended += new_row(row, names);
        continue;
      }
      std::size_t at = data_element->close;
      if (next < stored.size()) {
        const auto after = raw_element(*xml, stored[next].node);
        if (!after) return unreadable("row " + std::to_string(stored[next].row) + " cannot be found in its text");
        at = after->begin;
      }
      edits.push_back({at, 0, new_row(row, names)});
      continue;
    }

    // A row it stores: the placeholder is filled.
    const pugi::xml_node node = stored[next].node;
    const auto element = raw_element(*xml, node);
    if (!element) return unreadable("row " + std::to_string(row.row) + " cannot be found in its text");
    struct StoredCell {
      int column = 0;
      pugi::xml_node node;
    };
    std::vector<StoredCell> cells;
    for (const pugi::xml_node& each : node.children()) {
      if (each.type() != pugi::node_element) continue;
      if (!is_element(each, "c")) {
        return unreadable("row " + std::to_string(row.row) + " holds something that is not a cell");
      }
      const auto ref = parse_cell_ref(each.attribute("r").value());
      if (!ref || ref->row != row.row) return unreadable("a cell of row " + std::to_string(row.row) + " has no place");
      if (!cells.empty() && ref->column <= cells.back().column) {
        return unreadable("the cells of row " + std::to_string(row.row) + " are not in order");
      }
      if (has_value(each)) {
        return fail(ErrorKind::Config, where + " cell " + cell_reference(ref->column, row.row) +
                                           " already holds a value: row " + std::to_string(row.row) +
                                           " is not empty, and is not written over");
      }
      cells.push_back({ref->column, each});
    }

    // The start tag as it is, less `spans`: a hint of the columns the row
    // uses, no longer true and not needed.
    std::string text;
    std::size_t from = element->begin;
    for (const RawAttribute& attribute : raw_attributes(*xml, *element)) {
      if (attribute.name != "spans") continue;
      text.append(*xml, from, attribute.begin - from);
      from = attribute.end;
    }
    text.append(*xml, from, element->tag_end - from);
    if (element->self_closing) {
      text.resize(text.size() - 2);
      while (!text.empty() && is_space(text.back())) text.pop_back();
      text += '>';
    }

    // The cells, the template's and the new ones, in column order.
    std::size_t a = 0;
    std::size_t b = 0;
    while (a < cells.size() || b < row.cells.size()) {
      const bool has_stored = a < cells.size();
      const bool has_new = b < row.cells.size();
      if (has_stored && (!has_new || cells[a].column < row.cells[b].column)) {
        const auto kept = raw_element(*xml, cells[a].node);  // as the template had it
        if (!kept) return unreadable("a cell of row " + std::to_string(row.row) + " cannot be found in its text");
        text.append(*xml, kept->begin, kept->end - kept->begin);
        ++a;
      } else if (has_stored && cells[a].column == row.cells[b].column) {
        append_cell(text, row.cells[b], row.row, names, cells[a].node);  // its style, and the value
        ++a;
        ++b;
      } else {
        append_cell(text, row.cells[b], row.row, names, {});
        ++b;
      }
    }
    text += "</" + std::string(node.name()) + ">";
    edits.push_back({element->begin, element->end - element->begin, std::move(text)});
  }

  if (data_element->self_closing && !appended.empty()) {
    std::string text = xml->substr(data_element->begin, data_element->tag_end - data_element->begin - 2);
    while (!text.empty() && is_space(text.back())) text.pop_back();
    text += ">" + appended + "</" + std::string(data_name) + ">";
    edits.push_back({data_element->begin, data_element->end - data_element->begin, std::move(text)});
  }

  // ---- The sheet's text with the edits made; all else is copied ----
  std::string out;
  std::size_t at = 0;
  for (const Edit& edit : edits) {
    if (edit.at < at || edit.at + edit.length > xml->size()) return unreadable("its rows overlap");
    out.append(*xml, at, edit.at - at);
    out += edit.text;
    at = edit.at + edit.length;
  }
  out.append(*xml, at, std::string::npos);

  // The walkers trusted the text to be what pugixml said it was. Where it is
  // not (an end tag inside a comment or CDATA is the same bytes as a real
  // one), rows have gone to the wrong place. So the edited text is parsed
  // and read as any reader would: it must be well-formed, hold the rows it
  // held and the ones added, each once and in order, and every cell written
  // must read back as what was asked. Otherwise nothing is kept.
  const auto hidden = [&](const std::string& what) {
    return unreadable(what + "; it holds markup this writer cannot see past (a tag inside a comment or CDATA)");
  };
  pugi::xml_document check;
  if (auto parsed = parse(check, out, where); !parsed) return hidden("it does not survive being written to");
  std::vector<Stored> now;
  for (const pugi::xml_node& each : child(check.document_element(), "sheetData").children()) {
    if (each.type() != pugi::node_element) continue;
    const auto number = is_element(each, "row") ? parse_int(each.attribute("r").value()) : std::nullopt;
    if (!number || (!now.empty() && *number <= now.back().row)) return hidden("its rows do not come out in order");
    now.push_back({*number, each});
  }
  if (now.size() != stored.size() + added) return hidden("its rows do not come out as they went in");
  for (const NewRow& row : wanted) {
    const auto found = std::lower_bound(now.begin(), now.end(), row.row,
                                        [](const Stored& each, int number) { return each.row < number; });
    if (found == now.end() || found->row != row.row) {
      return hidden("row " + std::to_string(row.row) + " is not there once written");
    }
    const auto values = read_cells(found->node, row.row, shared_strings_, where);
    if (!values) return hidden("row " + std::to_string(row.row) + " does not read once written");
    for (const NewCell& cell : row.cells) {
      const auto at_column = static_cast<std::size_t>(cell.column) - 1;
      if (at_column >= values->size() || (*values)[at_column] != cell.expected) {
        return hidden("cell " + cell_reference(cell.column, row.row) + " does not read back as it was written");
      }
    }
  }

  rewritten_[sheet->entry] = std::move(out);
  return {};
}

Result<void> Workbook::save(const std::filesystem::path& out) const {
  auto bytes = rebuild_zip(*archive_, rewritten_);
  if (!bytes) return fail(bytes.error());
  // Written beside the destination and renamed over it, so that a write that
  // fails leaves no half of a workbook, and an older one as it was.
  std::filesystem::path partial = out;
  partial += ".part";
  const auto failed = [&](const std::string& why) {
    std::error_code ignored;
    std::filesystem::remove(partial, ignored);
    return fail(ErrorKind::Io, "cannot write " + out.string() + why);
  };
  {
    std::ofstream file(partial, std::ios::binary | std::ios::trunc);
    if (!file) return fail(ErrorKind::Io, "cannot write " + out.string());
    file.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
    file.close();
    if (file.fail()) return failed("");
  }
  std::error_code ec;
  std::filesystem::rename(partial, out, ec);
  if (ec) return failed(": " + ec.message());
  // A workbook is for the user to upload: not to be quarantined (macOS).
  mark_as_user_file(out);
  return {};
}

}  // namespace pychron::earthbank::ooxml
