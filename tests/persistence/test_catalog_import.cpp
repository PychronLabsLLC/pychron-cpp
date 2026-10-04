// Catalog writes are ensure-by-natural-key so an importer can re-run over the
// same legacy catalog: the second call returns the first call's uuid and
// writes nothing. A row that exists keeps its values; what it lacks is filled
// by a later call that has it.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>

#include "sql/errors.hpp"
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

// A file-backed store and a second connection that reads rows back raw.
struct Shared {
  explicit Shared(const std::string& engine) : database(engine, true) {
    store = open_or_die(database.url());
    if (!store) return;
    lab = seed_lab(*store);
    auto opened = pd::Db::open(StoreConfig{database.url(), false});
    if (opened) db = std::move(*opened);
  }

  Uuid client() const { return lab.acquisition_client; }

  Row row(const char* table, Uuid uuid) {
    auto found = db->select_one(QStringLiteral("SELECT * FROM %1 WHERE uuid = ?").arg(QString::fromUtf8(table)),
                                {pd::qv(uuid)});
    EXPECT_TRUE(found && *found) << table;
    return found && *found ? **found : Row{};
  }

  // The field diffs of the updates recorded for `uuid`, oldest first.
  std::vector<QJsonObject> updates(Uuid uuid) {
    auto found = db->select(
        QStringLiteral("SELECT detail FROM change_entity WHERE entity_uuid = ? AND op = 'update' ORDER BY change_seq"),
        {pd::qv(uuid)});
    EXPECT_TRUE(found);
    std::vector<QJsonObject> out;
    if (!found) return out;
    for (const auto& r : *found) out.push_back(QJsonDocument::fromJson(r.value("detail").toString().toUtf8()).object());
    return out;
  }

  // A row made by `bare` lacks what `full` brings: `full` fills it under the
  // same uuid with one change-log entry, `full` again writes nothing, and
  // `differing` (other values for what is now there) changes nothing.
  // Returns the row's uuid.
  template <class Bare, class Full, class Differing>
  Uuid expect_fill(const char* table, Bare&& bare, Full&& full, Differing&& differing) {
    auto made = bare();
    EXPECT_TRUE(made) << (made ? "" : to_string(made.error()));
    if (!made) return Uuid{};
    const ChangeSeq before = *store->latest_change_seq();
    auto filled = full();
    EXPECT_TRUE(filled) << (filled ? "" : to_string(filled.error()));
    if (!filled) return *made;
    EXPECT_EQ(*filled, *made);
    auto page = store->changes_since(before, 10);
    EXPECT_TRUE(page);
    if (page) {
      EXPECT_EQ(page->entries.size(), 1u) << "a fill is one change";
      if (page->entries.size() == 1) {
        const auto& entry = page->entries.front();
        EXPECT_EQ(entry.kind, "catalog");
        EXPECT_EQ(entry.entities.size(), 1u);
        if (entry.entities.size() == 1) {
          EXPECT_EQ(entry.entities.front().entity_type, table);
          EXPECT_EQ(entry.entities.front().entity, *made);
          EXPECT_EQ(entry.entities.front().op, "update");
        }
      }
    }
    const ChangeSeq after = *store->latest_change_seq();
    auto again = full();
    EXPECT_TRUE(again) << (again ? "" : to_string(again.error()));
    if (again) {
      EXPECT_EQ(*again, *made);
    }
    EXPECT_EQ(*store->latest_change_seq(), after) << "nothing left to fill";
    auto other = differing();
    EXPECT_TRUE(other) << (other ? "" : to_string(other.error()));
    if (other) {
      EXPECT_EQ(*other, *made);
    }
    EXPECT_EQ(*store->latest_change_seq(), after) << "a stored value is kept";
    EXPECT_EQ(updates(*made).size(), 1u);
    return *made;
  }

  // Declared first so it is destroyed last: the connections point into it.
  TestDatabase database;
  std::unique_ptr<IStore> store;
  Lab lab;
  std::unique_ptr<pd::Db> db;
};

