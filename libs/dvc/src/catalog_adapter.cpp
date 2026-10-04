#include "pychron/dvc/catalog_adapter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "legacy_json.hpp"
#include "pychron/core/calendar.hpp"
#include "pychron/core/path_text.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/ingest/conflict_markers.hpp"
#include "pychron/ingest/tz.hpp"

namespace pychron::dvc {

namespace {

namespace P = pychron::persistence;

constexpr const char* kManifestFile = "MANIFEST.json";
constexpr std::string_view kBinarySuffix = "__base64";  // tools/legacy_dump_to_jsonl.py, BINARY_SUFFIX

// The catalog tables in the order their rows are sent: parents first.
enum TableIndex : std::size_t {
  kPi,
  kProject,
  kMaterial,
  kSample,
  kIrradiation,
  kLevel,
  kPosition,
  kUser,
  kMassSpec,
  kExtractDevice,
  kLoad,
  kLoadPosition,
  kTableCount
};

struct TableInfo {
  const char* name;
  const char* key;   // the primary key column
  const char* link;  // the column of the link a row is imported without when it is broken; nullptr: none
};

constexpr std::array<TableInfo, kTableCount> kTables{{
    {"PrincipalInvestigatorTbl", "id", nullptr},
    {"ProjectTbl", "id", "principal_investigatorID"},
    {"MaterialTbl", "id", nullptr},
    {"SampleTbl", "id", "materialID"},
    {"IrradiationTbl", "id", nullptr},
    {"LevelTbl", "id", nullptr},
    {"IrradiationPositionTbl", "id", "sampleID"},
    {"UserTbl", "name", nullptr},
    {"MassSpectrometerTbl", "name", nullptr},
    {"ExtractDeviceTbl", "name", nullptr},
    {"LoadTbl", "name", "username"},
    {"LoadPositionTbl", "id", nullptr},
}};

// ---------------------------------------------------------------- text

std::string lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// A string key as MySQL's default collations (latin1_swedish_ci,
// utf8_general_ci) compare it: without regard to case or trailing spaces.
// Folding ASCII letters is enough for the keys the legacy tables use (user,
// load and spectrometer names, identifiers); the collations also equate
// accented letters, which this does not.
std::string fold(std::string_view key) {
  while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
  return lower(std::string(key));
}

bool is_utf8(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t extra = 0;
    std::uint32_t point = 0, least = 0;
    if (c < 0x80) {
      ++i;
      continue;
    }
    if (c >= 0xC2 && c <= 0xDF) {
      extra = 1;
      point = c & 0x1Fu;
      least = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
      extra = 2;
      point = c & 0x0Fu;
      least = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
      extra = 3;
      point = c & 0x07u;
      least = 0x10000;
    } else {
      return false;
    }
    if (s.size() - i <= extra) return false;
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto t = static_cast<unsigned char>(s[i + k]);
      if ((t & 0xC0u) != 0x80u) return false;
      point = (point << 6) | (t & 0x3Fu);
    }
    if (point < least || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return false;
    i += extra + 1;
  }
  return true;
}

// Bytes that are text: UTF-8 as it is, anything else read as ISO-8859-1.
std::string text_of(std::string bytes) {
  if (is_utf8(bytes)) return bytes;
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const char raw : bytes) {
    const auto c = static_cast<unsigned char>(raw);
    if (c < 0x80) {
      out += raw;
    } else {
      out += static_cast<char>(0xC0u | (c >> 6));
      out += static_cast<char>(0x80u | (c & 0x3Fu));
    }
  }
  return out;
}

std::optional<std::string> base64_decode(std::string_view text) {
  std::string out;
  std::uint32_t bits = 0;
  int count = 0;
  std::size_t padding = 0;
  for (const char c : text) {
    int value = 0;
    if (c >= 'A' && c <= 'Z') {
      value = c - 'A';
    } else if (c >= 'a' && c <= 'z') {
      value = c - 'a' + 26;
    } else if (c >= '0' && c <= '9') {
      value = c - '0' + 52;
    } else if (c == '+') {
      value = 62;
    } else if (c == '/') {
      value = 63;
    } else if (c == '=') {
      ++padding;
      continue;
    } else {
      return std::nullopt;
    }
    if (padding > 0) return std::nullopt;  // data after padding
    bits = (bits << 6) | static_cast<std::uint32_t>(value);
    if (++count == 4) {
      out += static_cast<char>((bits >> 16) & 0xFFu);
      out += static_cast<char>((bits >> 8) & 0xFFu);
      out += static_cast<char>(bits & 0xFFu);
      bits = 0;
      count = 0;
    }
  }
  if (count == 1 || padding > 2) return std::nullopt;
  if (count == 2) out += static_cast<char>((bits >> 4) & 0xFFu);
  if (count == 3) {
    out += static_cast<char>((bits >> 10) & 0xFFu);
    out += static_cast<char>((bits >> 2) & 0xFFu);
  }
  return out;
}

bool is_zero_date(std::string_view text) { return text.substr(0, 10) == "0000-00-00"; }

// A uuid with dashes or, as the legacy String(32) column holds it, without.
std::optional<P::Uuid> parse_uuid(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '{')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '}')) text.remove_suffix(1);
  if (text.size() != 32) return P::Uuid::parse(text);
  std::string dashed;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i == 8 || i == 12 || i == 16 || i == 20) dashed += '-';
    dashed += text[i];
  }
  return P::Uuid::parse(dashed);
}

// PostgreSQL's jsonb has no NUL character: a conflict's detail spells it out.
void spell_nul(Json& j) {
  if (j.is_string()) {
    auto& s = j.get_ref<std::string&>();
    for (std::size_t at = s.find('\0'); at != std::string::npos; at = s.find('\0', at + 2)) s.replace(at, 1, "\\0");
  } else if (j.is_structured()) {
    for (auto& child : j) spell_nul(child);
  }
}

// ---------------------------------------------------------------- files

