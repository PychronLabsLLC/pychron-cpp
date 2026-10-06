// The catalog edit batch (sample and package entry spec, sections 5.2, 5.3).

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "catalog_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;
namespace pd = pychron::persistence::detail;

namespace {

class CatalogEditTest : public EntryTest {
 protected:
  CatalogFields row(CatalogTable t, Uuid u) {
    auto r = store_->catalog_row(t, u);
    EXPECT_TRUE(r && *r);
    return r && *r ? **r : CatalogFields{};
  }
  ChangeSeq seq() { return *store_->latest_change_seq(); }

  Uuid identify(Uuid position, const std::string& text) {
    auto id = store_->add_identifier(client(), {text, "unknown", std::nullopt, std::nullopt, position, std::nullopt, std::nullopt});
    EXPECT_TRUE(id);
    return id ? *id : Uuid{};
  }

  std::vector<Refusal> refusals(const CatalogOutcome& o) {
    const auto* r = std::get_if<std::vector<Refusal>>(&o);
    EXPECT_TRUE(r) << describe(o);
    return r ? *r : std::vector<Refusal>{};
  }
};

CatalogValue text(const char* s) { return std::string(s); }

}  // namespace

TEST_P(CatalogEditTest, InsertUpdateDeleteRoundTrip) {
  const Uuid pi = Uuid::v7(), project = Uuid::v7(), material = Uuid::v7(), sample = Uuid::v7();
  const Uuid pkg = Uuid::v7(), level = Uuid::v7(), position = Uuid::v7();
  CatalogEditBatch batch;
  batch.edits = {
      CatalogInsert{CatalogTable::PrincipalInvestigator, pi, {{"last_name", text("Doe")}, {"first_initial", text("A")}, {"email", text("a@b")}}},
      CatalogInsert{CatalogTable::Project, project, {{"name", text("Delta")}, {"pi_uuid", pi}, {"checkin_date", text("2026-10-04")}}},
      CatalogInsert{CatalogTable::Material, material, {{"name", text("plagioclase")}}},
      CatalogInsert{CatalogTable::Sample, sample,
                    {{"name", text("pl-1")}, {"project_uuid", project}, {"material_uuid", material}, {"lat", 34.5},
                     {"lon", std::int64_t{-106}}, {"approximate_age", 1.25}, {"igsn", text("IEXXX0001")}}},
      CatalogInsert{CatalogTable::Irradiation, pkg, {{"name", text("P-9")}, {"kind", text("package")}}},
      CatalogInsert{CatalogTable::Level, level, {{"irradiation_uuid", pkg}, {"name", text("A")}, {"note", text("top")}}},
      CatalogInsert{CatalogTable::IrradiationPosition, position,
                    {{"level_uuid", level}, {"position", std::int64_t{3}}, {"sample_uuid", sample}, {"weight", 2.0}, {"packet", text("P3")}}},
  };
  const auto out = apply(batch);
  ASSERT_TRUE(applied(out)) << describe(out);

  EXPECT_EQ(row(CatalogTable::PrincipalInvestigator, pi).at("email"), text("a@b"));
  EXPECT_EQ(row(CatalogTable::Project, project).at("checkin_date"), text("2026-10-04"));
  EXPECT_EQ(row(CatalogTable::Material, material).at("grainsize"), text(""));
  const CatalogFields s = row(CatalogTable::Sample, sample);
  EXPECT_EQ(s.at("lat"), CatalogValue{34.5});
  EXPECT_EQ(s.at("lon"), CatalogValue{-106.0});
  EXPECT_EQ(s.at("approximate_age"), CatalogValue{1.25});
  EXPECT_EQ(row(CatalogTable::Irradiation, pkg).at("kind"), text("package"));
  EXPECT_EQ(row(CatalogTable::Level, level).at("note"), text("top"));
  EXPECT_EQ(row(CatalogTable::IrradiationPosition, position).at("position"), CatalogValue{std::int64_t{3}});

  // Update every kind of column, with the values just read as expected.
  CatalogEditBatch update;
  update.edits = {
      CatalogUpdate{CatalogTable::Sample, sample, {{"lat", 34.5}, {"note", CatalogValue{}}}, {{"lat", 35.25}, {"note", text("n")}}},
      CatalogUpdate{CatalogTable::IrradiationPosition, position, {{"weight", 2.0}}, {{"weight", CatalogValue{}}, {"packet", text("P4")}}},
      CatalogUpdate{CatalogTable::Irradiation, pkg, {{"kind", text("package")}}, {{"kind", text("irradiation")}}},
  };
  ASSERT_TRUE(applied(apply(update)));
  EXPECT_EQ(row(CatalogTable::Sample, sample).at("lat"), CatalogValue{35.25});
  EXPECT_EQ(row(CatalogTable::IrradiationPosition, position).at("weight"), CatalogValue{});
  EXPECT_EQ(row(CatalogTable::Irradiation, pkg).at("kind"), text("irradiation"));

  // Delete in reverse dependency order.
  CatalogEditBatch remove;
  remove.edits = {
      CatalogDelete{CatalogTable::IrradiationPosition, position, {}},
      CatalogDelete{CatalogTable::Level, level, {}},
      CatalogDelete{CatalogTable::Irradiation, pkg, {}},
      CatalogDelete{CatalogTable::Sample, sample, {{"name", text("pl-1")}}},
      CatalogDelete{CatalogTable::Project, project, {}},
      CatalogDelete{CatalogTable::Material, material, {}},
      CatalogDelete{CatalogTable::PrincipalInvestigator, pi, {}},
  };
  const auto removed = apply(remove);
  ASSERT_TRUE(applied(removed)) << describe(removed);
  EXPECT_FALSE(*store_->catalog_row(CatalogTable::Sample, sample));
  EXPECT_FALSE(*store_->catalog_row(CatalogTable::PrincipalInvestigator, pi));
}

