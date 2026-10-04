#include <gtest/gtest.h>

#include "catalog_fixture.hpp"
#include "pychron/entry/level_sheet.hpp"

using namespace pychron;
using namespace pychron::entry;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

HolderValue holder(int holes, bool numbered = false) {
  HolderValue h;
  h.shape = "circle";
  h.radius = 1.0;
  h.has_hole_numbers = numbered;
  for (int i = 0; i < holes; ++i)
    h.holes.push_back(HolderHole{i, numbered ? "h" + std::to_string(i + 1) : std::to_string(i + 1), 0.1 * i, 0.0, std::nullopt});
  return h;
}

class LevelSheetTest : public EntryTest {
 protected:
  LevelSheetEdit edit(Uuid level, std::optional<HolderValue> h = holder(4)) {
    return LevelSheetEdit(**store_->level_sheet(level), std::move(h));
  }
  CatalogOutcome save(const LevelSheetEdit& e, bool allow = false) {
    auto batch = e.to_batch();
    batch.allow_analyzed_sample_change = allow;
    auto uow = *store_->begin(reducer());
    auto staged = e.stage_refs(*uow);
    EXPECT_TRUE(staged);
    auto out = *staged ? store_->apply_catalog_edits(reducer(), batch, *uow, ChangesetKind::Reference, "save")
                       : store_->apply_catalog_edits(client(), batch);
    EXPECT_TRUE(out) << (out ? "" : to_string(out.error()));
    return out ? *out : CatalogOutcome{std::vector<Refusal>{}};
  }
};

}  // namespace

TEST_P(LevelSheetTest, HolesFromHolderOrdinal) {
  auto e = edit(cat_.level_a, holder(3, true));
  ASSERT_EQ(e.rows().size(), 3u);
  EXPECT_EQ(e.rows()[0].position, 1);
  EXPECT_EQ(e.rows()[0].hole_id, "h1");
  EXPECT_EQ(e.rows()[0].sample_name, "FC-2");
  EXPECT_EQ(e.rows()[2].position, 3);
  EXPECT_TRUE(e.rows()[2].empty());
  EXPECT_FALSE(e.dirty());
  EXPECT_TRUE(e.to_batch().edits.empty());
}

TEST_P(LevelSheetTest, UnchangedEmitsNothingAndAssignInsertsOnce) {
  auto e = edit(cat_.level_a);
  SampleRow real = (*store_->samples({"bt-2", std::nullopt, std::nullopt, std::nullopt, 10})).front();
  e.assign_sample({3}, real);
  e.set_weight(3, 2.5);
  ASSERT_TRUE(e.dirty());
  const auto batch = e.to_batch();
  ASSERT_EQ(batch.edits.size(), 1u);
  const auto& insert = std::get<CatalogInsert>(batch.edits[0]);
  EXPECT_EQ(insert.values.at("position"), CatalogValue{std::int64_t{3}});
  EXPECT_EQ(insert.values.at("sample_uuid"), CatalogValue{real.uuid});
  ASSERT_TRUE(std::holds_alternative<CatalogApplied>(save(e)));
  const auto again = edit(cat_.level_a);
  EXPECT_EQ(again.row(3)->sample_name, "bt-2");
  EXPECT_EQ(again.row(3)->weight, 2.5);
  EXPECT_FALSE(again.dirty());
}

TEST_P(LevelSheetTest, ClearKeepsIdentifier) {
  ASSERT_TRUE(store_->add_identifier(client(), {"70001", "unknown", std::nullopt, std::nullopt, cat_.pos_a1, std::nullopt, std::nullopt}));
  auto e = edit(cat_.level_a);
  e.clear({1}, {SheetField::Packet, SheetField::Note});
  const auto batch = e.to_batch();
  ASSERT_EQ(batch.edits.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<CatalogUpdate>(batch.edits[0]));
  // Clearing the sample of a row with an identifier is a validation error, not a delete.
  e.clear({1}, {SheetField::Sample});
  EXPECT_FALSE(e.validate({}).empty());
  for (const auto& edit : e.to_batch().edits) EXPECT_FALSE(std::holds_alternative<CatalogDelete>(edit));
}

TEST_P(LevelSheetTest, StoredRowsClearedAreUpdatedNotDeleted) {
  auto e = edit(cat_.level_a);
  e.clear({2}, {SheetField::Sample, SheetField::Weight, SheetField::Packet, SheetField::Note});
  ASSERT_TRUE(std::holds_alternative<CatalogApplied>(save(e)));
  auto after = **store_->level_sheet(cat_.level_a);
  ASSERT_EQ(after.positions.size(), 2u);
  EXPECT_FALSE(after.positions[1].sample);
}

