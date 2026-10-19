// The OGC API - Records crawlable catalogue of a release (data publishing
// design, section 5.4).

#include "pychron/publish/records.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace pychron::publish {

using Json = nlohmann::ordered_json;

namespace {

constexpr const char* kRecordCore = "http://www.opengis.net/spec/ogcapi-records-1/1.0/req/record-core";
constexpr const char* kCrs84 = "http://www.opengis.net/def/crs/OGC/1.3/CRS84";

// A cell as JSON: null, number, text, time text, bool, or the parsed JSON.
Json json_of(const Cell& c) {
  struct Visitor {
    Json operator()(std::monostate) const { return nullptr; }
    Json operator()(double v) const { return v; }
    Json operator()(std::int32_t v) const { return v; }
    Json operator()(std::int64_t v) const { return v; }
    Json operator()(bool v) const { return v; }
    Json operator()(const std::string& v) const { return v; }
    Json operator()(const UtcTime& v) const { return v.iso(); }
    Json operator()(const JsonText& v) const { return Json::parse(v.value, nullptr, false); }
    Json operator()(const WkbBytes&) const { return nullptr; }
  };
  return std::visit(Visitor{}, c);
}

const Cell& cell(const Table& t, std::size_t row, const char* column) {
  static const Cell kNull{std::monostate{}};
  const auto c = t.column(column);
  return c ? t.rows[row][*c] : kNull;
}

std::string str(const Table& t, std::size_t row, const char* column) {
  const auto& c = cell(t, row, column);
  if (const auto* s = std::get_if<std::string>(&c)) return *s;
  return {};
}

struct Point {
  double lon, lat;
};

std::optional<Point> point_of(const Table& t, std::size_t row) {
  const auto& c = cell(t, row, "bbox");
  if (const auto* j = std::get_if<JsonText>(&c)) {
    const Json b = Json::parse(j->value, nullptr, false);
    if (b.is_object() && b.contains("xmin")) return Point{b["xmin"].get<double>(), b["ymin"].get<double>()};
  }
  return std::nullopt;
}

Json geometry_of(std::optional<Point> p) {
  if (!p) return nullptr;
  Json g;
  g["type"] = "Point";
  g["coordinates"] = Json::array({p->lon, p->lat});
  return g;
}

Json link(const std::string& rel, const std::string& href, const std::string& type = "",
          const std::string& title = "") {
  Json l;
  l["rel"] = rel;
  l["href"] = href;
  if (!type.empty()) l["type"] = type;
  if (!title.empty()) l["title"] = title;
  return l;
}

Json contacts_of(const std::vector<Author>& authors) {
  Json out = Json::array();
  for (const auto& a : authors) {
    Json c;
    c["name"] = a.name;
    if (a.affiliation) c["organization"] = *a.affiliation;
    if (a.orcid) c["links"] = Json::array({link("about", "https://orcid.org/" + *a.orcid)});
    c["roles"] = Json::array({"author"});
    out.push_back(c);
  }
  return out;
}

std::string release_id(const Manifest& m) { return m.release.name + "-v" + std::to_string(m.release.version); }

// Times over the samples that have them.
std::pair<std::optional<std::string>, std::optional<std::string>> interval_of(const Table& samples) {
  std::optional<UtcTime> first, last;
  for (std::size_t r = 0; r < samples.rows.size(); ++r) {
    if (const auto* f = std::get_if<UtcTime>(&cell(samples, r, "first_analysis_utc")))
      first = first ? std::min(*first, *f) : *f;
    if (const auto* l = std::get_if<UtcTime>(&cell(samples, r, "last_analysis_utc")))
      last = last ? std::max(*last, *l) : *l;
  }
  return {first ? std::optional(first->iso()) : std::nullopt, last ? std::optional(last->iso()) : std::nullopt};
}

std::optional<std::size_t> row_of_sample(const Table& samples, const Uuid& sample) {
  for (std::size_t r = 0; r < samples.rows.size(); ++r)
    if (str(samples, r, "sample_uuid") == sample.str()) return r;
  return std::nullopt;
}

// The interpreted ages of a sample as [{kind, age_ma, err_ma, n}].
Json ages_of(const ReleaseTables& t, const std::string& sample_uuid) {
  Json out = Json::array();
  const auto& ia = t.interpreted_ages;
  for (std::size_t r = 0; r < ia.rows.size(); ++r) {
    if (str(ia, r, "sample_uuid") != sample_uuid) continue;
    Json a;
    a["name"] = str(ia, r, "name");
    a["kind"] = str(ia, r, "age_kind");
    a["age_ma"] = json_of(cell(ia, r, "age_ma"));
    a["err_ma"] = json_of(cell(ia, r, "age_ma_err"));
    a["n"] = json_of(cell(ia, r, "n"));
    out.push_back(a);
  }
  return out;
}

}  // namespace