struct ManifestTable {
  std::string name;  // as the dump spells it
  std::string file;
  std::int64_t rows = 0;
  std::map<std::string, std::string, std::less<>> types;  // column -> first word of its type, lower case
};

struct Manifest {
  std::string sha256;
  // TIMESTAMP columns were dumped in UTC (the dump set its session zone to +00:00).
  bool utc_timestamps = false;
  // The dump ends with mysqldump's "-- Dump completed" line; true when the manifest does not say.
  bool dump_completed = true;
  std::vector<ManifestTable> tables;

  const ManifestTable* find(std::string_view name) const {
    const std::string wanted = lower(std::string(name));
    for (const auto& table : tables)
      if (lower(table.name) == wanted) return &table;
    return nullptr;
  }
};

Result<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + utf8(path));
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) return fail(ErrorKind::Io, "cannot read " + utf8(path));
  return text;
}

Result<Manifest> read_manifest(const std::filesystem::path& dir) {
  const auto path = dir / kManifestFile;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return fail(ErrorKind::Io, utf8(dir) + ": no " + kManifestFile +
                                   " (not the output of tools/legacy_dump_to_jsonl.py, or a conversion that did "
                                   "not finish)");
  auto text = read_file(path);
  if (!text) return fail(text.error());
  auto parsed = parse_legacy(*text);
  const auto bad = [&](const std::string& why) { return fail(ErrorKind::Protocol, utf8(path) + ": " + why); };
  if (!parsed) return bad(parsed.error().what);
  const Json& j = *parsed;
  if (!j.is_object()) return bad("not a JSON object");

  Manifest manifest;
  if (const auto it = j.find("sha256"); it != j.end() && it->is_string()) manifest.sha256 = it->get<std::string>();
  if (manifest.sha256.empty()) return bad("no sha256");
  if (const auto it = j.find("time_zone"); it != j.end() && it->is_string()) {
    const std::string zone = lower(it->get<std::string>());
    manifest.utc_timestamps = zone == "+00:00" || zone == "utc";
  }
  if (const auto it = j.find("dump_completed"); it != j.end() && it->is_boolean()) manifest.dump_completed = it->get<bool>();
  const auto tables = j.find("tables");
  if (tables == j.end() || !tables->is_object()) return bad("no tables");
  const auto files = j.find("files");
  const auto columns = j.find("columns");
  for (const auto& [name, count] : tables->items()) {
    ManifestTable table;
    table.name = name;
    if (!count.is_number_integer() || count.get<std::int64_t>() < 0) return bad("table " + name + " has no row count");
    table.rows = count.get<std::int64_t>();
    table.file = name + ".jsonl";
    if (files != j.end() && files->is_object())
      if (const auto it = files->find(name); it != files->end() && it->is_string()) table.file = it->get<std::string>();
    if (table.file.empty() || table.file.find_first_of("/\\") != std::string::npos || table.file == "..")
      return bad("table " + name + " names a file outside the directory");
    if (columns != j.end() && columns->is_object())
      if (const auto it = columns->find(name); it != columns->end() && it->is_object())
        for (const auto& [column, type] : it->items())
          if (type.is_string()) table.types.emplace(column, lower(type.get<std::string>()));
    manifest.tables.push_back(std::move(table));
  }
  return manifest;
}

// Calls `row(line number, line)` for every line of a table's file that is not
// empty, and checks that the file holds the rows the manifest counts.
template <class Fn>
Result<void> each_row(const std::filesystem::path& dir, const ManifestTable& table, Fn&& row) {
  const auto path = dir / table.file;
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + utf8(path) + " (" + kManifestFile + " lists it)");
  std::string line;
  std::size_t number = 0;
  std::int64_t rows = 0;
  while (std::getline(in, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    ++rows;
    if (auto r = row(number, line); !r) return r;
  }
  if (in.bad()) return fail(ErrorKind::Io, "cannot read " + utf8(path));
  if (rows != table.rows)
    return fail(ErrorKind::Protocol, utf8(path) + " has " + std::to_string(rows) + " rows and " + kManifestFile +
                                         " counts " + std::to_string(table.rows) +
                                         ": the directory is not one whole conversion");
  return {};
}

// ---------------------------------------------------------------- one row

// Reads the columns of one row and collects why it cannot be imported. A
// column that is absent, null or "" has no value.
class Fields {
 public:
  explicit Fields(const Json& row) : row_(row) {}

  bool ok() const { return problems_.empty(); }
  void refuse(std::string why) { problems_.push_back(std::move(why)); }
  const std::vector<std::string>& problems() const { return problems_; }

  // A link the store can do without names no usable parent: the row is
  // imported without it, and the link is reported. `outcome`: what became of
  // the row, as the conflict's reason ends.
  struct BrokenLink {
    std::string column, why, outcome;
  };
  void unlink(std::string_view column, std::string why, std::string outcome = "imported without it") {
    links_.push_back({std::string(column), std::move(why), std::move(outcome)});
  }
  const std::vector<BrokenLink>& broken_links() const { return links_; }

  std::optional<std::string> text(std::string_view column) {
    std::string out;
    if (const Json* v = value(column)) {
      auto read = as_text(*v);
      if (!read) {
        refuse(std::string(column) + " is not text");
        return std::nullopt;
      }
      out = std::move(*read);
    } else if (const Json* encoded = binary(column)) {
      // A BLOB column that holds text.
      auto bytes = encoded->is_string() ? base64_decode(encoded->get_ref<const std::string&>()) : std::nullopt;
      if (!bytes) {
        refuse(std::string(column) + " is not base64");
        return std::nullopt;
      }
      out = text_of(std::move(*bytes));
    } else {
      return std::nullopt;
    }
    if (out.find('\0') != std::string::npos) {
      refuse(std::string(column) + " holds a NUL character");
      return std::nullopt;
    }
    if (out.empty()) return std::nullopt;
    return out;
  }