QJsonArray filled_with(const QJsonValue& value) { return QJsonArray{QJsonValue(QJsonValue::Null), value}; }

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

TEST_P(CatalogImportTest, AddMaterialWithEmptyGrainsizeTwice) {
  expect_ensure([&] { return store_->add_material(client(), {.name = "sanidine"}); });
}

TEST_P(CatalogImportTest, AddPrincipalInvestigatorWithEmptyInitialTwice) {
  expect_ensure([&] { return store_->add_principal_investigator(client(), {.last_name = "Smith"}); });
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

TEST_P(CatalogImportTest, AddUserTwice) {
  expect_ensure([&] { return store_->add_user(client(), {.name = "mheizler", .email = "m@nmt.edu"}); });
  // A user that exists wins, whoever made it.
  EXPECT_EQ(*store_->add_user(client(), {.name = "jross", .email = "j@nmt.edu"}), lab_.analyst);
}

TEST_P(CatalogImportTest, UserProjectSampleAndIrradiationKeepLegacyColumns) {
  TestDatabase shared(GetParam(), true);
  auto store = open_or_die(shared.url());
  ASSERT_TRUE(store);
  const Lab lab = seed_lab(*store);
  const Uuid c = lab.acquisition_client;

  const Uuid user = *store->add_user(c, {"mheizler", "m@nmt.edu", "NMT", "staff"});
  Row r = raw_row(shared, "app_user", user);
  EXPECT_EQ(pd::to_std(r.value("email")), "m@nmt.edu");
  EXPECT_EQ(pd::to_std(r.value("affiliation")), "NMT");
  EXPECT_EQ(pd::to_std(r.value("category")), "staff");

  ProjectSpec project{.name = "Alpha"};
  project.checkin_date = "2016-02-29";
  project.comment = "two crates";
  project.lab_contact = "mheizler";
  project.institution = "NMT";
  const Uuid p = *store->add_project(c, project);
  r = raw_row(shared, "project", p);
  EXPECT_EQ(pd::to_std(r.value("checkin_date")).substr(0, 10), "2016-02-29");
  EXPECT_EQ(pd::to_std(r.value("comment")), "two crates");
  EXPECT_EQ(pd::to_std(r.value("lab_contact")), "mheizler");
  EXPECT_EQ(pd::to_std(r.value("institution")), "NMT");

  SampleSpec sample{.name = "FC-2", .project = p, .material = *store->add_material(c, {.name = "sanidine"})};
  sample.created = UtcTime::parse("2015-01-02T03:04:05Z").value();
  sample.updated = UtcTime::parse("2016-01-02T03:04:05Z").value();
  r = raw_row(shared, "sample", *store->add_sample(c, sample));
  EXPECT_EQ(pd::to_time(r.value("created_utc")), *sample.created);
  EXPECT_EQ(pd::to_time(r.value("updated_utc")), *sample.updated);
  // Without an update time a sample was last updated when it was created.
  sample.name = "FC-3";
  sample.updated.reset();
  r = raw_row(shared, "sample", *store->add_sample(c, sample));
  EXPECT_EQ(pd::to_time(r.value("updated_utc")), *sample.created);

  const UtcTime made = UtcTime::parse("2014-05-06T07:08:09Z").value();
  r = raw_row(shared, "irradiation", *store->add_irradiation(c, IrradiationSpec{"NM-301", made}));
  EXPECT_EQ(pd::to_time(r.value("created_utc")), made);
}

TEST_P(CatalogImportTest, ProjectCheckinDateMustBeADate) {
  for (const char* bad : {"2016-02-30", "2016-13-01", "16-02-03", "2016-02-03 10:00:00", "0000-00-00", ""}) {
    ProjectSpec spec{.name = std::string("P ") + bad};
    spec.checkin_date = bad;
    auto added = store_->add_project(client(), spec);
    ASSERT_FALSE(added) << bad;
    EXPECT_EQ(added.error().kind, ErrorKind::Protocol) << bad;
  }
}

// find_catalog_row is the read half of every ensure: it names the row an
// add_* would return and creates nothing.
TEST_P(CatalogImportTest, FindCatalogRowByNaturalKey) {
  using T = CatalogTable;
  const Uuid c = client();
  const ChangeSeq before_reads = *store_->latest_change_seq();
  const auto find = [&](T table, std::vector<CatalogKeyPart> key) {
    auto found = store_->find_catalog_row(table, key);
    EXPECT_TRUE(found) << (found ? "" : to_string(found.error()));
    return found ? *found : std::optional<Uuid>{};
  };
  // Nothing there yet, and asking creates nothing.
  EXPECT_FALSE(find(T::PrincipalInvestigator, {std::string("Smith"), std::string("J")}));
  EXPECT_FALSE(find(T::Project, {std::string("Alpha"), std::monostate{}}));
  EXPECT_EQ(*store_->latest_change_seq(), before_reads);

  const Uuid pi = *store_->add_principal_investigator(c, {.last_name = "Smith", .first_initial = "J"});
  const Uuid bare = *store_->add_project(c, {.name = "Alpha"});
  const Uuid owned = *store_->add_project(c, {.name = "Alpha", .principal_investigator = pi});
  const Uuid mat = *store_->add_material(c, {.name = "sanidine"});
  const Uuid samp = *store_->add_sample(c, {.name = "FC-2", .project = owned, .material = mat});
  const Uuid device = *store_->add_extract_device(c, "Fusions CO2");
  const Uuid user = *store_->ensure_user(c, "newuser");
  const Uuid repo = *store_->add_repository(c, "IR1010");
  const Uuid holder = *store_->add_ref_object(c, {.type = RefType::LoadHolder, .key = "221-hole"});
  LoadSpec tray;
  tray.name = "L-1";
  const Uuid load = *store_->add_load(c, tray);
  ASSERT_TRUE(store_->add_load_position(c, {load, 3, lab_.identifier, std::nullopt, std::nullopt, std::nullopt}));
  const ChangeSeq after_writes = *store_->latest_change_seq();

  EXPECT_EQ(find(T::PrincipalInvestigator, {std::string("Smith"), std::string("J")}), pi);
  // A project without a principal investigator is not the one with.
  EXPECT_EQ(find(T::Project, {std::string("Alpha"), std::monostate{}}), bare);
  EXPECT_EQ(find(T::Project, {std::string("Alpha"), pi}), owned);
  EXPECT_EQ(find(T::Material, {std::string("sanidine"), std::string("")}), mat);
  EXPECT_EQ(find(T::Sample, {std::string("FC-2"), owned, mat}), samp);
  EXPECT_FALSE(find(T::Sample, {std::string("FC-2"), bare, mat}));
  EXPECT_EQ(find(T::Irradiation, {std::string("NM-300")}), lab_.irradiation);
  EXPECT_EQ(find(T::Level, {lab_.irradiation, std::string("A")}), lab_.level);
  EXPECT_EQ(find(T::IrradiationPosition, {lab_.level, 1}), lab_.position);
  EXPECT_FALSE(find(T::IrradiationPosition, {lab_.level, 2}));
  EXPECT_EQ(find(T::User, {std::string("newuser")}), user);
  EXPECT_EQ(find(T::MassSpectrometer, {std::string("jan")}), lab_.mass_spectrometer);
  EXPECT_EQ(find(T::ExtractDevice, {std::string("Fusions CO2")}), device);
  EXPECT_EQ(find(T::Load, {std::string("L-1")}), load);
  EXPECT_TRUE(find(T::LoadPosition, {load, 3, lab_.identifier}));
  EXPECT_FALSE(find(T::LoadPosition, {load, 4, lab_.identifier}));
  EXPECT_EQ(find(T::Repository, {std::string("IR1010")}), repo);
  EXPECT_EQ(find(T::RefObject, {std::string("load_holder"), std::string("221-hole")}), holder);
  EXPECT_FALSE(find(T::RefObject, {std::string("irradiation_holder"), std::string("221-hole")}));
  EXPECT_EQ(*store_->latest_change_seq(), after_writes) << "a read must not write";

  // A key of the wrong shape is refused, not guessed at.
  auto wrong = store_->find_catalog_row(T::Level, {std::string("A")});
  ASSERT_FALSE(wrong);
  EXPECT_EQ(wrong.error().kind, ErrorKind::Protocol);
}

// ---------------------------------------------------------------- filling
// Ensuring a row that exists keeps what it has and fills what it lacks.

TEST_P(CatalogImportTest, PositionCreatedBareIsFilledByALaterSpec) {
  Shared s(GetParam());
  ASSERT_TRUE(s.store && s.db);
  const Uuid c = s.client();
  const Uuid proj = *s.store->add_project(c, {.name = "Alpha"});
  const Uuid mat = *s.store->add_material(c, {.name = "sanidine"});
  const Uuid sample = *s.store->add_sample(c, {.name = "FC-1", .project = proj, .material = mat});
  const Uuid other_sample = *s.store->add_sample(c, {.name = "FC-9", .project = proj, .material = mat});
  PositionSpec bare{.level = s.lab.level, .position = 7};
  PositionSpec full = bare;
  full.sample = sample;
  full.weight = 12.5;
  full.packet = "p7";
  full.note = "chipped";
  PositionSpec differing = bare;
  differing.sample = other_sample;
  differing.weight = 99.0;
  differing.packet = "other";
  differing.note = "other";
  const Uuid id = s.expect_fill(
      "irradiation_position", [&] { return s.store->add_irradiation_position(c, bare); },
      [&] { return s.store->add_irradiation_position(c, full); },
      [&] { return s.store->add_irradiation_position(c, differing); });
  const Row r = s.row("irradiation_position", id);
  EXPECT_EQ(pd::to_uuid(r.value("sample_uuid")), sample);
  EXPECT_DOUBLE_EQ(r.value("weight").toDouble(), 12.5);
  EXPECT_EQ(pd::to_std(r.value("packet")), "p7");
  EXPECT_EQ(pd::to_std(r.value("note")), "chipped");
  EXPECT_EQ(r.value("position").toInt(), 7);
  const auto updates = s.updates(id);
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates.front().keys(), (QStringList{"note", "packet", "sample_uuid", "weight"}));
  EXPECT_EQ(updates.front().value("packet").toArray(), filled_with("p7"));
  EXPECT_EQ(updates.front().value("weight").toArray(), filled_with(12.5));
  EXPECT_EQ(updates.front().value("sample_uuid").toArray(), filled_with(QString::fromStdString(sample.str())));
}