std::string sample_dir_name(const Table& samples, std::size_t row) {
  std::string safe = file_safe(str(samples, row, "name"));
  std::size_t same = 0;
  for (std::size_t r = 0; r < samples.rows.size(); ++r)
    if (file_safe(str(samples, r, "name")) == safe) ++same;
  if (same <= 1) return safe;
  // The last 8 hex of the uuid: a v7's first 8 are a timestamp that rows made
  // in the same millisecond share.
  const std::string uuid = str(samples, row, "sample_uuid");
  return safe + "-" + uuid.substr(uuid.size() - 8);
}

std::string catalog_json(const Manifest& m, const ReleaseTables& t) {
  Json j;
  j["id"] = release_id(m);
  j["type"] = "Collection";
  j["itemType"] = "record";
  j["title"] = m.release.title;
  if (m.release.description) j["description"] = *m.release.description;
  Json extent = Json::object();
  double w = 180, s = 90, e = -180, n = -90;
  bool any = false;
  for (std::size_t r = 0; r < t.samples.rows.size(); ++r)
    if (auto p = point_of(t.samples, r)) {
      any = true;
      w = std::min(w, p->lon);
      e = std::max(e, p->lon);
      s = std::min(s, p->lat);
      n = std::max(n, p->lat);
    }
  if (any) {
    Json spatial;
    spatial["bbox"] = Json::array({Json::array({w, s, e, n})});
    spatial["crs"] = kCrs84;
    extent["spatial"] = spatial;
  }
  const auto [first, last] = interval_of(t.samples);
  if (first || last) {
    Json temporal;
    temporal["interval"] = Json::array({Json::array({first ? Json(*first) : Json(nullptr), last ? Json(*last) : Json(nullptr)})});
    temporal["trs"] = "http://www.opengis.net/def/uom/ISO-8601/0/Gregorian";
    extent["temporal"] = temporal;
  }
  j["extent"] = extent;
  j["license"] = m.release.licence;
  j["created"] = m.release.created.iso();
  j["updated"] = (m.release.published ? *m.release.published : m.release.created).iso();
  std::set<std::string> keywords{"40Ar/39Ar", "geochronology"};
  for (std::size_t r = 0; r < t.samples.rows.size(); ++r)
    for (const char* c : {"material", "lithology"})
      if (const std::string v = str(t.samples, r, c); !v.empty()) keywords.insert(v);
  j["keywords"] = Json(keywords);
  j["conformsTo"] = Json::array({kRecordCore});
  Json links = Json::array();
  links.push_back(link("self", "catalog.json", "application/json"));
  links.push_back(link("root", "catalog.json", "application/json"));
  links.push_back(link("license", "LICENSE.txt", "text/plain", m.release.licence));
  links.push_back(link("describedby", "manifest.json", "application/json", "manifest"));
  if (m.release.doi) links.push_back(link("cite-as", "https://doi.org/" + *m.release.doi));
  // The tables that were written (a build without Arrow has none).
  for (const auto& table : m.tables)
    for (const auto& f : m.files)
      if (f.path == table.path) links.push_back(link("enclosure", table.path, std::string(media_type_of(table.path)), table.path));
  for (std::size_t r = 0; r < t.samples.rows.size(); ++r)
    links.push_back(link("item", "records/" + str(t.samples, r, "sample_uuid") + ".json", "application/geo+json",
                         str(t.samples, r, "name")));
  links.push_back(link("item", "records/release.json", "application/geo+json", m.release.title));
  j["links"] = links;
  return j.dump(2) + "\n";
}