TEST_P(LevelSheetTest, MoveRefusedWhenAnalyzed) {
  ASSERT_TRUE(store_->add_identifier(client(), {"70001", "unknown", std::nullopt, std::nullopt, cat_.pos_a1, std::nullopt, std::nullopt}));
  analyze("70001");
  auto e = edit(cat_.level_a);
  EXPECT_FALSE(e.move(1, 4));
  EXPECT_FALSE(e.move(2, 1));  // not empty
  ASSERT_TRUE(e.move(2, 4));
  EXPECT_TRUE(e.row(2)->empty());
  EXPECT_EQ(e.row(4)->sample_name, "bt-1");
  ASSERT_TRUE(std::holds_alternative<CatalogApplied>(save(e)));
  EXPECT_EQ((**store_->level_sheet(cat_.level_a)).positions[1].position, 4);
}

TEST_P(LevelSheetTest, FillPacketsSequence) {
  auto e = edit(cat_.level_a);
  ASSERT_TRUE(e.fill_packets({4, 2, 3}, "P9"));
  EXPECT_EQ(e.row(2)->packet, "P9");
  EXPECT_EQ(e.row(3)->packet, "P10");
  EXPECT_EQ(e.row(4)->packet, "P11");
  EXPECT_FALSE(e.fill_packets({1}, "P"));
}

TEST_P(LevelSheetTest, ValidatePackets) {
  auto e = edit(cat_.level_a);
  e.set_packet(2, std::string("7P"));
  EXPECT_EQ(e.validate({}).size(), 1u);
  e.set_packet(2, std::string("P7"));
  EXPECT_TRUE(e.validate({}).empty());
  EntrySettings needs;
  needs.null_identifier_rows = "packet";
  e.set_packet(2, std::nullopt);
  EXPECT_EQ(e.validate(needs).size(), 1u);
}

TEST_P(LevelSheetTest, OrphansAfterHolderShrink) {
  auto e = edit(cat_.level_a, holder(1));
  ASSERT_EQ(e.rows().size(), 2u);
  EXPECT_FALSE(e.rows()[0].orphan);
  EXPECT_TRUE(e.rows()[1].orphan);
  EXPECT_EQ(e.rows()[1].sample_name, "bt-1");
  EXPECT_TRUE(e.to_batch().edits.empty());  // nothing deleted
  e.set_holder(std::nullopt, holder(5));
  EXPECT_EQ(e.rows().size(), 5u);
  EXPECT_FALSE(e.rows()[1].orphan);
}

TEST_P(LevelSheetTest, AnalyzedSampleChangeIsCountedAndNeedsTheFlag) {
  ASSERT_TRUE(store_->add_identifier(client(), {"70001", "unknown", std::nullopt, std::nullopt, cat_.pos_a1, std::nullopt, std::nullopt}));
  analyze("70001");
  auto e = edit(cat_.level_a);
  SampleRow other = (*store_->samples({"Other", std::nullopt, std::nullopt, std::nullopt, 10})).front();
  e.assign_sample({1}, other);
  EXPECT_EQ(e.analyses_changing_sample(), 1);
  EXPECT_TRUE(std::holds_alternative<std::vector<Refusal>>(save(e)));
  auto again = edit(cat_.level_a);
  again.assign_sample({1}, other);
  EXPECT_TRUE(std::holds_alternative<CatalogApplied>(save(again, true)));
}

TEST_P(LevelSheetTest, ZAndProductionCreateTheirReferences) {
  const Uuid prod = *store_->add_ref_object(client(), {RefType::Production, "NM-301/Triga", cat_.nm301, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  auto e = edit(cat_.level_a);
  e.set_z(2.0);
  e.set_production(prod);
  e.set_level_note(std::string("top"));
  ASSERT_TRUE(std::holds_alternative<CatalogApplied>(save(e)));
  auto sheet = **store_->level_sheet(cat_.level_a);
  ASSERT_TRUE(sheet.z);
  EXPECT_EQ(sheet.z->z, 2.0);
  ASSERT_TRUE(sheet.production_value);
  EXPECT_EQ(sheet.production_value->production, prod);
  EXPECT_EQ(sheet.level.note, "top");
  // A second edit revises the existing references.
  auto next = LevelSheetEdit(sheet, holder(4));
  next.set_z(3.0);
  ASSERT_TRUE(std::holds_alternative<CatalogApplied>(save(next)));
  EXPECT_EQ((**store_->level_sheet(cat_.level_a)).z->z, 3.0);
  // An edit made from the first load is stale on the reference.
  auto stale = LevelSheetEdit(sheet, holder(4));
  stale.set_z(4.0);
  EXPECT_TRUE(std::holds_alternative<std::vector<RefConflict>>(save(stale)));
}

INSTANTIATE_TEST_SUITE_P(Engines, LevelSheetTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