TEST_P(CatalogEditTest, LatitudeAndLongitudeAreOneGeometryColumn) {
  const Uuid sample = Uuid::v7();
  CatalogEditBatch insert;
  insert.edits = {CatalogInsert{CatalogTable::Sample, sample,
                                {{"name", text("geo-1")}, {"project_uuid", cat_.p_ross1}, {"material_uuid", cat_.biotite},
                                 {"lat", 34.0722}, {"lon", -106.905}}}};
  ASSERT_TRUE(applied(apply(insert)));
  CatalogFields s = row(CatalogTable::Sample, sample);
  EXPECT_EQ(s.at("lat"), CatalogValue{34.0722});
  EXPECT_EQ(s.at("lon"), CatalogValue{-106.905});

  // Changing one half keeps the other, and the other half still checks as expected.
  CatalogEditBatch half;
  half.edits = {CatalogUpdate{CatalogTable::Sample, sample, {{"lon", -106.905}}, {{"lat", 35.0}}}};
  ASSERT_TRUE(applied(apply(half)));
  s = row(CatalogTable::Sample, sample);
  EXPECT_EQ(s.at("lat"), CatalogValue{35.0});
  EXPECT_EQ(s.at("lon"), CatalogValue{-106.905});
  SampleQuery q;
  q.text = "geo-1";
  auto listed = store_->samples(q);
  ASSERT_TRUE(listed && listed->size() == 1);
  EXPECT_EQ(listed->front().fields.lat, 35.0);
  EXPECT_EQ(listed->front().fields.lon, -106.905);

  // Half a point and a coordinate off the globe are errors, not edits.
  CatalogEditBatch lone;
  lone.edits = {CatalogInsert{CatalogTable::Sample, Uuid::v7(),
                              {{"name", text("geo-2")}, {"project_uuid", cat_.p_ross1}, {"material_uuid", cat_.biotite},
                               {"lat", 1.0}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), lone));
  CatalogEditBatch off;
  off.edits = {CatalogUpdate{CatalogTable::Sample, sample, {}, {{"lat", 95.0}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), off));
  CatalogEditBatch far;
  far.edits = {CatalogUpdate{CatalogTable::Sample, sample, {}, {{"lon", -181.0}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), far));
  CatalogEditBatch drop_one;
  drop_one.edits = {CatalogUpdate{CatalogTable::Sample, sample, {}, {{"lon", CatalogValue{}}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), drop_one));
  EXPECT_EQ(row(CatalogTable::Sample, sample).at("lat"), CatalogValue{35.0});
  EXPECT_FALSE(store_->add_sample(client(), {.name = "geo-3", .project = cat_.p_ross1, .material = cat_.biotite, .lat = 1.0}));

  // Both to null clears the point.
  CatalogEditBatch clear;
  clear.edits = {CatalogUpdate{CatalogTable::Sample, sample, {{"lat", 35.0}}, {{"lat", CatalogValue{}}, {"lon", CatalogValue{}}}}};
  ASSERT_TRUE(applied(apply(clear)));
  s = row(CatalogTable::Sample, sample);
  EXPECT_EQ(s.at("lat"), CatalogValue{});
  EXPECT_EQ(s.at("lon"), CatalogValue{});
}

