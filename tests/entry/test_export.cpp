#include <gtest/gtest.h>

#include "catalog_fixture.hpp"
#include "pychron/entry/export.hpp"
#include "pychron/entry/identifier_plan.hpp"
#include "pychron/entry/positions_import.hpp"

using namespace pychron;
using namespace pychron::entry;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {
class ExportTest : public EntryTest {};
}  // namespace

TEST_P(ExportTest, ExportImportRoundTrip) {
  auto sheets = *package_sheets(*store_, cat_.nm301);
  const std::string csv = export_package_csv(sheets);
  EXPECT_NE(csv.find("A,1,,FC-2,Alpha,\"Ross, J\",sanidine,,,P1,,,"), std::string::npos) << csv;

  // Import the exported rows into a fresh package with the same levels.
  const Uuid pkg = *store_->add_irradiation(client(), "NM-400");
  const Uuid la = *store_->add_level(client(), {pkg, "A", std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  const Uuid lb = *store_->add_level(client(), {pkg, "B", std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  std::map<std::string, LevelSheetEdit> edits;
  edits.emplace("A", LevelSheetEdit(**store_->level_sheet(la), std::nullopt));
  edits.emplace("B", LevelSheetEdit(**store_->level_sheet(lb), std::nullopt));
  auto table = read_csv(csv);
  ASSERT_TRUE(table);
  auto catalog = read_snapshot(*store_);
  ASSERT_TRUE(catalog);
  const auto result = apply_position_import(*table, *catalog, edits);
  ASSERT_TRUE(result.errors.empty()) << result.errors.front();
  EXPECT_EQ(result.applied, 3);
  for (auto& [name, edit] : edits) {
    auto out = store_->apply_catalog_edits(client(), edit.to_batch());
    ASSERT_TRUE(out && std::holds_alternative<CatalogApplied>(*out));
  }
  auto copy = *package_sheets(*store_, pkg);
  // Same rows, apart from the level uuids.
  EXPECT_EQ(export_package_csv(copy), csv);
}

TEST_P(ExportTest, ImportErrorsApplyNothing) {
  std::map<std::string, LevelSheetEdit> edits;
  edits.emplace("A", LevelSheetEdit(**store_->level_sheet(cat_.level_a), std::nullopt));
  auto catalog = read_snapshot(*store_);
  auto table = read_csv("level,position,sample,weight\nA,5,bt-2,1\nZ,1,bt-2,\nA,0,bt-2,\nA,6,nope,\nA,7,bt-2,heavy\n");
  ASSERT_TRUE(table && catalog);
  const auto result = apply_position_import(*table, *catalog, edits);
  EXPECT_EQ(result.errors.size(), 4u);
  EXPECT_EQ(result.applied, 0);
  EXPECT_FALSE(edits.at("A").dirty());
}

INSTANTIATE_TEST_SUITE_P(Engines, ExportTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