  // An optional link or free text: the legacy "none" (nothing but hyphens, or
  // nothing but white space) is no value. Not for a name that is a natural key, which
  // is kept as the dump writes it.
  std::optional<std::string> optional_text(std::string_view column) {
    auto read = text(column);
    if (read && is_legacy_none(*read)) return std::nullopt;
    return read;
  }

  // The column's text; refused when there is none.
  std::string required(std::string_view column) {
    const std::size_t before = problems_.size();
    auto read = text(column);
    if (!read && problems_.size() == before) refuse(std::string(column) + " is missing");
    return read.value_or("");
  }

  std::optional<double> number(std::string_view column) { return typed(column, as_double, " is not a number"); }
  std::optional<int> integer(std::string_view column) { return typed(column, as_int, " is not a whole number"); }
  // Refused when there is none.
  std::optional<int> required_integer(std::string_view column) {
    const std::size_t before = problems_.size();
    auto read = integer(column);
    if (!read && problems_.size() == before) refuse(std::string(column) + " is missing");
    return read;
  }
  std::optional<bool> flag(std::string_view column) { return typed(column, as_bool, " is not 0 or 1"); }

 private:
  const Json* value(std::string_view column) const {
    const auto it = row_.find(column);
    return it == row_.end() || it->is_null() ? nullptr : &*it;
  }
  const Json* binary(std::string_view column) const {
    const auto it = row_.find(std::string(column) + std::string(kBinarySuffix));
    return it == row_.end() || it->is_null() ? nullptr : &*it;
  }

  template <class T>
  std::optional<T> typed(std::string_view column, std::optional<T> (*convert)(const Json&), const char* complaint) {
    const Json* v = value(column);
    if (!v) {
      if (binary(column)) refuse(std::string(column) + complaint);
      return std::nullopt;
    }
    if (v->is_string() && v->get_ref<const std::string&>().empty()) return std::nullopt;
    auto read = convert(*v);
    if (!read) refuse(std::string(column) + complaint);
    return read;
  }

  const Json& row_;
  std::vector<std::string> problems_;
  std::vector<BrokenLink> links_;
};

// One row of a catalog table: what it becomes. A refused row is one conflict;
// an imported row is its item and one conflict per link it lost.
struct Unit {
  std::optional<ingest::CatalogItem> item;
  std::vector<ingest::ConflictItem> conflicts;
  // The conflicts a row of this table can have and this one has not: a store
  // imported under an older rule may hold them (ImportBatch::superseded).
  std::vector<ingest::SourceKey> answered;
  std::string path;  // "<file>#<legacy id>": the row's name in this source
  std::string blob;  // SHA-256 of its line, as text
};

// ---------------------------------------------------------------- the catalog

struct PiKey {
  std::string last, first;
};
struct ProjectKey {
  std::string name;
  std::optional<PiKey> pi;
};
struct MaterialKey {
  std::string name, grainsize;
};
struct SampleKey {
  std::string name;
  ProjectKey project;
  MaterialKey material;
};
struct LevelKey {
  std::string irradiation, name;
};

std::string natural(const PiKey& k) { return k.last + "\n" + k.first; }
std::string natural(const ProjectKey& k) { return k.name + "\n" + (k.pi ? natural(*k.pi) : std::string("\n")); }
std::string natural(const MaterialKey& k) { return k.name + "\n" + k.grainsize; }
std::string natural(const SampleKey& k) { return k.name + "\n" + natural(k.project) + "\n" + natural(k.material); }
std::string natural(const LevelKey& k) { return k.irradiation + "\n" + k.name; }

// Turns every catalog table of a converted dump into units, parents first.
class Reader {
 public:
  Reader(const std::filesystem::path& dir, const Manifest& manifest, std::string zone)
      : dir_(dir), manifest_(manifest), zone_(std::move(zone)) {}

  Result<std::array<std::vector<Unit>, kTableCount>> read() {
    using Row = const Json&;
    using Id = const std::string&;
    if (auto r = table(kPi, [&](Row row, Fields& f, Id id) { return pi(row, f, id); }); !r) return fail(r.error());
    if (auto r = table(kProject, [&](Row row, Fields& f, Id id) { return project(row, f, id); }); !r)
      return fail(r.error());
    if (auto r = table(kMaterial, [&](Row row, Fields& f, Id id) { return material(row, f, id); }); !r)
      return fail(r.error());
    if (auto r = table(kSample, [&](Row row, Fields& f, Id id) { return sample(row, f, id); }); !r)
      return fail(r.error());
    if (auto r = table(kIrradiation, [&](Row row, Fields& f, Id id) { return irradiation(row, f, id); }); !r)
      return fail(r.error());
    if (auto r = table(kLevel, [&](Row row, Fields& f, Id id) { return level(row, f, id); }); !r)
      return fail(r.error());
    if (auto r = table(kPosition, [&](Row row, Fields& f, Id id) { return position(row, f, id); }); !r)
      return fail(r.error());
    if (auto r = table(kUser, [&](Row row, Fields& f, Id id) { return user(row, f, id); }); !r) return fail(r.error());
    if (auto r = table(kMassSpec, [&](Row row, Fields& f, Id id) { return mass_spectrometer(row, f, id); }); !r)
      return fail(r.error());
    if (auto r = table(kExtractDevice, [&](Row row, Fields& f, Id id) { return extract_device(row, f, id); }); !r)
      return fail(r.error());
    if (auto r = table(kLoad, [&](Row row, Fields& f, Id id) { return load(row, f, id); }); !r) return fail(r.error());
    if (auto r = table(kLoadPosition, [&](Row row, Fields& f, Id id) { return load_position(row, f, id); }); !r)
      return fail(r.error());
    return std::move(units_);
  }