TEST_P(CatalogEditTest, UnknownColumnWrongTypeAndMissingRequiredAreErrors) {
  CatalogEditBatch a;
  a.edits = {CatalogUpdate{CatalogTable::Sample, cat_.s1, {}, {{"created_utc", text("x")}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), a));
  CatalogEditBatch b;
  b.edits = {CatalogUpdate{CatalogTable::Sample, cat_.s1, {}, {{"lat", text("north")}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), b));
  CatalogEditBatch c;
  c.edits = {CatalogInsert{CatalogTable::Sample, Uuid::v7(), {{"name", text("x")}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), c));
  CatalogEditBatch d;
  d.edits = {CatalogUpdate{CatalogTable::Sample, cat_.s1, {}, {{"name", CatalogValue{}}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), d));
  CatalogEditBatch e;
  e.edits = {CatalogUpdate{CatalogTable::Irradiation, cat_.nm301, {}, {{"kind", text("argon")}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), e));
  CatalogEditBatch f;
  f.edits = {CatalogInsert{CatalogTable::Identifier, Uuid::v7(), {{"identifier", text("1")}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), f));
  CatalogEditBatch g;
  g.edits = {CatalogUpdate{CatalogTable::Load, cat_.s1, {}, {}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), g));
  CatalogEditBatch h;
  h.edits = {CatalogUpdate{CatalogTable::Project, cat_.p_ross1, {}, {{"checkin_date", text("2026-13-01")}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), h));
}

TEST_P(CatalogEditTest, RefObjectsAreInsertedOnly) {
  const Uuid geom = Uuid::v7();
  CatalogEditBatch batch;
  batch.edits = {CatalogInsert{CatalogTable::RefObject, geom,
                               {{"ref_type", text("level_geometry")}, {"key", text("NM-301/A")},
                                {"irradiation_uuid", cat_.nm301}, {"level_uuid", cat_.level_a}}}};
  ASSERT_TRUE(applied(apply(batch)));
  EXPECT_EQ(*store_->find_catalog_row(CatalogTable::RefObject, {std::string("level_geometry"), std::string("NM-301/A")}), geom);
  CatalogEditBatch again;
  again.edits = {CatalogInsert{CatalogTable::RefObject, Uuid::v7(), {{"ref_type", text("level_geometry")}, {"key", text("NM-301/A")}}}};
  EXPECT_EQ(refusals(apply(again)).front().rule, "unique");
  CatalogEditBatch flux;
  flux.edits = {CatalogInsert{CatalogTable::RefObject, Uuid::v7(), {{"ref_type", text("flux_position")}, {"key", text("x")}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), flux));
  CatalogEditBatch update;
  update.edits = {CatalogUpdate{CatalogTable::RefObject, geom, {}, {{"key", text("y")}}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), update));
  CatalogEditBatch remove;
  remove.edits = {CatalogDelete{CatalogTable::RefObject, geom, {}}};
  EXPECT_FALSE(store_->apply_catalog_edits(client(), remove));
}

TEST_P(CatalogEditTest, StaleWhenExpectedDiffers) {
  CatalogEditBatch batch;
  batch.edits = {CatalogUpdate{CatalogTable::Sample, cat_.s1, {{"name", text("bt-9")}, {"note", CatalogValue{}}},
                               {{"note", text("mine")}}}};
  const auto out = apply(batch);
  const auto* stale = std::get_if<std::vector<StaleRow>>(&out);
  ASSERT_TRUE(stale) << describe(out);
  ASSERT_EQ(stale->size(), 1u);
  EXPECT_EQ(stale->front().uuid, cat_.s1);
  EXPECT_EQ(stale->front().actual.at("name"), text("bt-1"));
  EXPECT_EQ(stale->front().actual.at("note"), CatalogValue{});
  EXPECT_EQ(row(CatalogTable::Sample, cat_.s1).at("note"), CatalogValue{});

  CatalogEditBatch gone;
  const Uuid missing = Uuid::v7();
  gone.edits = {CatalogUpdate{CatalogTable::Sample, missing, {}, {{"note", text("x")}}}};
  const auto out2 = apply(gone);
  const auto* stale2 = std::get_if<std::vector<StaleRow>>(&out2);
  ASSERT_TRUE(stale2) << describe(out2);
  EXPECT_TRUE(stale2->front().actual.empty());
}

