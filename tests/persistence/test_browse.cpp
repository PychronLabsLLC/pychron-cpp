// Browsing reads (data browsing and visualization design, section 9.2): newest
// first with keyset paging and totals, every filter, facets that ignore their
// own filter, analysis detail and raw blobs.

#include <gtest/gtest.h>

#include <algorithm>

#include "store_fixture.hpp"

namespace pychron::persistence::testing {
namespace {

class BrowseTest : public StoreTest {
 protected:
  void SetUp() override {
    StoreTest::SetUp();
    const Uuid c = lab_.acquisition_client;
    pi_ = *store_->add_principal_investigator(c, {"Ross", "J", std::nullopt, std::nullopt});
    project_ = *store_->add_project(c, {"Fish Canyon", pi_});
    material_ = *store_->add_material(c, {"sanidine", "60-80"});
    sample_ = *store_->add_sample(c, {"FC-2", project_, material_, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    ASSERT_TRUE(store_->add_extract_device(c, "co2"));
    const Uuid pos2 = *store_->add_irradiation_position(c, {lab_.level, 2, sample_, std::nullopt, {}, {}});
    ASSERT_TRUE(store_->add_identifier(c, {"77000", "unknown", std::nullopt, std::nullopt, pos2, std::nullopt}));
    // 77000-01, 77000-02 unknowns (FC-2, co2); 66574-01, -02 airs; 66573-01 blank.
    a1_ = add("77000", 1, "unknown", "2026-10-02T10:00:00Z", "co2", 5.0);
    a2_ = add("77000", 2, "unknown", "2026-10-02T11:00:00Z", "co2", 6.0);
    a3_ = add("66574", 1, "air", "2026-10-02T12:00:00Z");
    a4_ = add("66574", 2, "air", "2026-10-02T13:00:00Z");
    a5_ = add("66573", 1, "blank_unknown", "2026-10-02T14:00:00Z");
    // a4 is tagged invalid.
    auto uow = *store_->begin(Actor{lab_.reducer, lab_.reduction_client});
    ASSERT_TRUE(uow->add_revision(a4_, Kind::Tags, TagValue{"invalid", std::nullopt, std::nullopt},
                                  *store_->head(a4_, Kind::Tags)));
    ASSERT_TRUE(uow->commit(ChangesetKind::Reduction, "tag"));
    const Uuid repo = *store_->add_repository(lab_.reduction_client, "FishCanyon");
    ASSERT_TRUE(store_->add_repository_members(Actor{lab_.reducer, lab_.reduction_client}, repo, {a1_, a2_}));
  }

  Uuid add(const std::string& identifier, int aliquot, const std::string& type, const std::string& ts,
           std::optional<std::string> device = std::nullopt, std::optional<double> value = std::nullopt) {
    auto item = analysis_item(lab_, aliquot, series(1), series(0), identifier);
    auto& a = std::get<AnalysisIngest>(item.body);
    a.analysis_type = type;
    a.timestamp = *UtcTime::parse(ts);
    a.extract_device = device;
    a.extraction.extract_value = value;
    a.extraction.extract_units = value ? std::optional<std::string>("W") : std::nullopt;
    auto ok = store_->ingest(item);
    EXPECT_TRUE(ok) << (ok ? "" : to_string(ok.error()));
    return a.analysis;
  }

  std::vector<Uuid> uuids(const BrowseResult& r) {
    std::vector<Uuid> out;
    for (const auto& row : r.rows) out.push_back(row.summary.uuid);
    return out;
  }

  BrowseResult browse(BrowseFilter f, int limit = 100) {
    auto r = store_->browse(BrowseRequest{std::move(f), limit, std::nullopt, true});
    EXPECT_TRUE(r) << (r ? "" : to_string(r.error()));
    return r ? *r : BrowseResult{};
  }

  Uuid pi_, project_, material_, sample_, a1_, a2_, a3_, a4_, a5_;
};

TEST_P(BrowseTest, NewestFirstWithPagingAndTotals) {
  auto page1 = store_->browse(BrowseRequest{{}, 2, std::nullopt, true});
  ASSERT_TRUE(page1) << to_string(page1.error());
  EXPECT_EQ(uuids(*page1), (std::vector<Uuid>{a5_, a4_}));
  EXPECT_EQ(page1->total, 5);
  ASSERT_TRUE(page1->next);
  auto page2 = store_->browse(BrowseRequest{{}, 2, page1->next, false});
  ASSERT_TRUE(page2);
  EXPECT_EQ(uuids(*page2), (std::vector<Uuid>{a3_, a2_}));
  EXPECT_FALSE(page2->total);
  auto page3 = store_->browse(BrowseRequest{{}, 2, page2->next, false});
  ASSERT_TRUE(page3);
  EXPECT_EQ(uuids(*page3), (std::vector<Uuid>{a1_}));
  EXPECT_FALSE(page3->next);
  EXPECT_FALSE(store_->browse(BrowseRequest{{}, 0, std::nullopt, false}));
}

TEST_P(BrowseTest, RowsCarrySampleAndCatalogColumns) {
  const auto r = browse({});
  const auto it = std::find_if(r.rows.begin(), r.rows.end(), [&](const BrowseRow& row) { return row.summary.uuid == a2_; });
  ASSERT_NE(it, r.rows.end());
  EXPECT_EQ(it->summary.runid, "77000-02");
  EXPECT_EQ(it->sample, "FC-2");
  EXPECT_EQ(it->project, "Fish Canyon");
  EXPECT_EQ(it->material, "sanidine");
  EXPECT_EQ(it->principal_investigator, "Ross, J");
  EXPECT_EQ(it->extract_device, "co2");
  EXPECT_EQ(it->irradiation, "NM-300");
  EXPECT_EQ(it->level, "A");
  EXPECT_EQ(it->position, 2);
  EXPECT_EQ(it->load, "load-1");
  EXPECT_EQ(it->repository, "FishCanyon");
  EXPECT_EQ(it->tag, "ok");
  EXPECT_EQ(it->extract_value, 6.0);
  EXPECT_EQ(it->extract_units, "W");
  EXPECT_EQ(it->summary.timestamp, *UtcTime::parse("2026-10-02T11:00:00Z"));
}

TEST_P(BrowseTest, Filters) {
  BrowseFilter types;
  types.analysis_types = {"air", "blank_unknown"};
  EXPECT_EQ(uuids(browse(types)), (std::vector<Uuid>{a5_, a4_, a3_}));

  BrowseFilter sample;
  sample.samples = {"FC-2"};
  EXPECT_EQ(uuids(browse(sample)), (std::vector<Uuid>{a2_, a1_}));
  BrowseFilter project;
  project.projects = {"Fish Canyon"};
  project.principal_investigators = {"Ross, J"};
  project.materials = {"sanidine"};
  EXPECT_EQ(browse(project).rows.size(), 2u);

  BrowseFilter text;
  text.text = "6657";
  EXPECT_EQ(browse(text).rows.size(), 3u);
  text.text = "fc-";  // sample prefix, any case
  EXPECT_EQ(browse(text).rows.size(), 2u);
  text.text = "66_";  // LIKE wildcards are literal
  EXPECT_EQ(browse(text).rows.size(), 0u);

  BrowseFilter range;
  range.from = *UtcTime::parse("2026-10-02T11:00:00Z");
  range.to = *UtcTime::parse("2026-10-02T13:00:00Z");
  EXPECT_EQ(uuids(browse(range)), (std::vector<Uuid>{a4_, a3_, a2_}));
  BrowseFilter recent;
  recent.last_hours = 1.5;  // relative to the newest analysis (14:00)
  EXPECT_EQ(uuids(browse(recent)), (std::vector<Uuid>{a5_, a4_}));

  BrowseFilter tags;
  tags.exclude_tags = {"invalid"};
  EXPECT_EQ(browse(tags).rows.size(), 4u);
  EXPECT_EQ(browse(tags).total, 4);

  BrowseFilter repo;
  repo.repositories = {"FishCanyon"};
  EXPECT_EQ(uuids(browse(repo)), (std::vector<Uuid>{a2_, a1_}));
  BrowseFilter irr;
  irr.irradiations = {"NM-300"};
  irr.levels = {"A"};
  EXPECT_EQ(browse(irr).rows.size(), 3u);  // 77000 (pos 2) and 66573 (pos 1)
  BrowseFilter device;
  device.extract_devices = {"co2"};
  device.mass_spectrometers = {"jan"};
  device.identifiers = {"77000"};
  EXPECT_EQ(browse(device).rows.size(), 2u);
}

TEST_P(BrowseTest, FacetsIgnoreTheirOwnFilter) {
  BrowseFilter f;
  f.analysis_types = {"air"};
  auto types = store_->facet(BrowseFacet::AnalysisType, f);
  ASSERT_TRUE(types) << to_string(types.error());
  EXPECT_EQ(*types, (std::vector<std::string>{"air", "blank_unknown", "unknown"}));
  auto ids = store_->facet(BrowseFacet::Identifier, f);
  ASSERT_TRUE(ids);
  EXPECT_EQ(*ids, (std::vector<std::string>{"66574"}));
  EXPECT_EQ(*store_->facet(BrowseFacet::Sample, {}), (std::vector<std::string>{"FC-2"}));
  EXPECT_EQ(*store_->facet(BrowseFacet::Repository, {}), (std::vector<std::string>{"FishCanyon"}));
  EXPECT_TRUE(store_->facet(BrowseFacet::Repository, f)->empty());
  EXPECT_EQ(*store_->facet(BrowseFacet::PrincipalInvestigator, {}), (std::vector<std::string>{"Ross, J"}));
  EXPECT_EQ(*store_->facet(BrowseFacet::Level, {}), (std::vector<std::string>{"A"}));
}

TEST_P(BrowseTest, DetailAndBlobs) {
  auto d = store_->load_analysis_detail(a1_);
  ASSERT_TRUE(d) << to_string(d.error());
  ASSERT_TRUE(*d);
  EXPECT_EQ((*d)->row.summary.runid, "77000-01");
  EXPECT_EQ((*d)->extraction.extract_value, 5.0);
  EXPECT_EQ((*d)->analyst, "jross");
  ASSERT_EQ((*d)->isotopes.size(), 2u);
  EXPECT_EQ((*d)->isotopes[0].isotope, "Ar40");
  EXPECT_EQ((*d)->isotopes[0].detector, "H1");
  ASSERT_EQ((*d)->detectors.size(), 2u);
  EXPECT_EQ((*d)->detectors[1].detector, "H1");
  EXPECT_FALSE(*store_->load_analysis_detail(Uuid::v7()));

  const Bytes signal = series(1);
  const auto sha = blob_sha256(kCodecTv, signal);
  EXPECT_FALSE(*store_->load_blob(sha));  // not uploaded yet
  ASSERT_TRUE(store_->ingest(IngestItem{Uuid::v7(), {}, lab_.acquisition_client, BlobIngest{"f32le-tv/1", signal, 4}}));
  auto blob = store_->load_blob(sha);
  ASSERT_TRUE(blob) << to_string(blob.error());
  ASSERT_TRUE(*blob);
  EXPECT_EQ((*blob)->codec, "f32le-tv/1");
  EXPECT_EQ((*blob)->bytes, signal);
  EXPECT_EQ((*blob)->n_points, 4);
}

INSTANTIATE_TEST_SUITE_P(Engines, BrowseTest, ::testing::ValuesIn(engines()), [](const auto& p) { return p.param; });

}  // namespace
}  // namespace pychron::persistence::testing