TEST_P(CatalogImportTest, FillTakesOnlyWhatIsMissing) {
  Shared s(GetParam());
  ASSERT_TRUE(s.store && s.db);
  const Uuid c = s.client();
  PositionSpec first{.level = s.lab.level, .position = 8};
  first.weight = 3.5;
  const Uuid id = *s.store->add_irradiation_position(c, first);
  PositionSpec second{.level = s.lab.level, .position = 8};
  second.weight = 4.5;
  second.packet = "p8";
  EXPECT_EQ(*s.store->add_irradiation_position(c, second), id);
  const Row r = s.row("irradiation_position", id);
  EXPECT_DOUBLE_EQ(r.value("weight").toDouble(), 3.5);
  EXPECT_EQ(pd::to_std(r.value("packet")), "p8");
  EXPECT_TRUE(r.value("note").isNull());
  const auto updates = s.updates(id);
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates.front().keys(), QStringList{"packet"});
}

TEST_P(CatalogImportTest, LevelCreatedBareIsFilledByALaterSpec) {
  Shared s(GetParam());
  ASSERT_TRUE(s.store && s.db);
  const Uuid c = s.client();
  const Uuid holder = *s.store->add_ref_object(c, {.type = RefType::IrradiationHolder, .key = "24-hole"});
  const Uuid other_holder = *s.store->add_ref_object(c, {.type = RefType::IrradiationHolder, .key = "48-hole"});
  LevelSpec bare{.irradiation = s.lab.irradiation, .name = "K"};
  LevelSpec full = bare;
  full.holder = holder;
  full.z = 0.5;
  full.note = "top";
  LevelSpec differing = bare;
  differing.holder = other_holder;
  differing.z = 9.0;
  differing.note = "other";
  const Uuid id = s.expect_fill(
      "level", [&] { return s.store->add_level(c, bare); }, [&] { return s.store->add_level(c, full); },
      [&] { return s.store->add_level(c, differing); });
  const Row r = s.row("level", id);
  EXPECT_EQ(pd::to_uuid(r.value("holder_ref_uuid")), holder);
  EXPECT_DOUBLE_EQ(r.value("z").toDouble(), 0.5);
  EXPECT_EQ(pd::to_std(r.value("note")), "top");
}

