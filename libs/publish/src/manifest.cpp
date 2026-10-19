// The manifest of a release directory and its verification (data publishing
// design, sections 5.7 and 5.8).

#include "pychron/publish/manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace pychron::publish {

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

namespace {

Json time_or_null(const std::optional<UtcTime>& t) { return t ? Json(t->iso()) : Json(nullptr); }
Json text_or_null(const std::optional<std::string>& s) { return s ? Json(*s) : Json(nullptr); }
Json uuid_or_null(const std::optional<Uuid>& u) { return u ? Json(u->str()) : Json(nullptr); }

Result<std::optional<UtcTime>> time_of(const Json& j, const char* what) {
  if (j.is_null()) return std::optional<UtcTime>{};
  if (!j.is_string()) return fail(ErrorKind::Config, std::string("manifest: ") + what + " is not a time");
  auto t = UtcTime::parse(j.get<std::string>());
  if (!t) return fail(ErrorKind::Config, std::string("manifest: ") + what + " is not an ISO-8601 UTC time");
  return std::optional<UtcTime>{*t};
}

Result<std::optional<Uuid>> uuid_of(const Json& j, const char* what) {
  if (j.is_null()) return std::optional<Uuid>{};
  if (!j.is_string()) return fail(ErrorKind::Config, std::string("manifest: ") + what + " is not a uuid");
  auto u = Uuid::parse(j.get<std::string>());
  if (!u) return fail(ErrorKind::Config, std::string("manifest: ") + what + " is not a uuid");
  return std::optional<Uuid>{*u};
}

std::optional<std::string> opt_string(const Json& j) {
  if (j.is_string()) return j.get<std::string>();
  return std::nullopt;
}

Result<Sha256Digest> digest_of(const Json& j, const char* what) {
  if (!j.is_string() || j.get<std::string>().size() != 64)
    return fail(ErrorKind::Config, std::string("manifest: ") + what + " is not a SHA-256 hex");
  const std::string hex = j.get<std::string>();
  Sha256Digest out{};
  for (std::size_t i = 0; i < 32; ++i) {
    const auto nibble = [&](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return fail(ErrorKind::Config, std::string("manifest: ") + what + " is not hex");
    out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return out;
}

Result<std::string> slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

std::string manifest_json(const Manifest& m) {
  Json j;
  j["format"] = m.format;
  Json release;
  release["uuid"] = m.release.uuid.str();
  release["name"] = m.release.name;
  release["version"] = m.release.version;
  release["title"] = m.release.title;
  release["description"] = text_or_null(m.release.description);
  release["state"] = m.release.state;
  release["visibility"] = m.release.visibility;
  release["licence"] = m.release.licence;
  Json authors = Json::array();
  for (const auto& a : m.release.authors) {
    Json author;
    author["name"] = a.name;
    if (a.orcid) author["orcid"] = *a.orcid;
    if (a.affiliation) author["affiliation"] = *a.affiliation;
    authors.push_back(author);
  }
  release["authors"] = authors;
  release["publisher"] = text_or_null(m.release.publisher);
  release["doi"] = text_or_null(m.release.doi);
  Json supersedes;
  supersedes["uuid"] = uuid_or_null(m.release.supersedes);
  supersedes["doi"] = text_or_null(m.release.supersedes_doi);
  release["supersedes"] = supersedes;
  release["created_utc"] = m.release.created.iso();
  release["published_utc"] = time_or_null(m.release.published);
  release["embargo_until_utc"] = time_or_null(m.release.embargo_until);
  j["release"] = release;

  Json build;
  build["built_utc"] = m.build.built.iso();
  build["software"] = m.build.software;
  build["reduction_version"] = m.build.reduction_version;
  build["constants_preset"] = m.build.constants_preset;
  build["options"] = m.build.options_json.empty() ? Json::object() : Json::parse(m.build.options_json, nullptr, false);
  Json store;
  store["kind"] = m.build.store_kind;
  store["schema_version"] = m.build.schema_version;
  store["store_id"] = m.build.store_id;
  build["store"] = store;
  build["private_build"] = m.build.private_build;
  Json omitted;
  omitted["samples"] = m.build.omitted_samples;
  omitted["analyses"] = m.build.omitted_analyses;
  build["omitted_embargoed"] = omitted;
  j["build"] = build;

  Json pins;
  pins["bookmark_uuid"] = m.pins.bookmark.str();
  Json analyses = Json::array();
  for (const auto& a : m.pins.analyses) {
    Json p;
    p["uuid"] = a.uuid.str();
    p["runid"] = a.runid;
    p["record_sha256"] = text_or_null(a.record_sha256);
    Json heads = Json::object();
    for (const auto& [kind, revision] : a.heads) heads[kind] = revision;
    p["heads"] = heads;
    Json refs = Json::object();
    for (const auto& [object, revision] : a.refs) refs[object] = revision;
    p["refs"] = refs;
    analyses.push_back(p);
  }
  pins["analyses"] = analyses;
  Json ias = Json::array();
  for (const auto& [ia, revision] : m.pins.interpreted_ages) {
    Json p;
    p["uuid"] = ia.str();
    p["revision"] = revision.str();
    ias.push_back(p);
  }
  pins["interpreted_ages"] = ias;
  Json samples = Json::array();
  for (const auto& s : m.pins.samples) samples.push_back(s.str());
  pins["samples"] = samples;
  j["pins"] = pins;

  Json tables = Json::array();
  for (const auto& t : m.tables) {
    Json table;
    table["path"] = t.path;
    table["rows"] = t.rows;
    table["rows_sha256"] = to_hex(t.rows_sha256);
    table["columns"] = t.columns;
    tables.push_back(table);
  }
  j["tables"] = tables;
  Json files = Json::array();
  for (const auto& f : m.files) {
    Json file;
    file["path"] = f.path;
    file["bytes"] = f.bytes;
    file["sha256"] = to_hex(f.sha256);
    file["media_type"] = f.media_type;
    files.push_back(file);
  }
  j["files"] = files;
  return j.dump(2) + "\n";
}

Result<Manifest> parse_manifest(std::string_view json) {
  const Json j = Json::parse(json, nullptr, false);
  if (!j.is_object()) return fail(ErrorKind::Config, "manifest: not a JSON object");
  if (!j.contains("format") || j["format"] != kManifestFormat)
    return fail(ErrorKind::Config, std::string("manifest: not ") + std::string(kManifestFormat));
  for (const char* key : {"release", "build", "pins", "tables", "files"})
    if (!j.contains(key)) return fail(ErrorKind::Config, std::string("manifest: no ") + key);
  Manifest m;
  m.format = j["format"].get<std::string>();
  const Json& r = j["release"];
  auto uuid = uuid_of(r.value("uuid", Json(nullptr)), "release.uuid");
  if (!uuid || !*uuid) return fail(ErrorKind::Config, "manifest: release.uuid missing");
  m.release.uuid = **uuid;
  m.release.name = r.value("name", "");
  m.release.version = r.value("version", 1);
  m.release.title = r.value("title", "");
  m.release.description = opt_string(r.value("description", Json(nullptr)));
  m.release.state = r.value("state", "");
  m.release.visibility = r.value("visibility", "");
  m.release.licence = r.value("licence", "");
  if (r.contains("authors") && r["authors"].is_array()) {
    for (const auto& a : r["authors"]) {
      Author author;
      author.name = a.value("name", "");
      author.orcid = opt_string(a.value("orcid", Json(nullptr)));
      author.affiliation = opt_string(a.value("affiliation", Json(nullptr)));
      m.release.authors.push_back(author);
    }
  }
  m.release.publisher = opt_string(r.value("publisher", Json(nullptr)));
  m.release.doi = opt_string(r.value("doi", Json(nullptr)));
  if (r.contains("supersedes") && r["supersedes"].is_object()) {
    auto s = uuid_of(r["supersedes"].value("uuid", Json(nullptr)), "release.supersedes.uuid");
    if (!s) return fail(s.error());
    m.release.supersedes = *s;
    m.release.supersedes_doi = opt_string(r["supersedes"].value("doi", Json(nullptr)));
  }
  auto created = time_of(r.value("created_utc", Json(nullptr)), "release.created_utc");
  if (!created || !*created) return fail(ErrorKind::Config, "manifest: release.created_utc missing");
  m.release.created = **created;
  auto published = time_of(r.value("published_utc", Json(nullptr)), "release.published_utc");
  if (!published) return fail(published.error());
  m.release.published = *published;
  auto embargo = time_of(r.value("embargo_until_utc", Json(nullptr)), "release.embargo_until_utc");
  if (!embargo) return fail(embargo.error());
  m.release.embargo_until = *embargo;

  const Json& b = j["build"];
  auto built = time_of(b.value("built_utc", Json(nullptr)), "build.built_utc");
  if (!built || !*built) return fail(ErrorKind::Config, "manifest: build.built_utc missing");
  m.build.built = **built;
  m.build.software = b.value("software", "");
  m.build.reduction_version = b.value("reduction_version", "");
  m.build.constants_preset = b.value("constants_preset", "");
  m.build.options_json = b.contains("options") ? b["options"].dump() : "{}";
  if (b.contains("store") && b["store"].is_object()) {
    m.build.store_kind = b["store"].value("kind", "");
    m.build.schema_version = b["store"].value("schema_version", 0);
    m.build.store_id = b["store"].value("store_id", "");
  }
  m.build.private_build = b.value("private_build", false);
  if (b.contains("omitted_embargoed") && b["omitted_embargoed"].is_object()) {
    m.build.omitted_samples = b["omitted_embargoed"].value("samples", 0);
    m.build.omitted_analyses = b["omitted_embargoed"].value("analyses", 0);
  }

  const Json& p = j["pins"];
  auto bookmark = uuid_of(p.value("bookmark_uuid", Json(nullptr)), "pins.bookmark_uuid");
  if (!bookmark || !*bookmark) return fail(ErrorKind::Config, "manifest: pins.bookmark_uuid missing");
  m.pins.bookmark = **bookmark;
  if (p.contains("analyses"))
    for (const auto& a : p["analyses"]) {
      ManifestAnalysisPins pins;
      auto au = uuid_of(a.value("uuid", Json(nullptr)), "pins.analyses[].uuid");
      if (!au || !*au) return fail(ErrorKind::Config, "manifest: an analysis pin has no uuid");
      pins.uuid = **au;
      pins.runid = a.value("runid", "");
      pins.record_sha256 = opt_string(a.value("record_sha256", Json(nullptr)));
      if (a.contains("heads"))
        for (const auto& [kind, revision] : a["heads"].items()) pins.heads[kind] = revision.get<std::string>();
      if (a.contains("refs"))
        for (const auto& [object, revision] : a["refs"].items()) pins.refs[object] = revision.get<std::string>();
      m.pins.analyses.push_back(std::move(pins));
    }
  if (p.contains("interpreted_ages"))
    for (const auto& ia : p["interpreted_ages"]) {
      auto u = uuid_of(ia.value("uuid", Json(nullptr)), "pins.interpreted_ages[].uuid");
      auto rev = uuid_of(ia.value("revision", Json(nullptr)), "pins.interpreted_ages[].revision");
      if (!u || !*u || !rev || !*rev) return fail(ErrorKind::Config, "manifest: an interpreted age pin is incomplete");
      m.pins.interpreted_ages.emplace_back(**u, **rev);
    }
  if (p.contains("samples"))
    for (const auto& s : p["samples"]) {
      auto u = uuid_of(s, "pins.samples[]");
      if (!u || !*u) return fail(ErrorKind::Config, "manifest: a sample pin is no uuid");
      m.pins.samples.push_back(**u);
    }

  for (const auto& t : j["tables"]) {
    ManifestTable table;
    table.path = t.value("path", "");
    table.rows = t.value("rows", 0ULL);
    auto d = digest_of(t.value("rows_sha256", Json(nullptr)), "tables[].rows_sha256");
    if (!d) return fail(d.error());
    table.rows_sha256 = *d;
    if (t.contains("columns"))
      for (const auto& c : t["columns"]) table.columns.push_back(c.get<std::string>());
    m.tables.push_back(std::move(table));
  }
  for (const auto& f : j["files"]) {
    ManifestFile file;
    file.path = f.value("path", "");
    file.bytes = f.value("bytes", 0ULL);
    auto d = digest_of(f.value("sha256", Json(nullptr)), "files[].sha256");
    if (!d) return fail(d.error());
    file.sha256 = *d;
    file.media_type = f.value("media_type", "");
    m.files.push_back(std::move(file));
  }
  return m;
}

Result<Manifest> read_manifest(const fs::path& dir) {
  auto text = slurp(dir / kManifestFile);
  if (!text) return fail(text.error());
  return parse_manifest(*text);
}

std::string media_type_of(std::string_view path) {
  const auto dot = path.rfind('.');
  const std::string ext = dot == std::string_view::npos ? "" : std::string(path.substr(dot));
  if (ext == ".parquet") return "application/vnd.apache.parquet";
  if (ext == ".json") return path.starts_with("records/") ? "application/geo+json" : "application/json";
  if (ext == ".csv") return "text/csv";
  if (ext == ".pdf") return "application/pdf";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".md") return "text/markdown";
  if (ext == ".cff" || ext == ".yaml" || ext == ".yml") return "application/yaml";
  if (ext == ".html") return "text/html";
  if (ext == ".js") return "text/javascript";
  if (ext == ".wasm") return "application/wasm";
  return "text/plain";
}

Result<VerifyReport> verify_files(const fs::path& dir) {
  auto manifest = read_manifest(dir);
  if (!manifest) return fail(manifest.error());
  VerifyReport report;
  std::set<std::string> listed;
  for (const auto& f : manifest->files) {
    listed.insert(f.path);
    std::error_code ec;
    const fs::path path = dir / f.path;
    if (!fs::is_regular_file(path, ec)) {
      report.missing.push_back(f.path);
      continue;
    }
    auto bytes = slurp(path);
    if (!bytes) return fail(bytes.error());
    if (bytes->size() != f.bytes || sha256(std::string_view(*bytes)) != f.sha256) report.changed.push_back(f.path);
  }
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
    if (!it->is_regular_file()) continue;
    const std::string rel = fs::relative(it->path(), dir).generic_string();
    if (rel == kManifestFile || listed.contains(rel)) continue;
    report.extra.push_back(rel);
  }
  std::sort(report.missing.begin(), report.missing.end());
  std::sort(report.changed.begin(), report.changed.end());
  std::sort(report.extra.begin(), report.extra.end());
  return report;
}

Result<VerifyReport> verify_rebuild(const fs::path& dir, const Manifest& rebuilt) {
  auto manifest = read_manifest(dir);
  if (!manifest) return fail(manifest.error());
  VerifyReport report;
  if (manifest->build.reduction_version != rebuilt.build.reduction_version) {
    report.reduction_version_differs = "the directory was built with reduction version " +
                                       manifest->build.reduction_version + ", this build is " +
                                       rebuilt.build.reduction_version + ": nothing else is compared";
    return report;
  }
  std::map<std::string, const ManifestTable*> tables;
  for (const auto& t : rebuilt.tables) tables[t.path] = &t;
  for (const auto& t : manifest->tables) {
    auto it = tables.find(t.path);
    if (it == tables.end()) {
      report.missing.push_back(t.path);
      continue;
    }
    if (it->second->rows_sha256 != t.rows_sha256 || it->second->rows != t.rows) report.changed.push_back(t.path);
  }
  std::map<std::string, const ManifestFile*> files;
  for (const auto& f : rebuilt.files) files[f.path] = &f;
  for (const auto& f : manifest->files) {
    if (f.path.ends_with(".parquet")) continue;
    auto it = files.find(f.path);
    if (it == files.end()) {
      report.missing.push_back(f.path);
      continue;
    }
    if (it->second->sha256 != f.sha256) report.changed.push_back(f.path);
  }
  std::sort(report.missing.begin(), report.missing.end());
  std::sort(report.changed.begin(), report.changed.end());
  return report;
}

}  // namespace pychron::publish