TEST_P(CatalogEditTest, ReportsEveryStaleRowAndWritesNothing) {
  const ChangeSeq before = seq();
  CatalogEditBatch batch;
  batch.edits = {
      CatalogUpdate{CatalogTable::Sample, cat_.s1, {{"note", text("x")}}, {{"note", text("a")}}},
      CatalogUpdate{CatalogTable::Sample, cat_.s2, {}, {{"note", text("b")}}},
      CatalogUpdate{CatalogTable::Sample, cat_.s3, {{"note", text("y")}}, {{"note", text("c")}}},
  };
  const auto out = apply(batch);
  const auto* stale = std::get_if<std::vector<StaleRow>>(&out);
  ASSERT_TRUE(stale) << describe(out);
  EXPECT_EQ(stale->size(), 2u);
  EXPECT_EQ(row(CatalogTable::Sample, cat_.s2).at("note"), CatalogValue{});
  EXPECT_EQ(seq(), before);
}

TEST_P(CatalogEditTest, MixedFailuresWriteNothing) {
  const ChangeSeq before = seq();
  const Uuid fresh = Uuid::v7();
  CatalogEditBatch batch;
  batch.edits = {
      CatalogInsert{CatalogTable::Material, fresh, {{"name", text("quartz")}}},
      CatalogUpdate{CatalogTable::Sample, cat_.s1, {{"note", text("x")}}, {{"note", text("a")}}},
      CatalogDelete{CatalogTable::Project, cat_.p_ross1, {}},  // in use
  };
  const auto out = apply(batch);
  EXPECT_FALSE(applied(out));
  EXPECT_FALSE(*store_->catalog_row(CatalogTable::Material, fresh));
  EXPECT_EQ(seq(), before);
}

TEST_P(CatalogEditTest, DisjointFieldsBothApply) {
  CatalogEditBatch mine;
  mine.edits = {CatalogUpdate{CatalogTable::Sample, cat_.s1, {{"note", CatalogValue{}}}, {{"note", text("mine")}}}};
  CatalogEditBatch theirs;
  theirs.edits = {CatalogUpdate{CatalogTable::Sample, cat_.s1, {{"unit", CatalogValue{}}}, {{"unit", text("theirs")}}}};
  ASSERT_TRUE(applied(apply(mine)));
  ASSERT_TRUE(applied(apply(theirs)));
  const CatalogFields r = row(CatalogTable::Sample, cat_.s1);
  EXPECT_EQ(r.at("note"), text("mine"));
  EXPECT_EQ(r.at("unit"), text("theirs"));
  // An unchanged value is no change: no audit row, no change_log entry.
  const ChangeSeq before = seq();
  CatalogEditBatch same;
  same.edits = {CatalogUpdate{CatalogTable::Sample, cat_.s1, {{"note", text("mine")}}, {{"note", text("mine")}}}};
  ASSERT_TRUE(applied(apply(same)));
  EXPECT_EQ(seq(), before);
}

TEST_P(CatalogEditTest, SameFieldRace) {
  TestDatabase shared(GetParam(), true);
  auto setup = open_or_die(shared.url());
  ASSERT_TRUE(setup);
  const Lab lab = seed_lab(*setup);
  const EntryCatalog cat = seed_entry(*setup, lab);
  constexpr int kRounds = 20;
  for (int round = 0; round < kRounds; ++round) {
    const CatalogValue expected =
        round == 0 ? CatalogValue{} : CatalogValue{std::string("r") + std::to_string(round - 1)};
    std::atomic<int> wins{0}, stale{0};
    std::atomic<bool> errors{false};
    auto writer = [&]() {
      auto store = open_store(StoreConfig{shared.url(), false});
      if (!store) {
        errors = true;
        return;
      }
      CatalogEditBatch b;
      b.edits = {CatalogUpdate{CatalogTable::Sample, cat.s1, {{"note", expected}},
                               {{"note", std::string("r") + std::to_string(round)}}}};
      // Both writers write the same text, so the next round's expected value is known.
      auto out = (*store)->apply_catalog_edits(lab.reduction_client, b);
      if (!out) {
        errors = true;
        return;
      }
      if (applied(*out)) ++wins;
      if (std::holds_alternative<std::vector<StaleRow>>(*out)) ++stale;
    };
    std::thread a(writer), b(writer);
    a.join();
    b.join();
    ASSERT_FALSE(errors);
    ASSERT_EQ(wins.load(), 1) << "round " << round;
    ASSERT_EQ(stale.load(), 1) << "round " << round;
  }
}

