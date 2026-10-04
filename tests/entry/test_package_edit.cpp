#include <gtest/gtest.h>

#include "pychron/entry/package_edit.hpp"
#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::entry;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

Dose dose(const char* start, const char* end, double power = 1.0) {
  return Dose{0, power, *UtcTime::parse(start), *UtcTime::parse(end)};
}

NewPackage irradiation(const std::string& name) {
  NewPackage p;
  p.name = name;
  p.doses = {dose("2026-09-01T15:00:00Z", "2026-09-01T23:00:00Z"), dose("2026-09-02T15:00:00Z", "2026-09-02T19:00:00Z")};
  p.reactor = "Triga";
  ProductionValue v;
  v.ratios = {{"K4039", 0.007614, 0.000105}, {"Ca3937", 0.00066, 1e-05}};
  p.production = v;
  p.levels = {{"A", std::nullopt, 0.5, std::string("top")}, {"B", std::nullopt, std::nullopt, std::nullopt}};
  return p;
}

}  // namespace

TEST(PackageEdit, Validate) {
  EXPECT_TRUE(validate(irradiation("NM-302"), {"NM-301"}).empty());
  EXPECT_FALSE(validate(irradiation("NM-301"), {"NM-301"}).empty());
  EXPECT_FALSE(validate(irradiation("NM 302"), {}).empty());
  auto p = irradiation("NM-302");
  p.reactor.reset();
  EXPECT_EQ(validate(p, {}).size(), 1u);
  p = irradiation("NM-302");
  p.doses = {dose("2026-09-01T15:00:00Z", "2026-09-01T10:00:00Z", 0.0)};
  EXPECT_EQ(validate(p, {}).size(), 2u);
  p = irradiation("NM-302");
  p.doses = {dose("2026-09-01T15:00:00Z", "2026-09-01T23:00:00Z"), dose("2026-09-01T20:00:00Z", "2026-09-02T01:00:00Z")};
  EXPECT_EQ(validate(p, {}).size(), 1u);
  p = irradiation("NM-302");
  p.levels.push_back({"A", std::nullopt, std::nullopt, std::nullopt});
  EXPECT_EQ(validate(p, {}).size(), 1u);
  NewPackage plain;
  plain.name = "P-1";
  plain.kind = "package";
  EXPECT_TRUE(validate(plain, {}).empty());  // no reactor or dose needed
}

TEST(PackageEdit, HoursAndEstimatedJ) {
  const auto p = irradiation("x");
  EXPECT_DOUBLE_EQ(dose_hours(p.doses), 12.0);
  EntrySettings s;
  EXPECT_DOUBLE_EQ(estimated_j(p.doses, s), 12.0 * 1e-4);
}

TEST(PackageEdit, Reactors) {
  auto r = parse_reactors(R"({"Triga": {"K4039": [0.007614, 0.000105], "Cl_K": [0.227, 0.0], "source_path": "x"}, "bad": 3})");
  ASSERT_TRUE(r);
  ASSERT_EQ(r->size(), 1u);
  const auto& triga = r->at("Triga");
  EXPECT_EQ(triga.reactor, "Triga");
  ASSERT_EQ(triga.ratios.size(), 2u);
  EXPECT_EQ(triga.ratios[0].key, "K4039");
  EXPECT_EQ(triga.ratios[1].key, "Cl_K");
  EXPECT_FALSE(parse_reactors("[]"));
}

namespace {
class PackageStoreTest : public StoreTest {
 protected:
  Actor actor() const { return Actor{lab_.reducer, lab_.reduction_client}; }
};
}  // namespace

