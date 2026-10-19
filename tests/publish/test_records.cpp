// The OGC API - Records catalogue of a release (data publishing design,
// section 5.4): catalog.json and the records.

#include "pychron/publish/records.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "table_inputs.hpp"

namespace ps = pychron::persistence;
using namespace pychron;
using namespace pychron::publish;
using pychron::publish::testing::inputs;

namespace {

Manifest manifest_of(const TableInputs& in, const ReleaseTables& tables) {
  Manifest m;
  m.release.uuid = in.release.uuid;
  m.release.name = "r";
  m.release.version = 1;
  m.release.title = "Release r";
  m.release.description = "A test";
  m.release.state = "draft";
  m.release.visibility = "private";
  m.release.licence = "CC-BY-4.0";
  m.release.authors = {{"J. Ross", std::nullopt, std::nullopt}};
  m.release.doi = "10.1234/abcd";
  m.release.created = *ps::UtcTime::parse("2026-10-18T00:00:00Z");
  m.build.built = m.release.created;
  m.build.software = "pychron-cpp/0.4.0";
  m.build.reduction_version = "arar-1";
  for (const auto& s : in.samples) m.pins.samples.push_back(s.uuid);
  for (const Table* t : {&tables.samples, &tables.positions, &tables.analyses, &tables.isotopes, &tables.interpreted_ages})
    m.tables.push_back({"data/" + t->name + ".parquet", t->rows.size(), rows_sha256(*t), {}});
  for (const auto* path : {"data/samples.parquet", "tables/release.csv", "tables/FC-2/report.csv"})
    m.files.push_back({path, 1, sha256(std::string_view{path}), std::string(media_type_of(path))});
  return m;
}

std::vector<nlohmann::json> links(const nlohmann::json& j, const std::string& rel) {
  std::vector<nlohmann::json> out;
  for (const auto& l : j.at("links"))
    if (l.at("rel") == rel) out.push_back(l);
  return out;
}

}  // namespace

TEST(Records, CatalogHasItemLinkPerSampleAndRelease) {
  const auto in = inputs();
  const auto tables = build_tables(in);
  const auto j = nlohmann::json::parse(catalog_json(manifest_of(in, tables), tables));
  EXPECT_EQ(j.at("id"), "r-v1");
  EXPECT_EQ(j.at("type"), "Collection");
  EXPECT_EQ(j.at("itemType"), "record");
  EXPECT_EQ(j.at("title"), "Release r");
  EXPECT_EQ(j.at("license"), "CC-BY-4.0");
  EXPECT_EQ(j.at("conformsTo")[0], "http://www.opengis.net/spec/ogcapi-records-1/1.0/req/record-core");
  const auto items = links(j, "item");
  ASSERT_EQ(items.size(), 3u) << "two samples and the release";
  EXPECT_EQ(items[0].at("href"), "records/" + in.samples[1].uuid.str() + ".json") << "FC-2 first, by name";
  EXPECT_EQ(items[0].at("type"), "application/geo+json");
  EXPECT_EQ(items[0].at("title"), "FC-2");
  EXPECT_EQ(items[2].at("href"), "records/release.json");
  ASSERT_EQ(links(j, "cite-as").size(), 1u);
  EXPECT_EQ(links(j, "cite-as")[0].at("href"), "https://doi.org/10.1234/abcd");
  EXPECT_EQ(links(j, "enclosure").size(), 1u) << "one per table that was written (files lists one)";
  EXPECT_EQ(links(j, "enclosure")[0].at("href"), "data/samples.parquet");
  ASSERT_EQ(links(j, "describedby").size(), 1u);
  EXPECT_EQ(links(j, "describedby")[0].at("href"), "manifest.json");
  EXPECT_EQ(j.at("extent").at("spatial").at("crs"), "http://www.opengis.net/def/crs/OGC/1.3/CRS84");
}

TEST(Records, CatalogExtentSkipsSamplesWithoutLocation) {
  const auto in = inputs(false);  // FC-2 has no location; U1 sample does
  const auto tables = build_tables(in);
  const auto j = nlohmann::json::parse(catalog_json(manifest_of(in, tables), tables));
  const auto bbox = j.at("extent").at("spatial").at("bbox")[0];
  EXPECT_DOUBLE_EQ(bbox[0].get<double>(), -106.9);
  EXPECT_DOUBLE_EQ(bbox[1].get<double>(), 37.75);
  EXPECT_DOUBLE_EQ(bbox[2].get<double>(), -106.9);
  EXPECT_DOUBLE_EQ(bbox[3].get<double>(), 37.75);
  const auto interval = j.at("extent").at("temporal").at("interval")[0];
  EXPECT_EQ(interval[0], "2023-11-14T22:03:20.000000Z");
  EXPECT_EQ(interval[1], "2023-11-14T23:13:20.000000Z");

  auto none = inputs(false);
  none.samples[0].fields.lat = std::nullopt;  // U1 sample loses its location too
  none.samples[0].fields.lon = std::nullopt;
  const auto t2 = build_tables(none);
  const auto j2 = nlohmann::json::parse(catalog_json(manifest_of(none, t2), t2));
  EXPECT_FALSE(j2.at("extent").contains("spatial"));
}

