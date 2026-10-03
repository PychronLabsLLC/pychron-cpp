// Catalog writes are ensure-by-natural-key so an importer can re-run over the
// same legacy catalog: the second call returns the first call's uuid and
// writes nothing.

#include <gtest/gtest.h>

#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;
namespace pd = pychron::persistence::detail;
using pd::Row;

namespace {

class CatalogImportTest : public StoreTest {
 protected:
  Uuid client() const { return lab_.acquisition_client; }

  // Runs `write` twice: the same uuid comes back and the change cursor stays put.
  template <class F>
  Uuid expect_ensure(F&& write) {
    auto first = write();
    EXPECT_TRUE(first) << (first ? "" : to_string(first.error()));
    if (!first) return Uuid{};
    const ChangeSeq after_first = *store_->latest_change_seq();
    auto second = write();
    EXPECT_TRUE(second) << (second ? "" : to_string(second.error()));
    if (!second) return *first;
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(*store_->latest_change_seq(), after_first) << "the second call must not write";
    return *first;
  }

  Uuid project() { return *store_->add_project(client(), {.name = "Alpha"}); }
  Uuid material() { return *store_->add_material(client(), {.name = "sanidine"}); }

  // A second connection reads a row back raw (the fixture database is in memory,
  // so these tests build a file-backed one).
  static Row raw_row(const TestDatabase& database, const char* table, Uuid uuid) {
    auto db = pd::Db::open(StoreConfig{database.url(), false});
    EXPECT_TRUE(db);
    if (!db) return {};
    auto row = (*db)->select_one(QStringLiteral("SELECT * FROM %1 WHERE uuid = ?").arg(QString::fromUtf8(table)),
                                 {pd::qv(uuid)});
    EXPECT_TRUE(row && *row);
    return row && *row ? **row : Row{};
  }
};

}  // namespace

TEST_P(CatalogImportTest, EnsureUserTwice) {
  expect_ensure([&] { return store_->ensure_user(client(), "newuser"); });
}

TEST_P(CatalogImportTest, AddMassSpectrometerTwice) {
  expect_ensure([&] { return store_->add_mass_spectrometer(client(), {.name = "obama", .kind = "argus", .code = "o"}); });
}

TEST_P(CatalogImportTest, AddExtractDeviceTwice) {
  expect_ensure([&] { return store_->add_extract_device(client(), "Fusions CO2"); });
}

TEST_P(CatalogImportTest, AddPrincipalInvestigatorTwice) {
  expect_ensure([&] { return store_->add_principal_investigator(client(), {.last_name = "Smith", .first_initial = "J", .affiliation = "NMT"}); });
}

TEST_P(CatalogImportTest, AddProjectTwice) {
  expect_ensure([&] { return store_->add_project(client(), {.name = "Alpha"}); });
}

TEST_P(CatalogImportTest, AddProjectWithPiTwice) {
  const Uuid pi = *store_->add_principal_investigator(client(), {.last_name = "Smith", .first_initial = "J"});
  expect_ensure([&] { return store_->add_project(client(), {.name = "Alpha", .principal_investigator = pi}); });
}

TEST_P(CatalogImportTest, ProjectNameUnderDifferentPiIsADifferentProject) {
  const Uuid pi = *store_->add_principal_investigator(client(), {.last_name = "Smith", .first_initial = "J"});
  EXPECT_NE(*store_->add_project(client(), {.name = "Alpha", .principal_investigator = pi}), *store_->add_project(client(), {.name = "Alpha"}));
}

TEST_P(CatalogImportTest, AddMaterialTwice) {
  expect_ensure([&] { return store_->add_material(client(), {.name = "sanidine", .grainsize = "180-250"}); });
}

TEST_P(CatalogImportTest, AddSampleTwice) {
  const Uuid p = project(), m = material();
  expect_ensure([&] { return store_->add_sample(client(), {.name = "FC-1", .project = p, .material = m}); });
}

TEST_P(CatalogImportTest, AddIrradiationTwice) {
  expect_ensure([&] { return store_->add_irradiation(client(), "NM-301"); });
}

