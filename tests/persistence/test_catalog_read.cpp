// Entry reads (sample and package entry spec, section 5.1).

#include <gtest/gtest.h>

#include <algorithm>

#include "catalog_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

class CatalogReadTest : public EntryTest {};

std::vector<std::string> names(const std::vector<SampleRow>& rows) {
  std::vector<std::string> out;
  for (const auto& r : rows) out.push_back(r.name);
  std::sort(out.begin(), out.end());  // the engine's collation orders the rows
  return out;
}

}  // namespace

TEST_P(CatalogReadTest, PrincipalInvestigatorsProjectsMaterials) {
  auto pis = store_->principal_investigators();
  ASSERT_TRUE(pis);
  ASSERT_EQ(pis->size(), 2u);
  EXPECT_EQ((*pis)[0].display_name, "Ross, J");
  EXPECT_EQ((*pis)[1].display_name, "Smith");

  auto all = store_->projects(std::nullopt);
  ASSERT_TRUE(all);
  ASSERT_EQ(all->size(), 3u);
  EXPECT_EQ((*all)[0].name, "Alpha");
  EXPECT_EQ((*all)[0].principal_investigator_name, "Ross, J");
  EXPECT_EQ((*all)[0].n_samples, 2);
  auto ross = store_->projects(cat_.ross);
  ASSERT_TRUE(ross);
  EXPECT_EQ(ross->size(), 2u);

  auto materials = store_->materials();
  ASSERT_TRUE(materials);
  ASSERT_EQ(materials->size(), 2u);
  EXPECT_EQ((*materials)[0].name, "biotite");
  EXPECT_EQ((*materials)[0].grainsize, "20-40");
  EXPECT_EQ((*materials)[1].n_samples, 3);
}

TEST_P(CatalogReadTest, SamplesFilterAndCount) {
  auto all = store_->samples({});
  ASSERT_TRUE(all);
  EXPECT_EQ(names(*all), (std::vector<std::string>{"FC-2", "Other", "bt-1", "bt-2", "san_10%"}));

  auto bt = store_->samples({"BT", std::nullopt, std::nullopt, std::nullopt, 500});
  ASSERT_TRUE(bt);
  EXPECT_EQ(names(*bt), (std::vector<std::string>{"bt-1", "bt-2"}));

  // % and _ are text, not patterns.
  auto pct = store_->samples({"%", std::nullopt, std::nullopt, std::nullopt, 500});
  ASSERT_TRUE(pct);
  EXPECT_EQ(names(*pct), (std::vector<std::string>{"san_10%"}));
  auto underscore = store_->samples({"_", std::nullopt, std::nullopt, std::nullopt, 500});
  ASSERT_TRUE(underscore);
  EXPECT_EQ(names(*underscore), (std::vector<std::string>{"san_10%"}));

  auto by_pi = store_->samples({"", cat_.smith, std::nullopt, std::nullopt, 500});
  ASSERT_TRUE(by_pi);
  EXPECT_EQ(names(*by_pi), (std::vector<std::string>{"Other", "san_10%"}));
  auto by_project = store_->samples({"", std::nullopt, cat_.p_ross2, std::nullopt, 500});
  ASSERT_TRUE(by_project);
  EXPECT_EQ(names(*by_project), (std::vector<std::string>{"bt-2"}));
  auto by_material = store_->samples({"", std::nullopt, std::nullopt, cat_.biotite, 500});
  ASSERT_TRUE(by_material);
  EXPECT_EQ(names(*by_material), (std::vector<std::string>{"bt-1", "bt-2"}));
  auto limited = store_->samples({"", std::nullopt, std::nullopt, std::nullopt, 2});
  ASSERT_TRUE(limited);
  EXPECT_EQ(limited->size(), 2u);

  // Counts: FC-2 sits at A1, which gets an identifier and one analysis.
  ASSERT_TRUE(store_->add_identifier(client(), {"70001", "unknown", std::nullopt, std::nullopt, cat_.pos_a1, std::nullopt, std::nullopt}));
  analyze("70001");
  auto fc2 = store_->samples({"FC-2", std::nullopt, std::nullopt, std::nullopt, 500});
  ASSERT_TRUE(fc2);
  ASSERT_EQ(fc2->size(), 1u);
  const SampleRow& r = fc2->front();
  EXPECT_EQ(r.n_positions, 1);
  EXPECT_EQ(r.n_analyses, 1);
  EXPECT_EQ(r.project_name, "Alpha");
  EXPECT_EQ(r.principal_investigator_name, "Ross, J");
  EXPECT_EQ(r.material_name, "sanidine");
  EXPECT_EQ(r.principal_investigator, cat_.ross);
}