 private:
  // Reads one table. `to_item(row, fields, legacy id)` returns the row's item;
  // it is sent unless the row was refused.
  template <class Fn>
  Result<void> table(TableIndex index, Fn&& to_item) {
    const ManifestTable* found = manifest_.find(kTables[index].name);
    if (!found) return {};
    const ManifestTable& source = *found;
    current_ = &source;
    const std::string key_column = kTables[index].key;
    std::set<std::string> keys;
    return each_row(dir_, source, [&](std::size_t number, const std::string& line) -> Result<void> {
      std::string legacy_id = "line" + std::to_string(number);
      std::vector<std::string> problems;
      std::vector<Fields::BrokenLink> links;
      std::optional<ingest::CatalogItem> item;
      Json detail = Json::object();
      detail["table"] = source.name;
      detail["line"] = number;
      detail["legacy_id"] = nullptr;

      auto parsed = parse_legacy(line);
      if (!parsed || !parsed->is_object()) {
        problems.push_back("the line is not a JSON object");
        detail["text"] = line.substr(0, 2000);
      } else {
        const Json& row = *parsed;
        detail["row"] = row;
        std::optional<std::string> key;
        if (const auto it = row.find(key_column); it != row.end()) key = as_text(*it);
        if (key && key->empty()) key.reset();
        if (!key) {
          problems.push_back(key_column + " is missing");
        } else if (!keys.insert(*key).second) {
          problems.push_back(key_column + " repeats that of an earlier row");
          legacy_id = *key + "~line" + std::to_string(number);
          detail["legacy_id"] = *key;
        } else {
          legacy_id = *key;
          detail["legacy_id"] = *key;
          Fields fields(row);
          item = to_item(row, fields, legacy_id);
          problems = fields.problems();
          links = fields.broken_links();
          if (!problems.empty()) refused_[index].insert(fold(legacy_id));
        }
      }

      const Sha256Digest digest = sha256(std::string_view{line});
      const auto conflict = [&](const std::string& path, Json about) {
        spell_nul(about);
        ingest::ConflictItem made;
        made.key = {manifest_.sha256, path, to_hex(digest)};
        made.kind = P::ConflictKind::IdentityClash;
        made.file_sha256 = digest;
        made.detail_json = dump(about);
        return made;
      };
      Unit unit;
      unit.path = source.file + "#" + legacy_id;
      unit.blob = to_hex(digest);
      const auto answered = [&](const std::string& path) {
        unit.answered.push_back({manifest_.sha256, path, to_hex(digest)});
      };
      if (problems.empty()) {
        unit.item = std::move(item);
        answered(unit.path);
        // The row is stored; each link it lost is a conflict of its own, named
        // by the column, so it is never taken for a refusal of the row.
        for (const auto& link : links) {
          Json about = detail;
          about[ingest::kMarkerImported] = true;
          about["column"] = link.column;
          about["value"] = detail["row"].value(link.column, Json());
          about["reason"] = link.why + "; " + link.outcome;
          unit.conflicts.push_back(conflict(source.file + "#" + legacy_id + "@" + link.column, std::move(about)));
        }
        if (const char* column = kTables[index].link;
            column && std::none_of(links.begin(), links.end(), [&](const auto& l) { return l.column == column; }))
          answered(unit.path + "@" + column);
      } else {
        if (const char* column = kTables[index].link) answered(unit.path + "@" + column);
        std::string reason;
        for (const auto& problem : problems) reason += (reason.empty() ? "" : "; ") + problem;
        for (const auto& link : links) reason += "; " + link.why;
        detail[ingest::kMarkerImported] = false;
        detail["reason"] = reason;
        unit.conflicts.push_back(conflict(source.file + "#" + legacy_id, std::move(detail)));
      }
      units_[index].push_back(std::move(unit));
      return {};
    });
  }

  // Whether the store can hold a row without the parent a foreign key names
  // (the target column is nullable in migrations/pg/0001_init.sql).
  enum class Link { Required, Optional };

  // The parent row a foreign key names; nullptr when it names none. A
  // required link that is null, or names a row that is missing or was
  // refused, refuses the row. An optional link may be null; one that names no
  // usable row is dropped and reported (spec section 10.23). A key that
  // cannot be read refuses the row either way.
  template <class Key>
  const Key* parent(Fields& f, std::string_view column, TableIndex of, const std::map<int, Key>& rows, Link link) {
    const auto id = link == Link::Required ? f.required_integer(column) : f.integer(column);
    if (!id) return nullptr;
    if (const auto it = rows.find(*id); it != rows.end()) return &it->second;
    std::string why = missing(std::string(column) + " " + std::to_string(*id), of, std::to_string(*id));
    if (link == Link::Required)
      f.refuse(std::move(why));
    else
      f.unlink(column, std::move(why));
    return nullptr;
  }

  // Why a row named by `what` cannot be used.
  std::string missing(const std::string& what, TableIndex of, const std::string& legacy_id) const {
    const std::string name = kTables[of].name;
    return refused_[of].contains(fold(legacy_id)) ? what + " names a " + name + " row that was not imported"
                                            : what + " is not in " + name;
  }

  // A row with the natural key of an earlier one adds nothing: silently when
  // the two say the same, refused when they differ.
  void once(Fields& f, TableIndex index, const std::string& natural_key, const Json& signature,
            const std::string& legacy_id) {
    if (!f.ok()) return;
    std::string text = dump(signature);
    const auto [it, inserted] = seen_[index].try_emplace(natural_key, text, legacy_id);
    if (inserted || it->second.first == text) return;
    f.refuse(std::string("has the natural key of ") + kTables[index].name + " " + it->second.second +
             " and other values; that row is kept");
  }

  // A row of a table whose key is a name. Names that differ by case or
  // trailing spaces are one name to MySQL: such a row repeats the earlier one
  // (once()), and the name everything is stored under is the first row's
  // spelling, which is returned. `known`: the table's names, folded.
  std::string named(Fields& f, TableIndex index, std::map<std::string, std::string>& known,
                    const std::string& spelling, Json compared, const std::string& legacy_id) {
    const std::string key = fold(spelling);
    compared["name"] = key;
    const std::string kept = known.try_emplace(key, spelling).first->second;
    once(f, index, key, compared, legacy_id);
    return kept;
  }

  // The row without its surrogate key: what two rows are compared by.
  static Json signature(const Json& row) {
    Json out = row;
    out.erase("id");
    return out;
  }

