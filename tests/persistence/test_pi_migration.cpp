// Migration 0006: a principal investigator is a person or an organisation
// (docs/superpowers/specs/2026-10-13-pi-person-organisation-design.md,
// sections 4 and 11).

#include <gtest/gtest.h>

#include <map>

#include "catalog_fixture.hpp"
#include "migrate.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;
namespace pd = pychron::persistence::detail;

namespace {

CatalogValue text(const char* s) { return std::string(s); }

class PiMigration : public ::testing::TestWithParam<std::string> {};

class PiCatalog : public EntryTest {
 protected:
  std::vector<Refusal> refusals(const CatalogOutcome& o) {
    const auto* r = std::get_if<std::vector<Refusal>>(&o);
    EXPECT_TRUE(r) << describe(o);
    return r ? *r : std::vector<Refusal>{};
  }
};

// A raw connection as the store opens one: foreign keys on for SQLite.
std::unique_ptr<pd::Db> connect(const std::string& url) {
  auto db = pd::Db::open(StoreConfig{url, false});
  EXPECT_TRUE(db) << (db ? "" : to_string(db.error()));
  if (!db) return nullptr;
  if ((*db)->dialect() == Dialect::Sqlite) {
    EXPECT_TRUE((*db)->unprepared(QStringLiteral("PRAGMA foreign_keys = ON")));
  }
  return std::move(*db);
}

void insert(pd::Db& db, const char* table, pd::Row row) {
  row["created_utc"] = pd::qv(UtcTime::now());
  auto r = db.insert(QString::fromUtf8(table), row);
  EXPECT_TRUE(r) << table << ": " << (r ? "" : to_string(r.error()));
}

// What 0006 carries over unchanged: creation time, affiliation and email.
std::map<std::string, std::string> kept_columns(pd::Db& db) {
  std::map<std::string, std::string> out;
  auto rows = db.select(QStringLiteral("SELECT uuid, created_utc, affiliation, email FROM principal_investigator"));
  EXPECT_TRUE(rows) << (rows ? "" : to_string(rows.error()));
  if (!rows) return out;
  for (const auto& r : *rows)
    out[pd::to_std(r.value("uuid"))] = pd::to_std(r.value("created_utc")) + "|" + pd::to_std(r.value("affiliation")) +
                                       "|" + pd::to_std(r.value("email"));
  return out;
}

}  // namespace

