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
    pi_ = *store_->add_principal_investigator(c, {"Ross", "J", std::nullopt, std::nullopt, std::nullopt});
    project_ = *store_->add_project(c, {"Fish Canyon", pi_, std::nullopt});
    material_ = *store_->add_material(c, {"sanidine", "60-80", std::nullopt});
    sample_ = *store_->add_sample(c, {.name = "FC-2", .project = project_, .material = material_});
    ASSERT_TRUE(store_->add_extract_device(c, "co2"));
    const Uuid pos2 = *store_->add_irradiation_position(c, {lab_.level, 2, sample_, std::nullopt, {}, {}, std::nullopt});
    ASSERT_TRUE(store_->add_identifier(c, {"77000", "unknown", std::nullopt, std::nullopt, pos2, std::nullopt, std::nullopt}));
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

  // Caches an age for the analysis as the age sink would: one `age` row, in
  // Ma; a null value is an analysis cached as having no age.
  void cache_age(Uuid analysis, std::optional<double> age, std::optional<double> err) {
    const auto fp = pychron::sha256(std::string_view{"fingerprint of " + analysis.str()});
    auto ok = store_->put_derived(analysis, fp, "red-1", {DerivedRow{"age", age, err, std::string("Ma")}});
    ASSERT_TRUE(ok) << to_string(ok.error());
  }

  // a1 10 Ma, a2 20 Ma, a3 cached with no age, a4 (invalid) 15 Ma, a5 no row.
  void cache_ages() {
    cache_age(a1_, 10.0, 0.1);
    cache_age(a2_, 20.0, 0.2);
    cache_age(a3_, std::nullopt, std::nullopt);
    cache_age(a4_, 15.0, 0.15);
  }

  static BrowseFilter ages(std::optional<double> min, std::optional<double> max) {
    BrowseFilter f;
    f.age_min = min;
    f.age_max = max;
    return f;
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
  EXPECT_EQ(it->principal_investigator, "J Ross");
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
  project.principal_investigators = {"J Ross"};
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
  EXPECT_EQ(*store_->facet(BrowseFacet::PrincipalInvestigator, {}), (std::vector<std::string>{"J Ross"}));
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

TEST_P(BrowseTest, LatestChangeSeqIsTheEndOfTheLog) {
  auto latest = store_->latest_change_seq();
  ASSERT_TRUE(latest) << to_string(latest.error());
  EXPECT_GT(*latest, 0);
  auto page = store_->changes_since(*latest, 10);
  ASSERT_TRUE(page);
  EXPECT_TRUE(page->entries.empty());
  auto before = store_->changes_since(*latest - 1, 10);
  ASSERT_TRUE(before);
  ASSERT_EQ(before->entries.size(), 1u);
  EXPECT_EQ(before->entries[0].seq, *latest);
}

TEST_P(BrowseTest, AgeBounds) {
  cache_ages();
  EXPECT_EQ(uuids(browse(ages(12.0, std::nullopt))), (std::vector<Uuid>{a4_, a2_}));
  EXPECT_EQ(uuids(browse(ages(std::nullopt, 15.0))), (std::vector<Uuid>{a4_, a1_}));
  EXPECT_EQ(uuids(browse(ages(12.0, 18.0))), (std::vector<Uuid>{a4_}));
  EXPECT_EQ(uuids(browse(ages(10.0, 10.0))), (std::vector<Uuid>{a1_})) << "both ends are inclusive";
  // Every age there is: a3 (cached as having none) and a5 (never cached) are left out.
  EXPECT_EQ(uuids(browse(ages(-1e9, std::nullopt))), (std::vector<Uuid>{a4_, a2_, a1_}));
  EXPECT_EQ(uuids(browse(ages(std::nullopt, 1e9))), (std::vector<Uuid>{a4_, a2_, a1_}));
  EXPECT_TRUE(browse(ages(30.0, std::nullopt)).rows.empty());
  // A bound with the other filters.
  BrowseFilter unknowns = ages(12.0, std::nullopt);
  unknowns.analysis_types = {"unknown"};
  EXPECT_EQ(uuids(browse(unknowns)), (std::vector<Uuid>{a2_}));
}

TEST_P(BrowseTest, RowsCarryTheCachedAge) {
  cache_ages();
  const auto r = browse({});
  ASSERT_EQ(r.rows.size(), 5u);
  auto row = [](const BrowseResult& in, Uuid u) {
    const auto it = std::find_if(in.rows.begin(), in.rows.end(), [&](const BrowseRow& x) { return x.summary.uuid == u; });
    if (it == in.rows.end()) {
      ADD_FAILURE() << "no row for " << u.str();
      return BrowseRow{};
    }
    return *it;
  };
  EXPECT_EQ(row(r, a1_).age, 10.0);
  EXPECT_EQ(row(r, a1_).age_err, 0.1);
  EXPECT_EQ(row(r, a2_).age, 20.0);
  EXPECT_FALSE(row(r, a3_).age) << "cached as having no age";
  EXPECT_FALSE(row(r, a3_).age_err);
  EXPECT_FALSE(row(r, a5_).age) << "never cached";
  auto d = store_->load_analysis_detail(a2_);
  ASSERT_TRUE(d) << to_string(d.error());
  ASSERT_TRUE(*d);
  EXPECT_EQ((*d)->row.age, 20.0);
  EXPECT_EQ((*d)->row.age_err, 0.2);
  // A new fingerprint replaces the age: still one row for the analysis.
  const auto fp = pychron::sha256(std::string_view{"new inputs"});
  ASSERT_TRUE(store_->put_derived(a1_, fp, "red-1", {DerivedRow{"age", 11.0, 0.3, std::string("Ma")}}));
  const auto again = browse({});
  EXPECT_EQ(again.rows.size(), 5u);
  EXPECT_EQ(row(again, a1_).age, 11.0);
  EXPECT_EQ(row(again, a1_).age_err, 0.3);
}

TEST_P(BrowseTest, PagingAndTotalsUnderAnAgeBound) {
  cache_ages();
  auto page1 = store_->browse(BrowseRequest{ages(0.0, 100.0), 2, std::nullopt, true});
  ASSERT_TRUE(page1) << to_string(page1.error());
  EXPECT_EQ(uuids(*page1), (std::vector<Uuid>{a4_, a2_}));
  EXPECT_EQ(page1->total, 3);
  ASSERT_TRUE(page1->next);
  auto page2 = store_->browse(BrowseRequest{ages(0.0, 100.0), 2, page1->next, true});
  ASSERT_TRUE(page2) << to_string(page2.error());
  EXPECT_EQ(uuids(*page2), (std::vector<Uuid>{a1_}));
  EXPECT_EQ(page2->total, 3) << "the total is of every match, not of what is left";
  EXPECT_FALSE(page2->next);
}

TEST_P(BrowseTest, FacetsUnderAnAgeBound) {
  cache_ages();
  const BrowseFilter older = ages(12.0, std::nullopt);  // a2 (unknown, FC-2), a4 (air)
  auto types = store_->facet(BrowseFacet::AnalysisType, older);
  ASSERT_TRUE(types) << to_string(types.error());
  EXPECT_EQ(*types, (std::vector<std::string>{"air", "unknown"}));
  EXPECT_EQ(*store_->facet(BrowseFacet::Sample, older), (std::vector<std::string>{"FC-2"}));
  EXPECT_EQ(*store_->facet(BrowseFacet::Repository, older), (std::vector<std::string>{"FishCanyon"}));
  const BrowseFilter middle = ages(14.0, 16.0);  // a4 only
  EXPECT_EQ(*store_->facet(BrowseFacet::Identifier, middle), (std::vector<std::string>{"66574"}));
  EXPECT_TRUE(store_->facet(BrowseFacet::Sample, middle)->empty());
  EXPECT_TRUE(store_->facet(BrowseFacet::Repository, middle)->empty());
  EXPECT_TRUE(store_->facet(BrowseFacet::AnalysisType, ages(std::nullopt, 5.0))->empty());
}

INSTANTIATE_TEST_SUITE_P(Engines, BrowseTest, ::testing::ValuesIn(engines()), [](const auto& p) { return p.param; });

}  // namespace
}  // namespace pychron::persistence::testing