TEST_P(CatalogImportTest, ProjectCreatedBareIsFilledByALaterSpec) {
  Shared s(GetParam());
  ASSERT_TRUE(s.store && s.db);
  const Uuid c = s.client();
  ProjectSpec bare{.name = "Beta"};
  ProjectSpec full = bare;
  full.checkin_date = "2016-02-29";
  full.comment = "two crates";
  full.lab_contact = "mheizler";
  full.institution = "NMT";
  ProjectSpec differing = bare;
  differing.checkin_date = "2017-01-01";
  differing.comment = "other";
  differing.lab_contact = "other";
  differing.institution = "other";
  const Uuid id = s.expect_fill(
      "project", [&] { return s.store->add_project(c, bare); }, [&] { return s.store->add_project(c, full); },
      [&] { return s.store->add_project(c, differing); });
  const Row r = s.row("project", id);
  EXPECT_EQ(pd::to_std(r.value("checkin_date")).substr(0, 10), "2016-02-29");
  EXPECT_EQ(pd::to_std(r.value("comment")), "two crates");
  EXPECT_EQ(pd::to_std(r.value("lab_contact")), "mheizler");
  EXPECT_EQ(pd::to_std(r.value("institution")), "NMT");
  // The principal investigator is part of a project's key: it is not filled,
  // the project under one is another project.
  const Uuid pi = *s.store->add_principal_investigator(c, {.last_name = "Smith", .first_initial = "J"});
  EXPECT_NE(*s.store->add_project(c, {.name = "Beta", .principal_investigator = pi}), id);
  EXPECT_TRUE(s.row("project", id).value("pi_uuid").isNull());
}