  // `declared`: the column's type in the legacy ORM (fixtures/README.md,
  // section 7), used when the dump has no CREATE TABLE to say (mysqldump
  // --no-create-info). The dump's own type wins.
  std::optional<P::UtcTime> time(Fields& f, std::string_view column, std::string_view declared) const {
    auto text = f.text(column);
    if (!text || is_zero_date(*text)) return std::nullopt;
    if (text->size() == 10) *text += " 00:00:00";
    const auto dumped = current_->types.find(column);
    const std::string_view type = dumped != current_->types.end() ? std::string_view{dumped->second} : declared;
    if (manifest_.utc_timestamps && type == "timestamp") {
      std::string iso = *text;
      if (iso.size() > 10 && iso[10] == ' ') iso[10] = 'T';
      if (auto utc = P::UtcTime::parse(iso + "Z")) return utc;
    } else if (auto utc = ingest::local_to_utc(*text, zone_)) {
      return utc->utc;
    }
    f.refuse(std::string(column) + " '" + *text + "' is not a time");
    return std::nullopt;
  }

  // A DATE column, or the day of a DATETIME: the calendar date as written.
  static std::optional<std::string> date(Fields& f, std::string_view column) {
    auto text = f.text(column);
    if (!text || is_zero_date(*text)) return std::nullopt;
    std::string day = text->substr(0, 10);
    const bool only_day = text->size() == 10 || (text->size() > 10 && ((*text)[10] == ' ' || (*text)[10] == 'T'));
    if (only_day && is_calendar_date(day)) return day;
    f.refuse(std::string(column) + " '" + *text + "' is not a date");
    return std::nullopt;
  }

  // ------------------------------------------------------------ the tables

  ingest::CatalogItem pi(const Json& row, Fields& f, const std::string& legacy_id) {
    ingest::PiItem item;
    const auto id = f.integer("id");
    item.last_name = f.required("last_name");
    item.first_initial = f.text("first_initial").value_or("");
    item.affiliation = f.optional_text("affiliation");
    item.email = f.optional_text("email");
    if (f.ok() && id) {
      const PiKey key{item.last_name, item.first_initial};
      pis_.emplace(*id, key);
      once(f, kPi, natural(key), signature(row), legacy_id);
    }
    return item;
  }

  ingest::CatalogItem project(const Json& row, Fields& f, const std::string& legacy_id) {
    ingest::ProjectItem item;
    const auto id = f.integer("id");
    item.name = f.required("name");
    const PiKey* investigator = parent(f, "principal_investigatorID", kPi, pis_, Link::Optional);
    item.checkin_date = date(f, "checkin_date");
    item.comment = f.optional_text("comment");
    item.lab_contact = f.optional_text("lab_contact");
    item.institution = f.optional_text("institution");
    if (f.ok() && id) {
      ProjectKey key{item.name, std::nullopt};
      Json compared = signature(row);
      if (investigator) {
        key.pi = *investigator;
        item.pi_last_name = investigator->last;
        item.pi_first_initial = investigator->first;
        compared["principal_investigatorID"] = natural(*investigator);
      }
      projects_.emplace(*id, key);
      once(f, kProject, natural(key), compared, legacy_id);
    }
    return item;
  }

  ingest::CatalogItem material(const Json& row, Fields& f, const std::string& legacy_id) {
    ingest::MaterialItem item;
    const auto id = f.integer("id");
    item.name = f.required("name");
    item.grainsize = f.text("grainsize").value_or("");
    if (f.ok() && id) {
      const MaterialKey key{item.name, item.grainsize};
      materials_.emplace(*id, key);
      once(f, kMaterial, natural(key), signature(row), legacy_id);
    }
    return item;
  }

  ingest::CatalogItem sample(const Json& row, Fields& f, const std::string& legacy_id) {
    ingest::SampleItem item;
    const auto id = f.integer("id");
    auto& s = item.fields;
    s.name = f.required("name");
    // Legacy allows a sample without a material and the store does not: one
    // that names none, or none that can be used, is imported under the
    // placeholder material and reported (spec section 10.41).
    const MaterialKey placeholder{ingest::kPlaceholderMaterial, ""};
    const std::string under = std::string("imported under material '") + ingest::kPlaceholderMaterial + "'";
    const MaterialKey* of = &placeholder;
    const std::size_t problems = f.problems().size();
    if (const auto material_id = f.integer("materialID")) {
      if (const auto it = materials_.find(*material_id); it != materials_.end())
        of = &it->second;
      else
        f.unlink("materialID",
                 missing("materialID " + std::to_string(*material_id), kMaterial, std::to_string(*material_id)), under);
    } else if (f.problems().size() == problems) {  // not one that cannot be read, which refuses the row
      f.unlink("materialID", "materialID is missing", under);
    }
    const ProjectKey* in = parent(f, "projectID", kProject, projects_, Link::Required);
    s.note = f.optional_text("note");
    s.igsn = f.optional_text("igsn");
    s.lat = f.number("lat");
    s.lon = f.number("lon");
    s.elevation = f.number("elevation");
    s.storage_location = f.optional_text("storage_location");
    s.location = f.optional_text("location");
    s.unit = f.optional_text("unit");
    s.lithology = f.optional_text("lithology");
    s.lithology_class = f.optional_text("lithology_class");
    s.lithology_type = f.optional_text("lithology_type");
    s.lithology_group = f.optional_text("lithology_group");
    s.approximate_age = f.number("approximate_age");
    s.created = time(f, "create_date", "datetime");
    s.updated = time(f, "update_date", "datetime");
    if (f.ok() && id && in) {
      item.project = in->name;
      item.material = of->name;
      item.grainsize = of->grainsize;
      if (in->pi) {
        item.pi_last_name = in->pi->last;
        item.pi_first_initial = in->pi->first;
      }
      const SampleKey key{s.name, *in, *of};
      samples_.emplace(*id, key);
      Json compared = signature(row);
      compared["materialID"] = natural(*of);
      compared["projectID"] = natural(*in);
      once(f, kSample, natural(key), compared, legacy_id);
    }
    return item;
  }