// Three PIs and the projects and a repository that point at them, written
// by a build that knew migrations up to 0005, then the store opened by this
// one: every uuid kept, every project and repository still on its PI, the
// names moved to family and given name, each a person.
TEST_P(PiMigration, KeepsEveryPiAndEveryProjectsPi) {
  TestDatabase tdb(GetParam(), true);
  const Uuid ross = Uuid::v7(), smith = Uuid::v7(), lab = Uuid::v7();
  const Uuid alpha = Uuid::v7(), beta = Uuid::v7(), gamma = Uuid::v7(), repo = Uuid::v7();
  std::map<std::string, std::string> before;  // uuid -> created_utc|affiliation|email, as 0005 holds them
  {
    auto db = connect(tdb.url());
    ASSERT_TRUE(db);
    auto migrated = pd::migrate(*db, true, 5);
    ASSERT_TRUE(migrated) << to_string(migrated.error());
    ASSERT_EQ(migrated->size(), 5u);
    insert(*db, "principal_investigator",
           {{"uuid", pd::qv(ross)}, {"last_name", pd::qv("Ross")}, {"first_initial", pd::qv("J")}, {"affiliation", pd::qv("NMT")}});
    insert(*db, "principal_investigator", {{"uuid", pd::qv(smith)}, {"last_name", pd::qv("Smith")}, {"first_initial", pd::qv("")}, {"email", pd::qv("s@x")}});
    insert(*db, "principal_investigator", {{"uuid", pd::qv(lab)}, {"last_name", pd::qv("NMGRL")}, {"first_initial", pd::qv("")}});
    insert(*db, "project", {{"uuid", pd::qv(alpha)}, {"name", pd::qv("Alpha")}, {"pi_uuid", pd::qv(ross)}});
    insert(*db, "project", {{"uuid", pd::qv(beta)}, {"name", pd::qv("Beta")}, {"pi_uuid", pd::qv(smith)}});
    insert(*db, "project", {{"uuid", pd::qv(gamma)}, {"name", pd::qv("Gamma")}, {"pi_uuid", pd::qv(lab)}});
    insert(*db, "repository", {{"uuid", pd::qv(repo)}, {"name", pd::qv("Repo")}, {"pi_uuid", pd::qv(ross)}});
    before = kept_columns(*db);
    ASSERT_EQ(before.size(), 3u);
  }

  auto store = open_or_die(tdb.url());
  ASSERT_TRUE(store);
  auto pis = store->principal_investigators();
  ASSERT_TRUE(pis) << to_string(pis.error());
  ASSERT_EQ(pis->size(), 3u);
  std::map<Uuid, PrincipalInvestigatorRow> by_uuid;
  for (const auto& p : *pis) by_uuid[p.uuid] = p;
  ASSERT_EQ(by_uuid.size(), 3u);
  const auto& r = by_uuid.at(ross);
  EXPECT_EQ(r.kind, PiKind::Person);
  EXPECT_EQ(r.family_name, "Ross");
  EXPECT_EQ(r.given_name, "J");
  EXPECT_EQ(r.display_name, "J Ross");
  EXPECT_EQ(r.affiliation, std::optional<std::string>{"NMT"});
  EXPECT_EQ(r.orcid, std::nullopt);
  EXPECT_EQ(r.ror, std::nullopt);
  EXPECT_EQ(by_uuid.at(smith).display_name, "Smith");
  EXPECT_EQ(by_uuid.at(smith).given_name, "");
  EXPECT_EQ(by_uuid.at(lab).kind, PiKind::Person);  // nothing is guessed (spec 6.1)
  // Sorted by family name, then given name.
  EXPECT_EQ((*pis)[0].family_name, "NMGRL");
  EXPECT_EQ((*pis)[1].family_name, "Ross");
  EXPECT_EQ((*pis)[2].family_name, "Smith");

  auto projects = store->projects(std::nullopt);
  ASSERT_TRUE(projects);
  std::map<Uuid, ProjectRow> project;
  for (const auto& p : *projects) project[p.uuid] = p;
  EXPECT_EQ(project.at(alpha).principal_investigator, std::optional<Uuid>{ross});
  EXPECT_EQ(project.at(alpha).principal_investigator_name, "J Ross");
  EXPECT_EQ(project.at(beta).principal_investigator, std::optional<Uuid>{smith});
  EXPECT_EQ(project.at(gamma).principal_investigator, std::optional<Uuid>{lab});

  auto db = connect(tdb.url());
  ASSERT_TRUE(db);
  EXPECT_EQ(kept_columns(*db), before);
  auto rp = db->select_one(QStringLiteral("SELECT pi_uuid FROM repository WHERE uuid = ?"), {pd::qv(repo)});
  ASSERT_TRUE(rp && *rp);
  EXPECT_EQ(pd::to_uuid((**rp).value("pi_uuid")), ross);

  // The organisation the lab marks it to be is shown by its name.
  CatalogEditBatch mark;
  mark.edits = {CatalogUpdate{CatalogTable::PrincipalInvestigator, lab, {{"kind", text("person")}},
                              {{"kind", text("organisation")}}}};
  auto client = store->register_client({"entry-1", "reduction", std::nullopt, "test"});
  ASSERT_TRUE(client) << to_string(client.error());
  auto out = store->apply_catalog_edits(*client, mark);
  ASSERT_TRUE(out) << to_string(out.error());
  ASSERT_TRUE(applied(*out)) << describe(*out);
  pis = store->principal_investigators();
  ASSERT_TRUE(pis);
  for (const auto& p : *pis)
    if (p.uuid == lab) {
      EXPECT_EQ(p.kind, PiKind::Organisation);
      EXPECT_EQ(p.display_name, "NMGRL");
    }

  // The references still hold: a project naming no PI row is refused.
  insert(*db, "principal_investigator", {{"uuid", pd::qv(Uuid::v7())}, {"family_name", pd::qv("Jones")}});
  pd::Row orphan{{"uuid", pd::qv(Uuid::v7())},
                 {"name", pd::qv("Orphan")},
                 {"pi_uuid", pd::qv(Uuid::v7())},
                 {"created_utc", pd::qv(UtcTime::now())}};
  EXPECT_FALSE(db->insert(QStringLiteral("project"), orphan));
}