TEST_P(CatalogImportTest, SampleCreatedBareIsFilledByALaterSpec) {
  Shared s(GetParam());
  ASSERT_TRUE(s.store && s.db);
  const Uuid c = s.client();
  const Uuid proj = *s.store->add_project(c, {.name = "Alpha"});
  const Uuid mat = *s.store->add_material(c, {.name = "sanidine"});
  SampleSpec bare{.name = "FC-2", .project = proj, .material = mat};
  bare.created = UtcTime::parse("2015-01-02T03:04:05Z").value();
  SampleSpec full = bare;
  full.note = "n";
  full.igsn = "IGSN2";
  full.lat = 34.5;
  full.lon = -106.5;
  full.elevation = 1500.5;
  full.storage_location = "shelf 3";
  full.location = "Fish Canyon";
  full.unit = "Tuff";
  full.lithology = "ignimbrite";
  full.lithology_class = "volcanic";
  full.lithology_type = "pyroclastic";
  full.lithology_group = "Fish Canyon";
  full.approximate_age = 28.2;
  full.created = UtcTime::parse("2019-01-02T03:04:05Z").value();
  SampleSpec differing = bare;
  differing.note = "other";
  differing.igsn = "IGSN9";
  differing.lat = 1.0;
  differing.approximate_age = 1.0;
  const Uuid id = s.expect_fill(
      "sample", [&] { return s.store->add_sample(c, bare); }, [&] { return s.store->add_sample(c, full); },
      [&] { return s.store->add_sample(c, differing); });
  const Row r = s.row("sample", id);
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
  // The times a row was made with are values it has.
  EXPECT_EQ(pd::to_time(r.value("created_utc")), *bare.created);
  EXPECT_EQ(pd::to_time(r.value("updated_utc")), *bare.created);
}