TEST_P(PackageStoreTest, CreateIrradiationInOneChange) {
  const ChangeSeq before = *store_->latest_change_seq();
  auto made = create_package(*store_, actor(), irradiation("NM-302"));
  ASSERT_TRUE(made) << to_string(made.error());
  EXPECT_EQ(*store_->latest_change_seq(), before + 1);
  auto levels = store_->levels(made->package);
  ASSERT_TRUE(levels);
  ASSERT_EQ(levels->size(), 2u);
  auto a = **store_->level_sheet(made->levels[0]);
  ASSERT_TRUE(a.z);
  EXPECT_EQ(a.z->z, 0.5);
  EXPECT_EQ(a.level.note, "top");
  ASSERT_TRUE(a.production_value);
  auto productions = package_productions(*store_, made->package, "NM-302");
  ASSERT_TRUE(productions);
  ASSERT_EQ(productions->size(), 1u);
  EXPECT_EQ(productions->front().name, "Triga");
  EXPECT_EQ(productions->front().value.reactor, "Triga");
  EXPECT_EQ(a.production_value->production, productions->front().ref_object);
  auto chronology = package_chronology(*store_, made->package, "NM-302");
  ASSERT_TRUE(chronology);
  ASSERT_EQ(chronology->value.doses.size(), 2u);
  EXPECT_EQ(chronology->value.doses[1].ordinal, 1);
  auto rows = store_->irradiations();
  EXPECT_TRUE(std::any_of(rows->begin(), rows->end(), [](const IrradiationRow& r) { return r.name == "NM-302" && r.has_chronology; }));

  // References resolve for an analysis at a position of the new package.
  const Uuid pos = *store_->add_irradiation_position(lab_.reduction_client, {made->levels[0], 1, std::nullopt, std::nullopt, {}, {}, std::nullopt});
  ASSERT_TRUE(store_->add_identifier(lab_.reduction_client, {"70001", "unknown", std::nullopt, std::nullopt, pos, std::nullopt, std::nullopt}));
  const auto item = analysis_item(lab_, 1, series(1), series(0), "70001");
  ASSERT_TRUE(store_->ingest(item));
  auto refs = store_->resolve_refs(std::get<AnalysisIngest>(item.body).analysis, RefPolicy{});
  ASSERT_TRUE(refs);
  int chron = 0, prod = 0;
  for (const auto& r : refs->refs) {
    chron += r.type == RefType::Chronology;
    prod += r.type == RefType::Production;
  }
  EXPECT_EQ(chron, 1);
  EXPECT_EQ(prod, 1);
}

TEST_P(PackageStoreTest, PackageWritesNoRefs) {
  NewPackage p;
  p.name = "P-1";
  p.kind = "package";
  p.levels = {{"A", std::nullopt, std::nullopt, std::nullopt}};
  auto made = create_package(*store_, actor(), p);
  ASSERT_TRUE(made) << to_string(made.error());
  EXPECT_TRUE(store_->ref_objects(RefType::Chronology, made->package)->empty());
  EXPECT_TRUE(store_->ref_objects(RefType::Production, made->package)->empty());
  auto rows = *store_->irradiations();
  EXPECT_TRUE(std::any_of(rows.begin(), rows.end(), [](const IrradiationRow& r) { return r.name == "P-1" && r.kind == "package"; }));
}

TEST_P(PackageStoreTest, KindChangeKeepsRefs) {
  auto made = create_package(*store_, actor(), irradiation("NM-303"));
  ASSERT_TRUE(made);
  CatalogEditBatch batch;
  batch.edits = {CatalogUpdate{CatalogTable::Irradiation, made->package, {{"kind", std::string("irradiation")}},
                               {{"kind", std::string("package")}}}};
  auto out = store_->apply_catalog_edits(lab_.reduction_client, batch);
  ASSERT_TRUE(out && std::holds_alternative<CatalogApplied>(*out));
  EXPECT_EQ(store_->ref_objects(RefType::Chronology, made->package)->size(), 1u);
}

TEST_P(PackageStoreTest, DuplicateNameIsAnError) {
  ASSERT_TRUE(create_package(*store_, actor(), irradiation("NM-304")));
  EXPECT_FALSE(create_package(*store_, actor(), irradiation("NM-304")));
}

INSTANTIATE_TEST_SUITE_P(Engines, PackageStoreTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
