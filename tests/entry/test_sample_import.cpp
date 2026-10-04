#include <gtest/gtest.h>

#include <algorithm>

#include "catalog_fixture.hpp"
#include "pychron/entry/sample_import.hpp"
#include "pychron/entry/sample_search.hpp"

using namespace pychron;
using namespace pychron::entry;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

CatalogSnapshot snapshot() {
  CatalogSnapshot c;
  const Uuid ross = Uuid::v7(), alpha = Uuid::v7(), san = Uuid::v7(), bio = Uuid::v7();
  c.principal_investigators = {{ross, "Ross", "J", "Ross, J", std::nullopt, std::nullopt}};
  c.projects = {{alpha, "Alpha", ross, "Ross, J", std::nullopt, std::nullopt, std::nullopt, std::nullopt, 1}};
  c.materials = {{san, "sanidine", "", 1}, {bio, "biotite", "20-40", 0}};
  SampleRow fc2;
  fc2.uuid = Uuid::v7();
  fc2.name = "FC-2";
  fc2.project = alpha;
  fc2.material = san;
  fc2.principal_investigator = ross;
  fc2.project_name = "Alpha";
  fc2.principal_investigator_name = "Ross, J";
  fc2.material_name = "sanidine";
  fc2.fields.lat = 34.0;
  fc2.fields.lon = -106.0;
  c.samples = {fc2};
  return c;
}

SampleImportPlan plan(const std::string& csv, const CatalogSnapshot& c, ImportOptions o = {}) {
  auto t = read_csv(csv);
  EXPECT_TRUE(t);
  return plan_sample_import(*t, default_mapping(t->header), c, o);
}

}  // namespace

TEST(SampleImport, HeaderAliases) {
  const auto m = default_mapping({"Sample", "PI", "Latitude", "grain size", "mystery", "sample"});
  EXPECT_EQ(m[0], ImportField::Sample);
  EXPECT_EQ(m[1], ImportField::PrincipalInvestigator);
  EXPECT_EQ(m[2], ImportField::Lat);
  EXPECT_EQ(m[3], ImportField::Grainsize);
  EXPECT_FALSE(m[4]);
  EXPECT_FALSE(m[5]);  // the first column of a field wins
}

TEST(SampleImport, EveryRowState) {
  const auto c = snapshot();
  const auto p = plan(
      "sample,project,pi,material,grainsize,lat,lon\n"
      "FC-2,Alpha,\"Ross, J\",sanidine,,34,-106\n"      // exists
      "FC-2,Alpha,\"Ross, J\",sanidine,,35,-106\n"      // the same sample again: error
      "new-1,Alpha,\"Ross, J\",biotite,20-40,,\n"      // create
      "new-2,Beta,Smith,quartz,,,\n",                   // create with a new PI, project, material
      c);
  ASSERT_EQ(p.rows.size(), 4u);
  EXPECT_EQ(p.rows[0].state, RowState::Exists);
  EXPECT_EQ(p.rows[1].state, RowState::Error);
  EXPECT_NE(p.rows[1].messages.front().find("line 2"), std::string::npos);
  EXPECT_EQ(p.rows[2].state, RowState::Create);
  EXPECT_EQ(p.rows[3].state, RowState::Create);
  EXPECT_EQ(p.creates, 2);
  EXPECT_EQ(p.exists, 1);
  EXPECT_EQ(p.errors, 1);
  EXPECT_EQ(p.new_principal_investigators, (std::vector<std::string>{"Smith"}));
  EXPECT_EQ(p.new_projects, (std::vector<std::string>{"Beta (Smith)"}));
  EXPECT_EQ(p.new_materials, (std::vector<std::string>{"quartz"}));
}

TEST(SampleImport, UpdateOnlyWhenAsked) {
  const auto c = snapshot();
  const std::string csv = "sample,project,pi,material,lat,lon,note\nFC-2,Alpha,\"Ross, J\",sanidine,34.5,-106,hi\n";
  auto p = plan(csv, c);
  ASSERT_EQ(p.rows[0].state, RowState::Update);
  EXPECT_EQ(p.rows[0].changed, (std::vector<std::string>{"lat", "note"}));
  EXPECT_TRUE(to_batch(p, c, {}).edits.empty());
  ImportOptions o;
  o.update_existing = true;
  p = plan(csv, c, o);
  const auto batch = to_batch(p, c, o);
  ASSERT_EQ(batch.edits.size(), 1u);
  const auto& u = std::get<CatalogUpdate>(batch.edits[0]);
  EXPECT_EQ(u.expected.at("lat"), CatalogValue{34.0});
  EXPECT_EQ(u.expected.at("note"), CatalogValue{});
  EXPECT_EQ(u.values.at("lat"), CatalogValue{34.5});
}