TEST_P(CatalogEditTest, UniqueViolationIsRefusal) {
  CatalogEditBatch dup;
  dup.edits = {CatalogInsert{CatalogTable::Sample, Uuid::v7(),
                             {{"name", text("bt-1")}, {"project_uuid", cat_.p_ross1}, {"material_uuid", cat_.biotite}}}};
  auto r = refusals(apply(dup));
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].rule, "unique");

  CatalogEditBatch taken;
  taken.edits = {CatalogUpdate{CatalogTable::IrradiationPosition, cat_.pos_a2, {}, {{"position", std::int64_t{1}}}}};
  r = refusals(apply(taken));
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].rule, "unique");

  // A project without a PI clashes with another of the same name and no PI.
  const Uuid lone = *store_->add_project(client(), {"Lone", std::nullopt});
  (void)lone;
  CatalogEditBatch nopi;
  nopi.edits = {CatalogInsert{CatalogTable::Project, Uuid::v7(), {{"name", text("Lone")}}}};
  r = refusals(apply(nopi));
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].rule, "unique");
}

TEST_P(CatalogEditTest, AnalyzedIdentifierProtected) {
  const Uuid analyzed = identify(cat_.pos_a1, "70001");
  analyze("70001");
  const Uuid loaded = identify(cat_.pos_a2, "70002");
  const Uuid load = *store_->add_load(client(), {"load-9"});
  ASSERT_TRUE(store_->add_load_position(client(), {load, 1, loaded}));
  const Uuid free_id = identify(cat_.pos_b1, "70003");

  for (const Uuid id : {analyzed, loaded}) {
    CatalogEditBatch upd;
    upd.edits = {CatalogUpdate{CatalogTable::Identifier, id, {}, {{"identifier", text("80000")}}}};
    auto r = refusals(apply(upd));
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].rule, "analyzed_identifier");
    CatalogEditBatch del;
    del.edits = {CatalogDelete{CatalogTable::Identifier, id, {}}};
    r = refusals(apply(del));
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].rule, "analyzed_identifier");
  }
  // A position whose identifier is analyzed cannot move.
  CatalogEditBatch move;
  move.edits = {CatalogUpdate{CatalogTable::IrradiationPosition, cat_.pos_a1, {}, {{"position", std::int64_t{7}}}}};
  auto r = refusals(apply(move));
  ASSERT_FALSE(r.empty());
  EXPECT_EQ(r[0].rule, "analyzed_identifier");

  CatalogEditBatch ok;
  ok.edits = {CatalogDelete{CatalogTable::Identifier, free_id, {{"identifier", text("70003")}}}};
  ASSERT_TRUE(applied(apply(ok)));
}

TEST_P(CatalogEditTest, AnalyzedSampleChangeNeedsFlag) {
  identify(cat_.pos_a1, "70001");
  analyze("70001");
  CatalogEditBatch batch;
  batch.edits = {CatalogUpdate{CatalogTable::IrradiationPosition, cat_.pos_a1, {{"sample_uuid", cat_.fc2}}, {{"sample_uuid", cat_.s3}}}};
  auto r = refusals(apply(batch));
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].rule, "analyzed_sample_change");
  EXPECT_NE(r[0].what.find("1 analyses"), std::string::npos) << r[0].what;
  batch.allow_analyzed_sample_change = true;
  ASSERT_TRUE(applied(apply(batch)));
  EXPECT_EQ(row(CatalogTable::IrradiationPosition, cat_.pos_a1).at("sample_uuid"), CatalogValue{cat_.s3});
  // An unanalyzed position needs no flag; clearing a sample keeps the identifier.
  const Uuid id = identify(cat_.pos_b1, "70002");
  CatalogEditBatch clear;
  clear.edits = {CatalogUpdate{CatalogTable::IrradiationPosition, cat_.pos_b1, {}, {{"sample_uuid", CatalogValue{}}}}};
  ASSERT_TRUE(applied(apply(clear)));
  EXPECT_EQ(*store_->find_identifier("70002"), id);
}