TEST_P(CatalogImportTest, IdentifierCreatedBareIsFilledByALaterSpec) {
  Shared s(GetParam());
  ASSERT_TRUE(s.store && s.db);
  const Uuid c = s.client();
  const Uuid position = *s.store->add_irradiation_position(c, {.level = s.lab.level, .position = 11});
  const Uuid other_position = *s.store->add_irradiation_position(c, {.level = s.lab.level, .position = 12});
  const Uuid proj = *s.store->add_project(c, {.name = "Alpha"});
  const Uuid mat = *s.store->add_material(c, {.name = "sanidine"});
  const Uuid sample = *s.store->add_sample(c, {.name = "FC-1", .project = proj, .material = mat});
  IdentifierSpec bare{.identifier = "70011"};
  IdentifierSpec full = bare;
  full.position = position;
  full.sample = sample;
  full.mass_spectrometer = s.lab.mass_spectrometer;
  IdentifierSpec differing = bare;
  differing.position = other_position;
  const Uuid id = s.expect_fill(
      "identifier", [&] { return s.store->add_identifier(c, bare); }, [&] { return s.store->add_identifier(c, full); },
      [&] { return s.store->add_identifier(c, differing); });
  const Row r = s.row("identifier", id);
  EXPECT_EQ(pd::to_uuid(r.value("position_uuid")), position);
  EXPECT_EQ(pd::to_uuid(r.value("sample_uuid")), sample);
  EXPECT_EQ(pd::to_uuid(r.value("mass_spectrometer_uuid")), s.lab.mass_spectrometer);
  EXPECT_EQ(pd::to_std(r.value("kind")), "unknown");
  EXPECT_EQ(*s.store->identifier_at("NM-300", "A", 11), std::optional<std::string>{"70011"});
}

// identifier.position_uuid is UNIQUE: filling it with a position another
// identifier holds fails as inserting it there would, and writes nothing.
TEST_P(CatalogImportTest, FillingAPositionAnotherIdentifierHoldsFailsAsAnInsertWould) {
  Shared s(GetParam());
  ASSERT_TRUE(s.store && s.db);
  const Uuid c = s.client();
  const Uuid position = *s.store->add_irradiation_position(c, {.level = s.lab.level, .position = 13});
  IdentifierSpec holder{.identifier = "70013"};
  holder.position = position;
  const Uuid held_by = *s.store->add_identifier(c, holder);
  const Uuid bare = *s.store->add_identifier(c, {.identifier = "70014"});
  const ChangeSeq before = *s.store->latest_change_seq();

  IdentifierSpec inserted{.identifier = "70015"};
  inserted.position = position;
  auto as_insert = s.store->add_identifier(c, inserted);
  ASSERT_FALSE(as_insert);
  IdentifierSpec filled{.identifier = "70014"};
  filled.position = position;
  filled.analysis_type = "unknown";
  auto as_fill = s.store->add_identifier(c, filled);
  ASSERT_FALSE(as_fill);
  EXPECT_EQ(as_fill.error().kind, as_insert.error().kind);
  // The two are told apart: the row a fill is refused for exists.
  EXPECT_TRUE(is_refused_catalog_fill(as_fill.error())) << as_fill.error().what;
  EXPECT_FALSE(is_refused_catalog_fill(as_insert.error())) << as_insert.error().what;
  EXPECT_NE(as_fill.error().what.find("identifier"), std::string::npos) << as_fill.error().what;

  EXPECT_EQ(*s.store->latest_change_seq(), before);
  const Row r = s.row("identifier", bare);
  EXPECT_TRUE(r.value("position_uuid").isNull());
  EXPECT_TRUE(r.value("analysis_type").isNull()) << "the fill is one statement: all of it or none";
  EXPECT_EQ(pd::to_uuid(s.row("identifier", held_by).value("position_uuid")), position);
  EXPECT_TRUE(s.updates(bare).empty());
  // The store is still usable.
  EXPECT_EQ(*s.store->add_identifier(c, {.identifier = "70014"}), bare);
}