  ingest::CatalogItem irradiation(const Json& row, Fields& f, const std::string& legacy_id) {
    ingest::IrradiationItem item;
    const auto id = f.integer("id");
    item.name = f.required("name");
    item.created = time(f, "create_date", "timestamp");
    if (f.ok() && id) {
      irradiations_.emplace(*id, item.name);
      once(f, kIrradiation, item.name, signature(row), legacy_id);
    }
    return item;
  }

  ingest::CatalogItem level(const Json& row, Fields& f, const std::string& legacy_id) {
    ingest::LevelItem item;
    const auto id = f.integer("id");
    item.name = f.required("name");
    const std::string* in = parent(f, "irradiationID", kIrradiation, irradiations_, Link::Required);
    item.holder = f.optional_text("holder");
    item.z = f.number("z");
    item.note = f.optional_text("note");
    if (f.ok() && id && in) {
      item.irradiation = *in;
      const LevelKey key{*in, item.name};
      levels_.emplace(*id, key);
      Json compared = signature(row);
      compared["irradiationID"] = *in;
      once(f, kLevel, natural(key), compared, legacy_id);
    }
    return item;
  }

  ingest::CatalogItem position(const Json& row, Fields& f, const std::string& legacy_id) {
    ingest::PositionItem item;
    const LevelKey* in = parent(f, "levelID", kLevel, levels_, Link::Required);
    const SampleKey* of = parent(f, "sampleID", kSample, samples_, Link::Optional);
    const auto hole = f.required_integer("position");
    item.identifier = f.optional_text("identifier").value_or("");
    item.weight = f.number("weight");
    item.packet = f.optional_text("packet");
    item.note = f.optional_text("note");
    if (f.ok() && in && hole) {
      item.irradiation = in->irradiation;
      item.level = in->name;
      item.position = *hole;
      Json compared = signature(row);
      compared["levelID"] = natural(*in);
      if (of) {
        item.sample = of->name;
        item.project = of->project.name;
        item.material = of->material.name;
        item.grainsize = of->material.grainsize;
        if (of->project.pi) {
          item.pi_last_name = of->project.pi->last;
          item.pi_first_initial = of->project.pi->first;
        }
        compared["sampleID"] = natural(*of);
      }
      const std::string where = natural(*in) + "\n" + std::to_string(*hole);
      once(f, kPosition, where, compared, legacy_id);
      if (f.ok() && !item.identifier.empty()) {
        // An identifier sits at one position.
        const auto [it, inserted] =
            identifiers_.try_emplace(fold(item.identifier), Placed{where, legacy_id, item.identifier});
        if (!inserted && it->second.where != where)
          f.refuse("identifier " + item.identifier + " already sits at the position of IrradiationPositionTbl " +
                   it->second.legacy_id);
      }
    }
    if (!f.ok() && !item.identifier.empty()) refused_identifiers_.insert(fold(item.identifier));
    return item;
  }

  ingest::CatalogItem user(const Json& row, Fields& f, const std::string& legacy_id) {
    ingest::UserItem item;
    item.name = f.required("name");
    item.email = f.optional_text("email");
    item.affiliation = f.optional_text("affiliation");
    item.category = f.optional_text("category");
    if (f.ok()) item.name = named(f, kUser, users_, item.name, row, legacy_id);
    return item;
  }

  ingest::CatalogItem mass_spectrometer(const Json& row, Fields& f, const std::string& legacy_id) {
    ingest::MassSpecItem item;
    // Lower case, as the analysis and reference imports name spectrometers.
    item.spec.name = lower(f.required("name"));
    item.spec.kind = f.optional_text("kind");
    if (f.ok()) item.spec.name = named(f, kMassSpec, spectrometers_, item.spec.name, row, legacy_id);
    return item;
  }

  ingest::CatalogItem extract_device(const Json& row, Fields& f, const std::string& legacy_id) {
    ingest::ExtractDeviceItem item;
    item.name = f.required("name");
    if (f.ok()) item.name = named(f, kExtractDevice, devices_, item.name, row, legacy_id);
    return item;
  }

  ingest::CatalogItem load(const Json& row, Fields& f, const std::string& legacy_id) {
    ingest::LoadItem item;
    item.spec.name = f.required("name");
    item.spec.created = time(f, "create_date", "timestamp");
    item.spec.archived = f.flag("archived").value_or(false);
    item.holder_name = f.optional_text("holderName");
    Json compared = row;
    if (const auto by = f.optional_text("username")) {
      if (const auto creator = users_.find(fold(*by)); creator != users_.end()) {
        item.created_by = creator->second;
        compared["username"] = creator->second;
      } else {
        f.unlink("username", missing("username '" + *by + "'", kUser, *by));
      }
    }
    if (f.ok()) item.spec.name = named(f, kLoad, loads_, item.spec.name, compared, legacy_id);
    return item;
  }

  ingest::CatalogItem load_position(const Json& row, Fields& f, const std::string& legacy_id) {
    ingest::LoadPositionItem item;
    Json compared = signature(row);
    // Both parents are stored in their own spelling.
    if (const std::string tray = f.required("loadName"); !tray.empty()) {
      if (const auto found = loads_.find(fold(tray)); found != loads_.end())
        compared["loadName"] = item.load = found->second;
      else
        f.refuse(missing("loadName '" + tray + "'", kLoad, tray));
    }
    if (const std::string loaded = f.required("identifier"); !loaded.empty()) {
      if (const auto found = identifiers_.find(fold(loaded)); found != identifiers_.end())
        compared["identifier"] = item.identifier = found->second.spelling;
      else
        f.refuse("identifier " + loaded +
                 (refused_identifiers_.contains(fold(loaded))
                      ? " names an IrradiationPositionTbl row that was not imported"
                      : " is not in IrradiationPositionTbl"));
    }
    const auto hole = f.required_integer("position");
    item.weight = f.number("weight");
    item.nxtals = f.integer("nxtals");
    item.note = f.optional_text("note");
    if (f.ok() && hole) {
      item.position = *hole;
      once(f, kLoadPosition, item.load + "\n" + std::to_string(*hole) + "\n" + item.identifier, compared, legacy_id);
    }
    return item;
  }