TEST_P(CatalogReadTest, LevelsByName) {
  auto levels = store_->levels(cat_.nm301);
  ASSERT_TRUE(levels);
  ASSERT_EQ(levels->size(), 2u);
  EXPECT_EQ((*levels)[0].name, "A");
  EXPECT_EQ((*levels)[1].name, "B");
  EXPECT_EQ((*levels)[0].irradiation, cat_.nm301);
}

TEST_P(CatalogReadTest, LevelSheetJoinsEverything) {
  ASSERT_TRUE(store_->add_identifier(client(), {"70001", "unknown", std::nullopt, std::nullopt, cat_.pos_a1, std::nullopt, std::nullopt}));
  analyze("70001");
  // A flux value for A1, a z and a production for level A.
  const Uuid flux = *store_->add_ref_object(client(), {RefType::FluxPosition, "NM-301/A/1", cat_.nm301, cat_.level_a, cat_.pos_a1, std::nullopt, std::nullopt});
  const Uuid geom = *store_->add_ref_object(client(), {RefType::LevelGeometry, "NM-301/A", cat_.nm301, cat_.level_a, std::nullopt, std::nullopt, std::nullopt});
  const Uuid prod = *store_->add_ref_object(client(), {RefType::Production, "NM-301/Triga", cat_.nm301, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  auto uow = *store_->begin(reducer());
  FluxValue f;
  f.j = 0.0012;
  f.j_err = 0.000001;
  ASSERT_TRUE(uow->add_revision(flux, Kind::RefValue, RevisionPayload{RefPayload{f}}, std::nullopt));
  auto zrev = uow->add_revision(geom, Kind::RefValue, RevisionPayload{RefPayload{LevelZValue{2.5}}}, std::nullopt);
  ASSERT_TRUE(zrev);
  ASSERT_TRUE(uow->add_revision(prod, Kind::RefValue, RevisionPayload{RefPayload{ProductionValue{}}}, std::nullopt));
  ASSERT_TRUE(uow->commit(ChangesetKind::Reference, "refs"));

  auto sheet = store_->level_sheet(cat_.level_a);
  ASSERT_TRUE(sheet && *sheet);
  const LevelSheet& s = **sheet;
  EXPECT_EQ(s.level.name, "A");
  EXPECT_EQ(s.irradiation_name, "NM-301");
  EXPECT_EQ(s.irradiation_kind, "irradiation");
  ASSERT_EQ(s.positions.size(), 2u);
  const PositionRow& a1 = s.positions[0];
  EXPECT_EQ(a1.position, 1);
  EXPECT_EQ(a1.sample_name, "FC-2");
  EXPECT_EQ(a1.project, "Alpha");
  EXPECT_EQ(a1.principal_investigator, "Ross, J");
  EXPECT_EQ(a1.material, "sanidine");
  EXPECT_EQ(a1.identifier, "70001");
  EXPECT_EQ(a1.n_analyses, 1);
  EXPECT_FALSE(a1.in_load);
  EXPECT_EQ(a1.packet, "P1");
  ASSERT_TRUE(a1.j);
  EXPECT_DOUBLE_EQ(*a1.j, 0.0012);
  const PositionRow& a2 = s.positions[1];
  EXPECT_EQ(a2.position, 2);
  EXPECT_EQ(a2.grainsize, "20-40");
  EXPECT_EQ(a2.weight, 1.5);
  EXPECT_FALSE(a2.identifier);
  EXPECT_FALSE(a2.j);
  ASSERT_TRUE(s.geometry);
  EXPECT_EQ(s.geometry->ref_object, geom);
  EXPECT_EQ(s.geometry->revision, *zrev);
  ASSERT_TRUE(s.z);
  EXPECT_EQ(s.z->z, 2.5);
  EXPECT_FALSE(s.production);

  auto none = store_->level_sheet(Uuid::v7());
  ASSERT_TRUE(none);
  EXPECT_FALSE(*none);
}

TEST_P(CatalogReadTest, IrradiationCountsAndKind) {
  ASSERT_TRUE(store_->add_identifier(client(), {"70001", "unknown", std::nullopt, std::nullopt, cat_.pos_a1, std::nullopt, std::nullopt}));
  analyze("70001");
  const Uuid pkg = *store_->add_irradiation(client(), IrradiationSpec{"P-1", std::nullopt, std::string("package")});
  // Ensure keeps the stored kind.
  EXPECT_EQ(*store_->add_irradiation(client(), IrradiationSpec{"P-1", std::nullopt, std::string("irradiation")}), pkg);
  EXPECT_FALSE(store_->add_irradiation(client(), IrradiationSpec{"P-2", std::nullopt, std::string("other")}));

  auto rows = store_->irradiations();
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 3u);
  const auto find = [&](const std::string& name) {
    return *std::find_if(rows->begin(), rows->end(), [&](const IrradiationRow& r) { return r.name == name; });
  };
  const IrradiationRow nm301 = find("NM-301");
  EXPECT_EQ(nm301.kind, "irradiation");
  EXPECT_EQ(nm301.n_levels, 2);
  EXPECT_EQ(nm301.n_positions, 3);
  EXPECT_EQ(nm301.n_analyzed, 1);
  EXPECT_FALSE(nm301.has_chronology);
  EXPECT_EQ(find("P-1").kind, "package");
  EXPECT_EQ(find("NM-300").n_levels, 1);
}

TEST_P(CatalogReadTest, RefObjectsByTypeAndPackage) {
  const Uuid triga = *store_->add_ref_object(client(), {RefType::Production, "NM-301/Triga", cat_.nm301, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  const Uuid other = *store_->add_ref_object(client(), {RefType::Production, "NM-300/Triga", lab_.irradiation, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  auto uow = *store_->begin(reducer());
  auto rev = uow->add_revision(triga, Kind::RefValue, RevisionPayload{RefPayload{ProductionValue{}}}, std::nullopt);
  ASSERT_TRUE(rev);
  ASSERT_TRUE(uow->commit(ChangesetKind::Reference, "p"));
  auto mine = store_->ref_objects(RefType::Production, cat_.nm301);
  ASSERT_TRUE(mine);
  ASSERT_EQ(mine->size(), 1u);
  EXPECT_EQ(mine->front().key, "NM-301/Triga");
  EXPECT_EQ(mine->front().head, *rev);
  auto all = store_->ref_objects(RefType::Production, std::nullopt);
  ASSERT_TRUE(all);
  ASSERT_EQ(all->size(), 2u);
  EXPECT_EQ(all->front().uuid, other);
  EXPECT_FALSE(all->front().head);
}

TEST_P(CatalogReadTest, CounterAbsentIsNullopt) {
  auto counter = store_->identifier_counter(std::string(kIdentifierScope));
  ASSERT_TRUE(counter);
  EXPECT_FALSE(*counter);
}

TEST_P(CatalogReadTest, MaxNumericIdentifier) {
  // seed_lab has 66573 and 66574.
  EXPECT_EQ(*store_->max_numeric_identifier(), 66574);
  for (const char* id : {"01234", "bu-FD-J", "12a", "999", "1234567890123456789", "70001"})
    ASSERT_TRUE(store_->add_identifier(client(), {id, "unknown", std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt}));
  EXPECT_EQ(*store_->max_numeric_identifier(), 70001);
}

TEST_P(CatalogReadTest, MaxNumericIdentifierOfEmptyStoreIsZero) {
  TestDatabase empty(GetParam(), false);
  auto store = open_or_die(empty.url());
  ASSERT_TRUE(store);
  EXPECT_EQ(*store->max_numeric_identifier(), 0);
}

TEST_P(CatalogReadTest, CatalogRowReadsEditableColumns) {
  auto row = store_->catalog_row(CatalogTable::Sample, cat_.s1);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((**row).at("name"), CatalogValue{std::string("bt-1")});
  EXPECT_EQ((**row).at("project_uuid"), CatalogValue{cat_.p_ross1});
  EXPECT_EQ((**row).at("lat"), CatalogValue{});
  auto gone = store_->catalog_row(CatalogTable::Sample, Uuid::v7());
  ASSERT_TRUE(gone);
  EXPECT_FALSE(*gone);
  EXPECT_FALSE(store_->catalog_row(CatalogTable::Load, cat_.s1));
}

INSTANTIATE_TEST_SUITE_P(Engines, CatalogReadTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