TEST_P(CatalogImportTest, AddLevelTwice) {
  expect_ensure([&] { return store_->add_level(client(), {.irradiation = lab_.irradiation, .name = "B", .z = 1.0}); });
}

TEST_P(CatalogImportTest, AddIrradiationPositionTwice) {
  expect_ensure([&] { return store_->add_irradiation_position(client(), {.level = lab_.level, .position = 2, .weight = 3.5}); });
}

TEST_P(CatalogImportTest, AddIdentifierTwice) {
  expect_ensure([&] { return store_->add_identifier(client(), {.identifier = "70001"}); });
}

TEST_P(CatalogImportTest, AddRefObjectTwice) {
  expect_ensure([&] { return store_->add_ref_object(client(), {.type = RefType::LoadHolder, .key = "24-Pit"}); });
}

TEST_P(CatalogImportTest, AddRepositoryTwice) {
  expect_ensure([&] { return store_->add_repository(client(), "repo-1"); });
}

TEST_P(CatalogImportTest, AddLoadTwice) {
  expect_ensure([&] { return store_->add_load(client(), {.name = "load-1"}); });
}

TEST_P(CatalogImportTest, AddLoadPositionTwice) {
  const Uuid load = *store_->add_load(client(), {.name = "load-1"});
  LoadPositionSpec spec;
  spec.load = load;
  spec.position = 3;
  spec.identifier = lab_.identifier;
  spec.weight = 1.5;
  ASSERT_TRUE(store_->add_load_position(client(), spec));
  const ChangeSeq after_first = *store_->latest_change_seq();
  auto second = store_->add_load_position(client(), spec);
  ASSERT_TRUE(second) << to_string(second.error());
  EXPECT_EQ(*store_->latest_change_seq(), after_first);
  // The same position holding another identifier is another row.
  spec.identifier = lab_.identifier2;
  ASSERT_TRUE(store_->add_load_position(client(), spec));
  EXPECT_GT(*store_->latest_change_seq(), after_first);
}

TEST_P(CatalogImportTest, ExistingRowWinsOverADifferingSpec) {
  const Uuid p = project(), m = material();
  SampleSpec first{.name = "FC-1", .project = p, .material = m};
  first.note = "original";
  const Uuid id = *store_->add_sample(client(), first);
  const ChangeSeq seq = *store_->latest_change_seq();
  SampleSpec again{.name = "FC-1", .project = p, .material = m};
  again.note = "changed";
  again.igsn = "IGSN1";
  again.uuid = Uuid::v7();
  EXPECT_EQ(*store_->add_sample(client(), again), id);
  EXPECT_EQ(*store_->latest_change_seq(), seq);
}

TEST_P(CatalogImportTest, CreatedRowUsesSuppliedUuid) {
  const Uuid a = Uuid::v7(), b = Uuid::v7(), c = Uuid::v7(), d = Uuid::v7(), e = Uuid::v7(), f = Uuid::v7(),
             g = Uuid::v7(), h = Uuid::v7(), i = Uuid::v7(), j = Uuid::v7();
  EXPECT_EQ(*store_->add_mass_spectrometer(client(), {.name = "ms-x", .uuid = a}), a);
  const Uuid pi = *store_->add_principal_investigator(client(), {.last_name = "Jones", .first_initial = "K", .uuid = b});
  EXPECT_EQ(pi, b);
  const Uuid proj = *store_->add_project(client(), {.name = "P", .principal_investigator = pi, .uuid = c});
  EXPECT_EQ(proj, c);
  const Uuid mat = *store_->add_material(client(), {.name = "biotite", .uuid = d});
  EXPECT_EQ(mat, d);
  SampleSpec sample_spec{.name = "S", .project = proj, .material = mat};
  sample_spec.uuid = e;
  EXPECT_EQ(*store_->add_sample(client(), sample_spec), e);
  LevelSpec level_spec{.irradiation = lab_.irradiation, .name = "C", .uuid = f};
  EXPECT_EQ(*store_->add_level(client(), level_spec), f);
  IdentifierSpec id_spec{.identifier = "70002"};
  id_spec.uuid = g;
  EXPECT_EQ(*store_->add_identifier(client(), id_spec), g);
  RefObjectSpec ref_spec{.type = RefType::LoadHolder, .key = "holder"};
  ref_spec.uuid = h;
  EXPECT_EQ(*store_->add_ref_object(client(), ref_spec), h);
  PositionSpec pos_spec{.level = lab_.level, .position = 9};
  pos_spec.uuid = j;
  EXPECT_EQ(*store_->add_irradiation_position(client(), pos_spec), j);
  LoadSpec load_spec{.name = "L"};
  load_spec.uuid = i;
  EXPECT_EQ(*store_->add_load(client(), load_spec), i);
}