  const std::filesystem::path& dir_;
  const Manifest& manifest_;
  std::string zone_;
  const ManifestTable* current_ = nullptr;  // the table being read

  std::array<std::vector<Unit>, kTableCount> units_;
  std::array<std::set<std::string>, kTableCount> refused_;  // legacy ids of refused rows, folded
  // natural key -> (what the first row with it says, its legacy id)
  std::array<std::map<std::string, std::pair<std::string, std::string>>, kTableCount> seen_;

  // Rows that later tables may name, by legacy id.
  std::map<int, PiKey> pis_;
  std::map<int, ProjectKey> projects_;
  std::map<int, MaterialKey> materials_;
  std::map<int, SampleKey> samples_;
  std::map<int, std::string> irradiations_;
  std::map<int, LevelKey> levels_;
  // Rows that later tables may name by a string key, by that key folded: MySQL
  // matches such keys without regard to case or trailing spaces. The value is
  // the parent's own spelling, which is what references to it are stored as.
  struct Placed {
    std::string where;      // the position, as a natural key
    std::string legacy_id;  // of the IrradiationPositionTbl row
    std::string spelling;   // the identifier as that row writes it
  };
  std::map<std::string, Placed> identifiers_;
  std::set<std::string> refused_identifiers_;  // folded
  std::map<std::string, std::string> users_, spectrometers_, devices_, loads_;
};

std::optional<std::size_t> parse_index(std::string_view text) {
  std::size_t value = 0;
  if (text.empty()) return std::nullopt;
  const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
  if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

std::string what_failed(const std::exception& e) { return std::string("catalog dump: ") + e.what(); }

}  // namespace

// ---------------------------------------------------------------- adapter

class CatalogAdapter::Impl {
 public:
  std::string url;
  std::string sha256;
  std::size_t batch_rows = 1;
  std::vector<std::string> warnings;
  std::vector<Unit> units;                          // every row, in the order sent
  std::array<std::size_t, kTableCount + 1> starts{};  // where each table's rows begin in `units`
  std::size_t next = 0;

  // "<table index>:<row index>@<sha256>" of the row at `at`.
  std::string token(std::size_t at) const {
    std::size_t index = 0;
    while (index < kTableCount && at >= starts[index + 1]) ++index;
    return std::to_string(index) + ":" + std::to_string(at - starts[index]) + "@" + sha256;
  }

  // Where a token points; nullopt when it is not one of this dump.
  std::optional<std::size_t> place(std::string_view token_text) const {
    const auto at = token_text.find('@');
    const auto colon = token_text.find(':');
    if (at == std::string_view::npos || colon == std::string_view::npos || colon > at) return std::nullopt;
    if (token_text.substr(at + 1) != sha256) return std::nullopt;
    const auto index = parse_index(token_text.substr(0, colon));
    const auto row = parse_index(token_text.substr(colon + 1, at - colon - 1));
    if (!index || !row || *index > kTableCount) return std::nullopt;
    if (*index == kTableCount) return *row == 0 ? std::optional<std::size_t>{units.size()} : std::nullopt;
    if (*row > starts[*index + 1] - starts[*index]) return std::nullopt;
    return starts[*index] + *row;
  }
};

CatalogAdapter::CatalogAdapter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CatalogAdapter::~CatalogAdapter() = default;

Result<std::unique_ptr<CatalogAdapter>> CatalogAdapter::open(CatalogAdapterConfig config) {
  try {
    if (config.batch_rows < 1) return fail(ErrorKind::Config, "catalog dump: batch_rows must be at least 1");
    if (!ingest::known_zone(config.lab_time_zone))
      return fail(ErrorKind::Config, "catalog dump: unknown time zone '" + config.lab_time_zone + "'");
    std::error_code ec;
    if (!std::filesystem::is_directory(config.dir, ec))
      return fail(ErrorKind::Io, "catalog dump: " + utf8(config.dir) + " is not a directory");
    auto manifest = read_manifest(config.dir);
    if (!manifest) return fail(manifest.error());

    auto tables = Reader(config.dir, *manifest, config.lab_time_zone).read();
    if (!tables) return fail(tables.error());

    auto impl = std::make_unique<Impl>();
    auto absolute = std::filesystem::absolute(config.dir, ec);
    if (ec) absolute = config.dir;
    impl->url = utf8(absolute.lexically_normal());
    while (impl->url.size() > 1 && (impl->url.back() == '/' || impl->url.back() == '\\')) impl->url.pop_back();
    impl->sha256 = manifest->sha256;
    if (!manifest->dump_completed)
      impl->warnings.push_back("catalog dump: " + impl->url +
                               ": the dump has no completion marker; it may be truncated");
    impl->batch_rows = static_cast<std::size_t>(config.batch_rows);
    for (std::size_t index = 0; index < kTableCount; ++index) {
      impl->starts[index] = impl->units.size();
      for (auto& unit : (*tables)[index]) impl->units.push_back(std::move(unit));
    }
    impl->starts[kTableCount] = impl->units.size();
    return std::unique_ptr<CatalogAdapter>(new CatalogAdapter(std::move(impl)));
  } catch (const std::exception& e) {
    return fail(ErrorKind::Io, what_failed(e));
  }
}

Result<ingest::SourceDescription> CatalogAdapter::describe() {
  return ingest::SourceDescription{P::ImportSourceKind::LegacyDb, impl_->url, "", impl_->sha256};
}

std::vector<std::string> CatalogAdapter::warnings() const { return impl_->warnings; }

Result<int> CatalogAdapter::plan(std::optional<std::string> resume_token, ingest::IImportState&) {
  impl_->next = resume_token ? impl_->place(*resume_token).value_or(0) : 0;
  return static_cast<int>(impl_->units.size() - impl_->next);
}