Result<std::string> sample_record_json(const Manifest& m, const ReleaseTables& t, const Uuid& sample) {
  const auto row = row_of_sample(t.samples, sample);
  if (!row) return fail(ErrorKind::Config, "sample " + sample.str() + " is not in the release");
  const std::size_t r = *row;
  const std::string name = str(t.samples, r, "name"), material = str(t.samples, r, "material"),
                    project = str(t.samples, r, "project"), uuid = str(t.samples, r, "sample_uuid");
  Json j;
  j["type"] = "Feature";
  j["id"] = uuid;
  j["geometry"] = geometry_of(point_of(t.samples, r));
  const auto& first = cell(t.samples, r, "first_analysis_utc");
  const auto& last = cell(t.samples, r, "last_analysis_utc");
  if (std::holds_alternative<UtcTime>(first) || std::holds_alternative<UtcTime>(last)) {
    Json time;
    time["interval"] = Json::array({json_of(first), json_of(last)});
    j["time"] = time;
  } else {
    j["time"] = nullptr;
  }
  Json p;
  p["type"] = "dataset";
  p["title"] = name + " (" + material + ")";
  const auto n_analyses = json_of(cell(t.samples, r, "n_analyses"));
  std::string description = "Sample " + name + " of project " + project;
  if (const std::string pi = str(t.samples, r, "principal_investigator"); !pi.empty()) description += " (" + pi + ")";
  description += ": " + std::to_string(n_analyses.is_number() ? n_analyses.get<int>() : 0) + " analyses";
  if (const std::string lith = str(t.samples, r, "lithology"); !lith.empty()) description += ", " + lith;
  p["description"] = description;
  Json keywords = Json::array();
  std::set<std::string> kws{material};
  for (const char* c : {"lithology", "unit", "location"})
    if (const std::string v = str(t.samples, r, c); !v.empty()) kws.insert(v);
  for (std::size_t a = 0; a < t.positions.rows.size(); ++a)
    if (str(t.positions, a, "sample_uuid") == uuid)
      if (const std::string irr = str(t.positions, a, "irradiation"); !irr.empty()) kws.insert(irr);
  p["keywords"] = Json(kws);
  p["language"] = "en";
  p["license"] = m.release.licence;
  p["created"] = m.release.created.iso();
  p["updated"] = (m.release.published ? *m.release.published : m.release.created).iso();
  Json external = Json::array();
  if (const std::string igsn = str(t.samples, r, "igsn"); !igsn.empty()) {
    Json e;
    e["scheme"] = "igsn";
    e["value"] = igsn;
    external.push_back(e);
  }
  p["externalIds"] = external;
  Json contacts = Json::array();
  if (const std::string pi = str(t.samples, r, "principal_investigator"); !pi.empty()) {
    Json c;
    c["name"] = pi;
    c["roles"] = Json::array({"principalInvestigator"});
    contacts.push_back(c);
  }
  p["contacts"] = contacts;
  p["formats"] = Json::array({"GeoParquet", "CSV"});
  Json theme;
  theme["scheme"] = "https://pychronlabs.com/themes";
  theme["concepts"] = Json::array({Json{{"id", "40Ar/39Ar"}}});
  p["themes"] = Json::array({theme});
  p["pychron:sample_uuid"] = uuid;
  p["pychron:n_analyses"] = n_analyses;
  p["pychron:n_ages"] = json_of(cell(t.samples, r, "n_ages"));
  p["pychron:ages"] = ages_of(t, uuid);
  j["properties"] = p;
  Json links = Json::array();
  links.push_back(link("self", uuid + ".json", "application/geo+json"));
  links.push_back(link("collection", "../catalog.json", "application/json"));
  const std::string dir = sample_dir_name(t.samples, r);
  for (const auto& f : m.files) {
    if (f.path.starts_with("tables/" + dir + "/") || f.path.starts_with("figures/" + dir + "/"))
      links.push_back(link("enclosure", "../" + f.path, f.media_type, f.path));
  }
  if (const std::string igsn = str(t.samples, r, "igsn"); !igsn.empty() && igsn.starts_with("10."))
    links.push_back(link("related", "https://doi.org/" + igsn, "", "IGSN"));
  j["links"] = links;
  return j.dump(2) + "\n";
}