TEST_P(PiCatalog, NamesakesAreAllowed) {
  const Uuid a = Uuid::v7(), b = Uuid::v7();
  CatalogEditBatch batch;
  batch.edits = {
      CatalogInsert{CatalogTable::PrincipalInvestigator, a, {{"family_name", text("Ross")}, {"given_name", text("Jake")}}},
      CatalogInsert{CatalogTable::PrincipalInvestigator, b, {{"family_name", text("Ross")}, {"given_name", text("Jake")}}},
  };
  const auto out = apply(batch);
  ASSERT_TRUE(applied(out)) << describe(out);
  const auto pis = store_->principal_investigators();
  ASSERT_TRUE(pis);
  int jakes = 0;
  for (const auto& p : *pis) jakes += p.display_name == "Jake Ross" ? 1 : 0;
  EXPECT_EQ(jakes, 2);
}

TEST_P(PiCatalog, AnOrcidIsUniqueWhenPresent) {
  CatalogEditBatch without;
  without.edits = {
      CatalogInsert{CatalogTable::PrincipalInvestigator, Uuid::v7(), {{"family_name", text("Ruiz")}}},
      CatalogInsert{CatalogTable::PrincipalInvestigator, Uuid::v7(), {{"family_name", text("Ruiz")}}},
      CatalogInsert{CatalogTable::PrincipalInvestigator, Uuid::v7(),
                    {{"family_name", text("Ruiz")}, {"orcid", text("0000-0002-1825-0097")}}},
  };
  const auto first = apply(without);
  ASSERT_TRUE(applied(first)) << describe(first);

  const Uuid twin = Uuid::v7();
  CatalogEditBatch same;
  same.edits = {CatalogInsert{CatalogTable::PrincipalInvestigator, twin,
                              {{"family_name", text("Ruiz García")}, {"orcid", text("0000-0002-1825-0097")}}}};
  const auto r = refusals(apply(same));
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].uuid, twin);
  EXPECT_FALSE(*store_->catalog_row(CatalogTable::PrincipalInvestigator, twin));
}

TEST_P(PiCatalog, AnOrganisationHasNoOrcidOrGivenName) {
  CatalogEditBatch org;
  const Uuid nmgrl = Uuid::v7();
  org.edits = {CatalogInsert{CatalogTable::PrincipalInvestigator, nmgrl,
                             {{"kind", text("organisation")}, {"family_name", text("NMGRL")}, {"ror", text("005p9kw61")}}}};
  const auto ok = apply(org);
  ASSERT_TRUE(applied(ok)) << describe(ok);
  const auto row = store_->catalog_row(CatalogTable::PrincipalInvestigator, nmgrl);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((**row).at("kind"), text("organisation"));
  EXPECT_EQ((**row).at("ror"), text("005p9kw61"));

  for (const auto& extra : {std::pair{"given_name", "J"}, std::pair{"orcid", "0000-0002-1825-0097"}}) {
    CatalogEditBatch bad;
    bad.edits = {CatalogInsert{CatalogTable::PrincipalInvestigator, Uuid::v7(),
                               {{"kind", text("organisation")}, {"family_name", text("Lab")}, {extra.first, text(extra.second)}}}};
    EXPECT_EQ(refusals(apply(bad)).size(), 1u) << extra.first;
  }
  // A kind outside the two, and an empty identifier, are the caller's mistakes.
  for (const auto& [column, value] : {std::pair{"kind", "group"}, std::pair{"orcid", ""}, std::pair{"ror", ""}}) {
    CatalogEditBatch odd;
    odd.edits = {CatalogInsert{CatalogTable::PrincipalInvestigator, Uuid::v7(),
                               {{"family_name", text("Lab")}, {column, text(value)}}}};
    const auto out = store_->apply_catalog_edits(client(), odd);
    ASSERT_FALSE(out) << column;
    EXPECT_EQ(out.error().kind, ErrorKind::Protocol) << column;
  }
}

INSTANTIATE_TEST_SUITE_P(Engines, PiMigration, ::testing::ValuesIn(engines()), [](const auto& p) { return p.param; });
INSTANTIATE_TEST_SUITE_P(Engines, PiCatalog, ::testing::ValuesIn(engines()), [](const auto& p) { return p.param; });