Result<std::optional<ingest::ImportBatch>> CatalogAdapter::next_batch() {
  try {
    Impl& d = *impl_;
    if (d.next >= d.units.size()) return std::optional<ingest::ImportBatch>{};
    const std::size_t end = d.next + std::min(d.batch_rows, d.units.size() - d.next);
    ingest::ImportBatch batch;
    for (std::size_t i = d.next; i < end; ++i) {
      if (d.units[i].item) batch.catalog.push_back(*d.units[i].item);
      for (const auto& conflict : d.units[i].conflicts) batch.conflicts.push_back(conflict);
      for (const auto& key : d.units[i].answered) batch.superseded.push_back(key);
    }
    d.next = end;
    batch.resume_token = d.token(end);
    batch.done = static_cast<int>(end);
    batch.total = static_cast<int>(d.units.size());
    batch.head = d.sha256;
    return std::optional<ingest::ImportBatch>{std::move(batch)};
  } catch (const std::exception& e) {
    return fail(ErrorKind::Io, what_failed(e));
  }
}

Result<std::optional<std::int64_t>> CatalogAdapter::order_of(std::string_view) {
  return std::optional<std::int64_t>{};
}

// A row that was sent is accounted for by the catalog row its item names, and
// by the conflict of each link it lost; a refused row by its conflict.
Result<void> CatalogAdapter::for_each_unit(ingest::IImportState&,
                                           const std::function<Result<void>(const ingest::SourceUnit&)>& visit) {
  try {
    using ingest::Evidence;
    for (const auto& row : impl_->units) {
      ingest::SourceUnit unit;
      unit.commit = impl_->sha256;
      unit.path = row.path;
      unit.blob_sha = row.blob;
      unit.disposition = row.item ? ingest::UnitDisposition::Imported : ingest::UnitDisposition::Conflict;
      if (row.item) unit.evidence.push_back({Evidence::Kind::CatalogRow, {}, {}, {}, {}, *row.item});
      for (const auto& conflict : row.conflicts)
        unit.evidence.push_back({Evidence::Kind::Conflict, conflict.key.commit, conflict.key.path});
      if (auto r = visit(unit); !r) return r;
    }
    return {};
  } catch (const std::exception& e) {
    return fail(ErrorKind::Io, what_failed(e));
  }
}

// ---------------------------------------------------------------- tags

Result<std::function<std::optional<std::string>(const persistence::Uuid&)>> load_tag_lookup(
    const std::filesystem::path& dir) {
  using Lookup = std::function<std::optional<std::string>(const P::Uuid&)>;
  try {
    const Lookup none = [](const P::Uuid&) { return std::optional<std::string>{}; };
    std::error_code ec;
    if (!std::filesystem::exists(dir / kManifestFile, ec)) {
      for (const char* file : {"AnalysisTbl.jsonl", "AnalysisChangeTbl.jsonl"})
        if (std::filesystem::exists(dir / file, ec))
          return fail(ErrorKind::Io, "catalog dump: " + utf8(dir) + " has " + file + " but no " + kManifestFile +
                                         " (a conversion that did not finish)");
      return none;
    }
    auto manifest = read_manifest(dir);
    if (!manifest) return fail(manifest.error());
    const ManifestTable* analyses = manifest->find("AnalysisTbl");
    const ManifestTable* changes = manifest->find("AnalysisChangeTbl");
    if (!analyses || !changes) return none;

    const auto object = [](const ManifestTable& table, std::size_t number, const std::string& line) -> Result<Json> {
      auto parsed = parse_legacy(line);
      if (!parsed || !parsed->is_object())
        return fail(ErrorKind::Protocol,
                    "catalog dump: " + table.file + " line " + std::to_string(number) + " is not a JSON object");
      return parsed;
    };
    const auto column = [](const Json& row, const char* name) -> const Json& {
      static const Json null;
      const auto it = row.find(name);
      return it == row.end() ? null : *it;
    };

    // Legacy analysis id -> its latest change row and that row's tag.
    std::unordered_map<int, std::pair<int, std::optional<std::string>>> latest;
    if (auto r = each_row(dir, *changes,
                          [&](std::size_t number, const std::string& line) -> Result<void> {
                            auto row = object(*changes, number, line);
                            if (!row) return fail(row.error());
                            const auto analysis = as_int(column(*row, "analysisID"));
                            if (!analysis) return {};
                            const int change = as_int(column(*row, "idanalysischangeTbl")).value_or(-1);
                            auto tag = as_text(column(*row, "tag"));
                            if (tag && tag->empty()) tag.reset();
                            const auto [it, inserted] = latest.try_emplace(*analysis, change, tag);
                            if (!inserted && change >= it->second.first) it->second = {change, std::move(tag)};
                            return {};
                          });
        !r)
      return fail(r.error());

    auto tags = std::make_shared<std::unordered_map<P::Uuid, std::string>>();
    if (auto r = each_row(dir, *analyses,
                          [&](std::size_t number, const std::string& line) -> Result<void> {
                            auto row = object(*analyses, number, line);
                            if (!row) return fail(row.error());
                            const auto id = as_int(column(*row, "id"));
                            if (!id) return {};
                            const auto change = latest.find(*id);
                            if (change == latest.end() || !change->second.second) return {};
                            const auto text = as_text(column(*row, "uuid"));
                            const auto uuid = text ? parse_uuid(*text) : std::nullopt;
                            if (uuid) tags->insert_or_assign(*uuid, *change->second.second);
                            return {};
                          });
        !r)
      return fail(r.error());

    return Lookup([tags](const P::Uuid& analysis) -> std::optional<std::string> {
      const auto it = tags->find(analysis);
      return it == tags->end() ? std::nullopt : std::optional<std::string>{it->second};
    });
  } catch (const std::exception& e) {
    return fail(ErrorKind::Io, what_failed(e));
  }
}

}  // namespace pychron::dvc