TEST_P(CatalogEditTest, DeletePositionWithIdentifierRefused) {
  identify(cat_.pos_b1, "70002");
  CatalogEditBatch batch;
  batch.edits = {CatalogDelete{CatalogTable::IrradiationPosition, cat_.pos_b1, {}}};
  auto r = refusals(apply(batch));
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].rule, "position_has_identifier");
  EXPECT_NE(r[0].what.find("70002"), std::string::npos);
}

TEST_P(CatalogEditTest, InUseDeleteNamesReferrers) {
  CatalogEditBatch batch;
  batch.edits = {CatalogDelete{CatalogTable::Material, cat_.sanidine, {}},
                 CatalogDelete{CatalogTable::PrincipalInvestigator, cat_.ross, {}},
                 CatalogDelete{CatalogTable::Sample, cat_.fc2, {}}};
  auto r = refusals(apply(batch));
  ASSERT_EQ(r.size(), 3u);
  for (const auto& x : r) EXPECT_EQ(x.rule, "in_use");
  EXPECT_NE(r[0].what.find("3 sample"), std::string::npos) << r[0].what;
  EXPECT_NE(r[1].what.find("2 project"), std::string::npos) << r[1].what;
  EXPECT_NE(r[2].what.find("1 irradiation_position"), std::string::npos) << r[2].what;
}