TEST_P(CatalogImportTest, SampleKeepsEveryColumn) {
  TestDatabase shared(GetParam(), true);
  auto store = open_or_die(shared.url());
  ASSERT_TRUE(store);
  const Lab lab = seed_lab(*store);
  SampleSpec spec{.name = "FC-2",
                  .project = *store->add_project(lab.acquisition_client, {.name = "Alpha"}),
                  .material = *store->add_material(lab.acquisition_client, {.name = "sanidine"})};
  spec.note = "n";
  spec.igsn = "IGSN2";
  spec.lat = 34.5;
  spec.lon = -106.5;
  spec.elevation = 1500.5;
  spec.storage_location = "shelf 3";
  spec.location = "Fish Canyon";
  spec.unit = "Tuff";
  spec.lithology = "ignimbrite";
  spec.lithology_class = "volcanic";
  spec.lithology_type = "pyroclastic";
  spec.lithology_group = "Fish Canyon";
  spec.approximate_age = 28.2;
  const Uuid id = *store->add_sample(lab.acquisition_client, spec);
  const Row r = raw_row(shared, "sample", id);
  EXPECT_EQ(pd::to_std(r.value("note")), "n");
  EXPECT_EQ(pd::to_std(r.value("igsn")), "IGSN2");
  EXPECT_DOUBLE_EQ(r.value("lat").toDouble(), 34.5);
  EXPECT_DOUBLE_EQ(r.value("lon").toDouble(), -106.5);
  EXPECT_DOUBLE_EQ(r.value("elevation").toDouble(), 1500.5);
  EXPECT_EQ(pd::to_std(r.value("storage_location")), "shelf 3");
  EXPECT_EQ(pd::to_std(r.value("location")), "Fish Canyon");
  EXPECT_EQ(pd::to_std(r.value("unit")), "Tuff");
  EXPECT_EQ(pd::to_std(r.value("lithology")), "ignimbrite");
  EXPECT_EQ(pd::to_std(r.value("lithology_class")), "volcanic");
  EXPECT_EQ(pd::to_std(r.value("lithology_type")), "pyroclastic");
  EXPECT_EQ(pd::to_std(r.value("lithology_group")), "Fish Canyon");
  EXPECT_DOUBLE_EQ(r.value("approximate_age").toDouble(), 28.2);
}

TEST_P(CatalogImportTest, LoadKeepsItsColumns) {
  TestDatabase shared(GetParam(), true);
  auto store = open_or_die(shared.url());
  ASSERT_TRUE(store);
  const Lab lab = seed_lab(*store);
  const Uuid holder = *store->add_ref_object(lab.acquisition_client, {.type = RefType::LoadHolder, .key = "24-Pit"});
  LoadSpec spec{.name = "load-2"};
  spec.holder = holder;
  spec.created_by_user = lab.analyst;
  spec.archived = true;
  spec.created = UtcTime::parse("2020-02-03T04:05:06Z").value();
  const Uuid id = *store->add_load(lab.acquisition_client, spec);
  const Row r = raw_row(shared, "load", id);
  EXPECT_EQ(pd::to_uuid(r.value("holder_ref_uuid")), holder);
  EXPECT_EQ(pd::to_uuid(r.value("created_by_user_uuid")), lab.analyst);
  EXPECT_TRUE(r.value("archived").toBool());
  EXPECT_EQ(pd::to_time(r.value("created_utc")), *spec.created);
}

INSTANTIATE_TEST_SUITE_P(Engines, CatalogImportTest, ::testing::ValuesIn(engines()),
                         [](const auto& info) { return info.param; });