TEST(Records, SampleRecordCarriesIgsnAsExternalId) {
  const auto in = inputs();
  const auto tables = build_tables(in);
  const auto m = manifest_of(in, tables);
  auto record = sample_record_json(m, tables, in.samples[1].uuid);  // FC-2
  ASSERT_TRUE(record) << to_string(record.error());
  const auto j = nlohmann::json::parse(*record);
  EXPECT_EQ(j.at("type"), "Feature");
  EXPECT_EQ(j.at("id"), in.samples[1].uuid.str());
  EXPECT_EQ(j.at("geometry").at("type"), "Point");
  EXPECT_DOUBLE_EQ(j.at("geometry").at("coordinates")[0].get<double>(), -106.9);
  EXPECT_EQ(j.at("time").at("interval")[0], "2023-11-14T22:13:20.000000Z");
  const auto& p = j.at("properties");
  EXPECT_EQ(p.at("type"), "dataset");
  EXPECT_EQ(p.at("title"), "FC-2 (sanidine)");
  EXPECT_EQ(p.at("license"), "CC-BY-4.0");
  EXPECT_EQ(p.at("externalIds")[0].at("scheme"), "igsn");
  EXPECT_EQ(p.at("externalIds")[0].at("value"), "IEFC20001");
  EXPECT_EQ(p.at("pychron:sample_uuid"), in.samples[1].uuid.str());
  EXPECT_EQ(p.at("pychron:n_analyses"), 3);
  EXPECT_EQ(p.at("contacts")[0].at("name"), "J Ross");
  ASSERT_EQ(links(j, "collection").size(), 1u);
  EXPECT_EQ(links(j, "collection")[0].at("href"), "../catalog.json");
  const auto enclosures = links(j, "enclosure");
  ASSERT_FALSE(enclosures.empty());
  EXPECT_EQ(enclosures[0].at("href"), "../tables/FC-2/report.csv");
  EXPECT_FALSE(sample_record_json(m, tables, ps::Uuid::v7()));

  // A sample without a location: geometry null; no IGSN: no externalIds.
  auto bare = inputs(false);
  bare.samples[1].fields.igsn = std::nullopt;
  const auto t2 = build_tables(bare);
  const auto j2 = nlohmann::json::parse(*sample_record_json(manifest_of(bare, t2), t2, bare.samples[1].uuid));
  EXPECT_TRUE(j2.at("geometry").is_null());
  EXPECT_TRUE(j2.at("properties").at("externalIds").empty());
}

TEST(Records, ReleaseRecordLinksEveryEnclosure) {
  const auto in = inputs();
  const auto tables = build_tables(in);
  const auto m = manifest_of(in, tables);
  const auto j = nlohmann::json::parse(release_record_json(m, tables));
  EXPECT_EQ(j.at("id"), "release");
  EXPECT_EQ(j.at("properties").at("title"), "Release r");
  EXPECT_EQ(j.at("properties").at("externalIds")[0].at("scheme"), "doi");
  EXPECT_EQ(j.at("properties").at("contacts")[0].at("name"), "J. Ross");
  EXPECT_EQ(links(j, "enclosure").size(), m.files.size()) << "every file of the manifest";
  EXPECT_EQ(links(j, "enclosure")[0].at("href"), "../data/samples.parquet");
  EXPECT_EQ(j.at("properties").at("pychron:n_samples"), 2);
  EXPECT_EQ(j.at("properties").at("pychron:n_analyses"), 4);
}

TEST(Records, SampleDirNamesAreSafeAndUnique) {
  auto in = inputs();
  in.samples[0].name = "FC-2";  // U1 sample renamed to clash with FC-2
  in.samples[1].name = "FC-2/x";
  const auto tables = build_tables(in);
  // Safe names: "FC-2" and "FC-2_x": distinct, no suffix.
  EXPECT_EQ(sample_dir_name(tables.samples, 0), "FC-2");
  EXPECT_EQ(sample_dir_name(tables.samples, 1), "FC-2_x");
  in.samples[1].name = "FC-2";
  const auto t2 = build_tables(in);
  const std::string a = sample_dir_name(t2.samples, 0), b = sample_dir_name(t2.samples, 1);
  EXPECT_NE(a, b);
  EXPECT_EQ(a.rfind("FC-2-", 0), 0u);
  EXPECT_EQ(a.size(), std::string("FC-2-").size() + 8);
}