TEST_P(CatalogEditTest, RenameRewritesRefKeys) {
  const Uuid flux = *store_->add_ref_object(client(), {RefType::FluxPosition, "NM-301/A/1", cat_.nm301, cat_.level_a, cat_.pos_a1, std::nullopt, std::nullopt});
  const Uuid geom = *store_->add_ref_object(client(), {RefType::LevelGeometry, "NM-301/A", cat_.nm301, cat_.level_a, std::nullopt, std::nullopt, std::nullopt});
  const Uuid geom_b = *store_->add_ref_object(client(), {RefType::LevelGeometry, "NM-301/B", cat_.nm301, cat_.level_b, std::nullopt, std::nullopt, std::nullopt});
  const Uuid chron = *store_->add_ref_object(client(), {RefType::Chronology, "NM-301", cat_.nm301, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  const Uuid prod = *store_->add_ref_object(client(), {RefType::Production, "NM-301/Triga", cat_.nm301, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  // Another package whose name starts the same way is left alone.
  const Uuid other = *store_->add_irradiation(client(), "NM-3010");
  const Uuid other_chron = *store_->add_ref_object(client(), {RefType::Chronology, "NM-3010", other, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

  CatalogEditBatch level;
  level.edits = {CatalogUpdate{CatalogTable::Level, cat_.level_a, {{"name", text("A")}}, {{"name", text("C")}}}};
  ASSERT_TRUE(applied(apply(level)));
  EXPECT_EQ(*store_->find_catalog_row(CatalogTable::RefObject, {std::string("flux_position"), std::string("NM-301/C/1")}), flux);
  EXPECT_EQ(*store_->find_catalog_row(CatalogTable::RefObject, {std::string("level_geometry"), std::string("NM-301/C")}), geom);
  EXPECT_EQ(*store_->find_catalog_row(CatalogTable::RefObject, {std::string("level_geometry"), std::string("NM-301/B")}), geom_b);

  CatalogEditBatch pkg;
  pkg.edits = {CatalogUpdate{CatalogTable::Irradiation, cat_.nm301, {{"name", text("NM-301")}}, {{"name", text("NM-302")}}}};
  ASSERT_TRUE(applied(apply(pkg)));
  EXPECT_EQ(*store_->find_catalog_row(CatalogTable::RefObject, {std::string("flux_position"), std::string("NM-302/C/1")}), flux);
  EXPECT_EQ(*store_->find_catalog_row(CatalogTable::RefObject, {std::string("level_geometry"), std::string("NM-302/B")}), geom_b);
  EXPECT_EQ(*store_->find_catalog_row(CatalogTable::RefObject, {std::string("chronology"), std::string("NM-302")}), chron);
  EXPECT_EQ(*store_->find_catalog_row(CatalogTable::RefObject, {std::string("production"), std::string("NM-302/Triga")}), prod);
  EXPECT_EQ(*store_->find_catalog_row(CatalogTable::RefObject, {std::string("chronology"), std::string("NM-3010")}), other_chron);
}

TEST_P(CatalogEditTest, AnalyzedRenameRefused) {
  identify(cat_.pos_b1, "70002");
  analyze("70002");
  CatalogEditBatch level;
  level.edits = {CatalogUpdate{CatalogTable::Level, cat_.level_b, {}, {{"name", text("Z")}}}};
  auto r = refusals(apply(level));
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].rule, "analyzed_rename");
  CatalogEditBatch pkg;
  pkg.edits = {CatalogUpdate{CatalogTable::Irradiation, cat_.nm301, {}, {{"name", text("NM-999")}}}};
  r = refusals(apply(pkg));
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].rule, "analyzed_rename");
  // A level without analyses in the same package can still be renamed.
  CatalogEditBatch other;
  other.edits = {CatalogUpdate{CatalogTable::Level, cat_.level_a, {}, {{"name", text("Q")}}}};
  EXPECT_TRUE(applied(apply(other)));
}

TEST_P(CatalogEditTest, SampleUpdatedUtcAdvances) {
  auto before = store_->samples({"bt-1", std::nullopt, std::nullopt, std::nullopt, 10});
  ASSERT_TRUE(before && before->size() == 1);
  CatalogEditBatch batch;
  batch.edits = {CatalogUpdate{CatalogTable::Sample, cat_.s1, {}, {{"note", text("n")}}}};
  ASSERT_TRUE(applied(apply(batch)));
  auto after = store_->samples({"bt-1", std::nullopt, std::nullopt, std::nullopt, 10});
  ASSERT_TRUE(after && after->size() == 1);
  EXPECT_GT(after->front().updated.iso(), before->front().updated.iso());
}

TEST_P(CatalogEditTest, AuditDetailHasBeforeAfter) {
  TestDatabase shared(GetParam(), true);
  auto store = open_or_die(shared.url());
  ASSERT_TRUE(store);
  const Lab lab = seed_lab(*store);
  const EntryCatalog cat = seed_entry(*store, lab);
  const ChangeSeq before = *store->latest_change_seq();
  CatalogEditBatch batch;
  batch.edits = {CatalogUpdate{CatalogTable::Sample, cat.s1, {}, {{"note", text("n")}, {"name", text("bt-1")}}},
                 CatalogUpdate{CatalogTable::Sample, cat.s2, {}, {{"unit", text("u")}}}};
  auto out = store->apply_catalog_edits(lab.reduction_client, batch);
  ASSERT_TRUE(out && applied(*out));
  auto page = store->changes_since(before, 10);
  ASSERT_TRUE(page);
  ASSERT_EQ(page->entries.size(), 1u);
  EXPECT_EQ(page->entries[0].kind, "catalog");
  EXPECT_EQ(page->entries[0].entities.size(), 2u);
  auto db = pd::Db::open(StoreConfig{shared.url(), false});
  ASSERT_TRUE(db);
  auto row = (*db)->select_one("SELECT detail FROM change_entity WHERE entity_uuid = ? AND op = 'update'", {pd::qv(cat.s1)});
  ASSERT_TRUE(row && *row);
  std::string diff = pd::to_std((*row)->value("detail"));
  std::erase(diff, ' ');  // jsonb spaces it out
  EXPECT_NE(diff.find("\"note\":[null,\"n\"]"), std::string::npos) << diff;
  EXPECT_EQ(diff.find("name"), std::string::npos) << diff;  // unchanged: not recorded
}

TEST_P(CatalogEditTest, LaterEditNamesEarlierInsert) {
  const Uuid project = Uuid::v7(), sample = Uuid::v7();
  CatalogEditBatch batch;
  batch.edits = {CatalogInsert{CatalogTable::Project, project, {{"name", text("New")}, {"pi_uuid", cat_.smith}}},
                 CatalogInsert{CatalogTable::Sample, sample, {{"name", text("n-1")}, {"project_uuid", project}, {"material_uuid", cat_.sanidine}}},
                 CatalogUpdate{CatalogTable::Sample, sample, {{"name", text("n-1")}}, {{"note", text("later")}}}};
  const auto out = apply(batch);
  ASSERT_TRUE(applied(out)) << describe(out);
  EXPECT_EQ(row(CatalogTable::Sample, sample).at("note"), text("later"));
}

TEST_P(CatalogEditTest, LevelSaveWithRefs) {
  const Uuid geom = *store_->add_ref_object(client(), {RefType::LevelGeometry, "NM-301/A", cat_.nm301, cat_.level_a, std::nullopt, std::nullopt, std::nullopt});
  const ChangeSeq before = seq();
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(geom, Kind::RefValue, RevisionPayload{RefPayload{LevelZValue{3.0}}}, std::nullopt));
  CatalogEditBatch batch;
  batch.edits = {CatalogUpdate{CatalogTable::IrradiationPosition, cat_.pos_a2, {}, {{"note", text("n")}}}};
  auto out = store_->apply_catalog_edits(reducer(), batch, *uow, ChangesetKind::Reference, "level save");
  ASSERT_TRUE(out) << to_string(out.error());
  ASSERT_TRUE(applied(*out)) << describe(*out);
  auto page = store_->changes_since(before, 10);
  ASSERT_EQ(page->entries.size(), 1u);
  EXPECT_EQ(page->entries[0].kind, "changeset");
  EXPECT_TRUE(page->entries[0].changeset);
  auto sheet = **store_->level_sheet(cat_.level_a);
  ASSERT_TRUE(sheet.z);
  EXPECT_EQ(sheet.z->z, 3.0);
  EXPECT_EQ(sheet.positions[1].note, "n");
}

TEST_P(CatalogEditTest, RefConflictRollsBackCatalog) {
  const Uuid geom = *store_->add_ref_object(client(), {RefType::LevelGeometry, "NM-301/A", cat_.nm301, cat_.level_a, std::nullopt, std::nullopt, std::nullopt});
  // Someone else writes the first z.
  {
    auto other = *store_->begin(reducer());
    ASSERT_TRUE(other->add_revision(geom, Kind::RefValue, RevisionPayload{RefPayload{LevelZValue{1.0}}}, std::nullopt));
    ASSERT_TRUE(other->commit(ChangesetKind::Reference, "theirs"));
  }
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(geom, Kind::RefValue, RevisionPayload{RefPayload{LevelZValue{3.0}}}, std::nullopt));
  CatalogEditBatch batch;
  batch.edits = {CatalogUpdate{CatalogTable::IrradiationPosition, cat_.pos_a2, {}, {{"note", text("n")}}}};
  auto out = store_->apply_catalog_edits(reducer(), batch, *uow, ChangesetKind::Reference, "level save");
  ASSERT_TRUE(out);
  const auto* lost = std::get_if<std::vector<RefConflict>>(&*out);
  ASSERT_TRUE(lost) << describe(*out);
  ASSERT_EQ(lost->size(), 1u);
  EXPECT_EQ(lost->front().subject, geom);
  EXPECT_TRUE(lost->front().actual);
  EXPECT_EQ(row(CatalogTable::IrradiationPosition, cat_.pos_a2).at("note"), CatalogValue{});
}

TEST_P(CatalogEditTest, StaleCatalogRollsBackRefs) {
  const Uuid geom = *store_->add_ref_object(client(), {RefType::LevelGeometry, "NM-301/A", cat_.nm301, cat_.level_a, std::nullopt, std::nullopt, std::nullopt});
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(geom, Kind::RefValue, RevisionPayload{RefPayload{LevelZValue{3.0}}}, std::nullopt));
  CatalogEditBatch batch;
  batch.edits = {CatalogUpdate{CatalogTable::IrradiationPosition, cat_.pos_a2, {{"note", text("x")}}, {{"note", text("n")}}}};
  auto out = store_->apply_catalog_edits(reducer(), batch, *uow, ChangesetKind::Reference, "level save");
  ASSERT_TRUE(out);
  EXPECT_TRUE(std::holds_alternative<std::vector<StaleRow>>(*out));
  EXPECT_FALSE(*store_->head(geom, Kind::RefValue));
}

INSTANTIATE_TEST_SUITE_P(Engines, CatalogEditTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