TEST(SampleImport, EveryErrorOfARowIsListed) {
  const auto c = snapshot();
  const auto p = plan("sample,project,pi,material,lat,lon,elevation\n,1bad,jake ross,,95,,high\n", c);
  ASSERT_EQ(p.rows.size(), 1u);
  const auto& m = p.rows[0].messages;
  const auto has = [&](const char* text) {
    return std::any_of(m.begin(), m.end(), [&](const std::string& s) { return s.find(text) != std::string::npos; });
  };
  EXPECT_TRUE(has("no sample name"));
  EXPECT_TRUE(has("no material"));
  EXPECT_TRUE(has("Last, F"));
  EXPECT_TRUE(has("project '1bad'"));
  EXPECT_TRUE(has("elevation 'high'"));
  EXPECT_TRUE(has("go together"));
}

TEST(SampleImport, GrainsizeIsPartOfTheMaterial) {
  const auto c = snapshot();
  // biotite exists only with grainsize 20-40: plain biotite is a new material.
  const auto p = plan("sample,project,pi,material\nb-1,Alpha,\"Ross, J\",biotite\n", c);
  EXPECT_EQ(p.new_materials, (std::vector<std::string>{"biotite"}));
}

TEST(SampleImport, UtmFillsLatLon) {
  const auto c = snapshot();
  auto p = plan("sample,project,pi,material,easting,northing,zone\nu-1,Alpha,\"Ross, J\",sanidine,323394,4307396,13S\n", c);
  ASSERT_EQ(p.rows[0].state, RowState::Create);
  ASSERT_TRUE(p.rows[0].fields.lat);
  EXPECT_NEAR(*p.rows[0].fields.lat, 38.8977, 1e-3);
  p = plan("sample,project,pi,material,lat,lon,easting,northing,zone\nu-2,Alpha,\"Ross, J\",sanidine,1,2,323394,4307396,13S\n", c);
  EXPECT_EQ(*p.rows[0].fields.lat, 1.0);  // lat/lon given: UTM ignored
  p = plan("sample,project,pi,material,easting\nu-3,Alpha,\"Ross, J\",sanidine,323394\n", c);
  EXPECT_EQ(p.rows[0].state, RowState::Error);
}

TEST(SampleImport, TemplateParsesBack) {
  auto t = read_csv(template_csv());
  ASSERT_TRUE(t);
  const auto m = default_mapping(t->header);
  ASSERT_EQ(m.size(), import_fields().size());
  for (std::size_t i = 0; i < m.size(); ++i) EXPECT_EQ(m[i], import_fields()[i]);
}

TEST(SampleImport, BatchInsertsInKeyOrderOnce) {
  const auto c = snapshot();
  const auto p = plan("sample,project,pi,material\na,Beta,Smith,quartz\nb,Beta,Smith,quartz\n", c);
  const auto batch = to_batch(p, c, {});
  ASSERT_EQ(batch.edits.size(), 5u);  // one PI, one project, one material, two samples
  EXPECT_EQ(std::get<CatalogInsert>(batch.edits[0]).table, CatalogTable::PrincipalInvestigator);
  EXPECT_EQ(std::get<CatalogInsert>(batch.edits[1]).table, CatalogTable::Project);
  EXPECT_EQ(std::get<CatalogInsert>(batch.edits[2]).table, CatalogTable::Material);
  EXPECT_EQ(std::get<CatalogInsert>(batch.edits[3]).table, CatalogTable::Sample);
}

TEST(SampleSearch, NearDuplicates) {
  const auto c = snapshot();
  EXPECT_EQ(near_duplicates("fc 2", c.samples).size(), 1u);
  EXPECT_EQ(near_duplicates("FC_2", c.samples).size(), 1u);
  EXPECT_TRUE(near_duplicates("FC-3", c.samples).empty());
  EXPECT_TRUE(near_duplicates(" - ", c.samples).empty());
}

namespace {
class SampleImportStoreTest : public StoreTest {};
}  // namespace

TEST_P(SampleImportStoreTest, ImportThenReimportIsAllExists) {
  const std::string csv =
      "sample,project,principal_investigator,material,grainsize,lat,lon,note\n"
      "s-1,Delta,\"Doe, A\",sanidine,,34.1,-106.2,first\n"
      "s-2,Delta,\"Doe, A\",biotite,20-40,,,\n";
  auto t = read_csv(csv);
  ASSERT_TRUE(t);
  auto c = read_snapshot(*store_);
  ASSERT_TRUE(c);
  auto p = plan_sample_import(*t, default_mapping(t->header), *c, {});
  ASSERT_EQ(p.creates, 2);
  auto out = store_->apply_catalog_edits(lab_.reduction_client, to_batch(p, *c, {}));
  ASSERT_TRUE(out);
  ASSERT_TRUE(std::holds_alternative<CatalogApplied>(*out)) << describe(*out);

  c = read_snapshot(*store_);
  ASSERT_TRUE(c);
  p = plan_sample_import(*t, default_mapping(t->header), *c, {});
  EXPECT_EQ(p.exists, 2);
  EXPECT_EQ(p.creates, 0);
  EXPECT_TRUE(to_batch(p, *c, {}).edits.empty());
}

INSTANTIATE_TEST_SUITE_P(Engines, SampleImportStoreTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