// The other fills a constraint keeps out: a position for a special identifier
// (CHECK) and a spectrometer code another spectrometer has (UNIQUE).
TEST_P(CatalogImportTest, FillsAConstraintKeepsOutAreRefusedFills) {
  Shared s(GetParam());
  ASSERT_TRUE(s.store && s.db);
  const Uuid c = s.client();
  const Uuid position = *s.store->add_irradiation_position(c, {.level = s.lab.level, .position = 14});
  IdentifierSpec special{.identifier = "bu-XX"};
  special.kind = "special";
  special.analysis_type = "blank_unknown";
  const Uuid blank = *s.store->add_identifier(c, special);
  const ChangeSeq before = *s.store->latest_change_seq();

  IdentifierSpec placed = special;
  placed.position = position;
  auto at_a_position = s.store->add_identifier(c, placed);
  ASSERT_FALSE(at_a_position);
  EXPECT_TRUE(is_refused_catalog_fill(at_a_position.error())) << at_a_position.error().what;

  ASSERT_TRUE(s.store->add_mass_spectrometer(c, {"argus-1", "argus", "a1", std::nullopt}));
  const Uuid bare = *s.store->add_mass_spectrometer(c, {"argus-2", std::nullopt, std::nullopt, std::nullopt});
  const ChangeSeq made = *s.store->latest_change_seq();
  auto same_code = s.store->add_mass_spectrometer(c, {"argus-2", "argus", "a1", std::nullopt});
  ASSERT_FALSE(same_code);
  EXPECT_TRUE(is_refused_catalog_fill(same_code.error())) << same_code.error().what;
  EXPECT_EQ(*s.store->latest_change_seq(), made);
  EXPECT_TRUE(s.row("mass_spectrometer", bare).value("kind").isNull());
  EXPECT_TRUE(s.row("identifier", blank).value("position_uuid").isNull());
  EXPECT_GT(made, before);

  // The error is known by its code, not its words: context a caller adds
  // does not hide it, and the words alone do not make it one.
  Error with_context = same_code.error();
  with_context.what = "while importing: " + with_context.what;
  EXPECT_TRUE(is_refused_catalog_fill(with_context));
  EXPECT_FALSE(is_refused_catalog_fill(Error{ErrorKind::Protocol, same_code.error().what, "persistence"}));
  EXPECT_FALSE(is_refused_catalog_fill(Error{ErrorKind::Protocol, "unknown identifier 'x'", ""}));
}

// Only an integrity constraint refuses a fill. An UPDATE that fails for any
// other reason (no permission, a column that is gone, a trigger that raises,
// SQL that does not run) is an error like any other.
TEST_P(CatalogImportTest, FillThatFailsForAnotherReasonIsNotARefusedFill) {
  Shared s(GetParam());
  ASSERT_TRUE(s.store && s.db);
  const Uuid c = s.client();
  const Uuid bare = *s.store->add_mass_spectrometer(c, {"argus-3", std::nullopt, std::nullopt, std::nullopt});
  const ChangeSeq before = *s.store->latest_change_seq();
  auto broken = break_updates_of(*s.db, "mass_spectrometer");
  ASSERT_TRUE(broken) << broken.error().what;
  auto filled = s.store->add_mass_spectrometer(c, {"argus-3", "argus", "a3", std::nullopt});
  ASSERT_FALSE(filled);
  EXPECT_FALSE(is_refused_catalog_fill(filled.error())) << filled.error().what;
  EXPECT_EQ(*s.store->latest_change_seq(), before);
  EXPECT_TRUE(s.row("mass_spectrometer", bare).value("kind").isNull());
}