std::string release_record_json(const Manifest& m, const ReleaseTables& t) {
  Json j;
  j["type"] = "Feature";
  j["id"] = "release";
  // The release's place: the centre of its samples' bbox, when any has one.
  std::optional<Point> centre;
  {
    double w = 180, s = 90, e = -180, n = -90;
    bool any = false;
    for (std::size_t r = 0; r < t.samples.rows.size(); ++r)
      if (auto p = point_of(t.samples, r)) {
        any = true;
        w = std::min(w, p->lon);
        e = std::max(e, p->lon);
        s = std::min(s, p->lat);
        n = std::max(n, p->lat);
      }
    if (any) centre = Point{(w + e) / 2, (s + n) / 2};
  }
  j["geometry"] = geometry_of(centre);
  const auto [first, last] = interval_of(t.samples);
  if (first || last) {
    Json time;
    time["interval"] = Json::array({first ? Json(*first) : Json(nullptr), last ? Json(*last) : Json(nullptr)});
    j["time"] = time;
  } else {
    j["time"] = nullptr;
  }
  Json p;
  p["type"] = "dataset";
  p["title"] = m.release.title;
  p["description"] = (m.release.description ? *m.release.description + " " : std::string()) + "pychron-cpp release " +
                     release_id(m) + ": " + std::to_string(t.analyses.rows.size()) + " analyses, " +
                     std::to_string(t.samples.rows.size()) + " samples.";
  p["keywords"] = Json::array({"40Ar/39Ar", "geochronology", "release"});
  p["language"] = "en";
  p["license"] = m.release.licence;
  p["created"] = m.release.created.iso();
  p["updated"] = (m.release.published ? *m.release.published : m.release.created).iso();
  Json external = Json::array();
  if (m.release.doi) {
    Json e;
    e["scheme"] = "doi";
    e["value"] = *m.release.doi;
    external.push_back(e);
  }
  p["externalIds"] = external;
  p["contacts"] = contacts_of(m.release.authors);
  p["formats"] = Json::array({"GeoParquet", "CSV", "JSON"});
  p["pychron:release_uuid"] = m.release.uuid.str();
  p["pychron:version"] = m.release.version;
  p["pychron:n_samples"] = t.samples.rows.size();
  p["pychron:n_analyses"] = t.analyses.rows.size();
  p["pychron:software"] = m.build.software;
  j["properties"] = p;
  Json links = Json::array();
  links.push_back(link("self", "release.json", "application/geo+json"));
  links.push_back(link("collection", "../catalog.json", "application/json"));
  if (m.release.doi) links.push_back(link("cite-as", "https://doi.org/" + *m.release.doi));
  if (m.release.supersedes_doi) links.push_back(link("predecessor-version", "https://doi.org/" + *m.release.supersedes_doi));
  for (const auto& f : m.files) links.push_back(link("enclosure", "../" + f.path, f.media_type, f.path));
  j["links"] = links;
  return j.dump(2) + "\n";
}

}  // namespace pychron::publish