// The driver's code decides: SQLSTATE class 23, and SQLite's SQLITE_CONSTRAINT
// (19) with its extended codes.
TEST(SqlErrors, OnlyAnIntegrityConstraintViolationIsOne) {
  using pd::is_constraint_violation;
  for (const char* code : {"19", "2067", "1555", "787", "275", "1299", "1811", "3091"}) {
    EXPECT_TRUE(is_constraint_violation(Dialect::Sqlite, code)) << code;
  }
  for (const char* code : {"", "1", "20", "5", "6", "8", "x19", "19x", "23505"}) {
    EXPECT_FALSE(is_constraint_violation(Dialect::Sqlite, code)) << code;
  }
  for (const char* code : {"23505", "23514", "23503", "23502", "23P01", "23000"}) {
    EXPECT_TRUE(is_constraint_violation(Dialect::PostgreSql, code)) << code;
  }
  for (const char* code : {"", "42501", "42703", "42P01", "P0001", "22P02", "40001", "0A000", "19", "2350", "235050"}) {
    EXPECT_FALSE(is_constraint_violation(Dialect::PostgreSql, code)) << code;
  }
  // The error carries it; its kind is as before.
  const Error unique = pd::sql_error(Dialect::PostgreSql, "23505", false, "duplicate key");
  EXPECT_EQ(unique.kind, ErrorKind::Protocol);
  EXPECT_EQ(unique.code, pd::kCodeConstraint);
  const Error denied = pd::sql_error(Dialect::PostgreSql, "42501", false, "permission denied");
  EXPECT_EQ(denied.kind, ErrorKind::Protocol);
  EXPECT_TRUE(denied.code.empty());
  EXPECT_EQ(pd::sql_error(Dialect::Sqlite, "2067", false, "UNIQUE constraint failed").code, pd::kCodeConstraint);
  EXPECT_TRUE(pd::sql_error(Dialect::Sqlite, "1", false, "no such table").code.empty());
}

TEST_P(CatalogImportTest, UserLoadAndMassSpectrometerAreFilled) {
  Shared s(GetParam());
  ASSERT_TRUE(s.store && s.db);
  const Uuid c = s.client();
  const Uuid user = *s.store->ensure_user(c, "mheizler");
  EXPECT_EQ(*s.store->add_user(c, {"mheizler", "m@nmt.edu", "NMT", "staff"}), user);
  Row r = s.row("app_user", user);
  EXPECT_EQ(pd::to_std(r.value("email")), "m@nmt.edu");
  EXPECT_EQ(pd::to_std(r.value("affiliation")), "NMT");
  EXPECT_EQ(pd::to_std(r.value("category")), "staff");

  const Uuid holder = *s.store->add_ref_object(c, {.type = RefType::LoadHolder, .key = "24-Pit"});
  const Uuid load = *s.store->add_load(c, {.name = "load-3"});
  LoadSpec full{.name = "load-3"};
  full.holder = holder;
  full.created_by_user = user;
  full.archived = true;
  EXPECT_EQ(*s.store->add_load(c, full), load);
  r = s.row("load", load);
  EXPECT_EQ(pd::to_uuid(r.value("holder_ref_uuid")), holder);
  EXPECT_EQ(pd::to_uuid(r.value("created_by_user_uuid")), user);
  EXPECT_FALSE(r.value("archived").toBool()) << "archived is a value the load has";

  const Uuid ms = *s.store->add_mass_spectrometer(c, {.name = "obama"});
  EXPECT_EQ(*s.store->add_mass_spectrometer(c, {.name = "obama", .kind = "argus", .code = "o"}), ms);
  r = s.row("mass_spectrometer", ms);
  EXPECT_EQ(pd::to_std(r.value("kind")), "argus");
  EXPECT_EQ(pd::to_std(r.value("code")), "o");
  for (const Uuid id : {user, load, ms}) EXPECT_EQ(s.updates(id).size(), 1u);
}

INSTANTIATE_TEST_SUITE_P(Engines, CatalogImportTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
