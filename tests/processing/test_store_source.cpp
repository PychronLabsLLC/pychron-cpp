// StoreSource (data browsing and visualization design, section 9.2): browse,
// facets and loads over a real store, raw series from blobs, change-log
// refresh, saved edits, and the pure mappings. The store is
// store_source_fixture.hpp's. What the Ar-Ar loader reads of the references,
// the age cache and the backfill are tests/arar test_store_loader.cpp's.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <thread>

#include "pychron/processing/store_source.hpp"
#include "store_source_fixture.hpp"

namespace pychron::processing {
namespace {

namespace ps = pychron::persistence;
namespace fs = std::filesystem;

using StoreSourceTest = test::StoreSourceFixture;
using test::argon_parts;
using test::tv;

TEST_F(StoreSourceTest, LoadAtBookmarkReproducesTheOldIntercept) {
  StoreSource& src = source();
  auto before = src.load(unknown_.str());
  ASSERT_TRUE(before) << to_string(before.error());
  const double original = (*before)->find_isotope("Ar40")->intercept.value;
  const std::string old_head = (*before)->heads.at("intercepts");
  const ps::Uuid group = *store_->create_group(reducer(), "release:r@v1", {unknown_});
  // As a release pins: every head and every resolved reference at its revision.
  ps::BookmarkSpec spec{"release r v1", std::nullopt, std::nullopt, group};
  std::vector<ps::HeadInfo> entries = *store_->heads(unknown_);
  auto refs = store_->resolve_refs(unknown_, ps::RefPolicy{});
  ASSERT_TRUE(refs) << to_string(refs.error());
  ASSERT_EQ(refs->refs.size(), 5u);
  for (const auto& ref : refs->refs)
    entries.push_back(ps::HeadInfo{ref.ref_object, ps::Kind::RefValue, ref.revision, 1});
  spec.entries = entries;
  const ps::Uuid bookmark = *store_->create_bookmark(reducer(), spec);

  auto uow = *store_->begin(reducer());
  ps::InterceptRow row;
  row.isotope = "Ar40";
  row.detector = "H1";
  row.value = original + 50.0;
  ASSERT_TRUE(uow->add_revision(unknown_, ps::Kind::Intercepts, ps::Intercepts{row},
                                *store_->head(unknown_, ps::Kind::Intercepts)));
  auto outcome = uow->commit(ps::ChangesetKind::Reduction, "refit");
  ASSERT_TRUE(outcome && std::holds_alternative<ps::Committed>(*outcome));
  ASSERT_TRUE(src.refresh());

  auto now = src.load(unknown_.str());
  ASSERT_TRUE(now);
  EXPECT_NEAR((*now)->find_isotope("Ar40")->intercept.value, original + 50.0, 1e-9);
  auto at = src.load_at(unknown_.str(), bookmark.str());
  ASSERT_TRUE(at) << to_string(at.error());
  EXPECT_NEAR((*at)->find_isotope("Ar40")->intercept.value, original, 1e-9);
  EXPECT_EQ((*at)->heads.at("intercepts"), old_head);
  EXPECT_NE((*at)->input_fingerprint, (*now)->input_fingerprint);
  EXPECT_EQ((*at)->input_fingerprint, (*before)->input_fingerprint);
  auto none = src.load_at(unknown_.str(), ps::Uuid::v7().str());
  EXPECT_FALSE(none) << "a bookmark that does not pin the analysis";
}

TEST_F(StoreSourceTest, BrowsePagesNewestFirstWithCatalogColumns) {
  BrowseQuery q;
  q.limit = 1;
  auto page = source().browse(q);
  ASSERT_TRUE(page) << to_string(page.error());
  ASSERT_EQ(page->rows.size(), 1u);
  EXPECT_EQ(page->rows[0].uuid, air_.str());
  EXPECT_EQ(page->total, 2u);
  ASSERT_TRUE(page->next);
  q.after = page->next;
  auto page2 = source().browse(q);
  ASSERT_TRUE(page2) << to_string(page2.error());
  ASSERT_EQ(page2->rows.size(), 1u);
  const auto& s = page2->rows[0];
  EXPECT_EQ(s.uuid, unknown_.str());
  EXPECT_EQ(s.runid, "77000-01");
  EXPECT_EQ(s.sample, "FC-2");
  EXPECT_EQ(s.project, "Fish Canyon");
  EXPECT_EQ(s.principal_investigator, "J Ross");
  EXPECT_EQ(s.irradiation, "NM-300");
  EXPECT_EQ(s.level, "A");
  EXPECT_EQ(s.load, "load-1");
  EXPECT_EQ(s.extract_device, "co2");
  EXPECT_EQ(s.tag, "ok");
  EXPECT_EQ(s.extract_value, 4.5);
  EXPECT_DOUBLE_EQ(s.timestamp, static_cast<double>(ps::UtcTime::parse("2026-10-02T10:00:00Z")->micros) / 1e6);
  EXPECT_FALSE(page2->next);

  BrowseQuery types;
  types.analysis_types = {"air"};
  auto airs = source().browse(types);
  ASSERT_TRUE(airs);
  ASSERT_EQ(airs->rows.size(), 1u);
  EXPECT_EQ(airs->rows[0].analysis_type, "air");
  auto facet = source().facet(Facet::AnalysisType, types);
  ASSERT_TRUE(facet) << to_string(facet.error());
  EXPECT_EQ(*facet, (std::vector<std::string>{"air", "unknown"}));
  EXPECT_EQ(*source().facet(Facet::Sample, {}), (std::vector<std::string>{"FC-2"}));
  q.limit = 0;
  EXPECT_FALSE(source().browse(q));
}

TEST_F(StoreSourceTest, BrowsesByUuid) {
  BrowseQuery q;
  q.uuids = {unknown_.str(), air_.str()};
  q.exclude_tags = {};
  auto page = source().browse(q);
  ASSERT_TRUE(page) << to_string(page.error());
  ASSERT_EQ(page->rows.size(), 2u);
  EXPECT_EQ(page->rows[0].uuid, air_.str());
  EXPECT_EQ(page->rows[1].uuid, unknown_.str());
  EXPECT_EQ(page->total, 2u);

  q.uuids = {unknown_.str()};
  auto one = source().browse(q);
  ASSERT_TRUE(one) << to_string(one.error());
  ASSERT_EQ(one->rows.size(), 1u);
  EXPECT_EQ(one->rows[0].uuid, unknown_.str());
  auto types = source().facet(Facet::AnalysisType, q);
  ASSERT_TRUE(types) << to_string(types.error());
  EXPECT_EQ(*types, (std::vector<std::string>{"unknown"}));

  q.uuids = {"not-a-uuid"};
  auto bad = source().browse(q);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().kind, ErrorKind::Config);
  EXPECT_FALSE(source().facet(Facet::AnalysisType, q));
}

// What the experiment window's References dock asks after each saved run: the
// newest blanks (of every kind), or airs, of the session's spectrometer since
// a time.
TEST_F(StoreSourceTest, TheNewestReferencesOfOneSpectrometerSinceATime) {
  ASSERT_TRUE(store_->add_mass_spectrometer(acq_, {"felix", "helix", "f", std::nullopt}));
  const std::vector<std::pair<std::string, std::string>> specials{
      {"66601", "blank_unknown"}, {"66602", "blank_air"}, {"66603", "blank_cocktail"}, {"66604", "cocktail"}};
  for (const auto& [identifier, type] : specials) {
    ASSERT_TRUE(store_->add_identifier(
        acq_, {identifier, "special", type, std::nullopt, std::nullopt, std::nullopt, std::nullopt}));
  }
  const auto at = [](const std::string& ts) { return static_cast<double>(ps::UtcTime::parse(ts)->micros) / 1e6; };
  // jan, oldest first; the fixture's unknown (10:00 on the 2nd) and air (11:00) are before all of these
  const ps::Uuid b1 = ingest("66601", 1, "blank_unknown", "2026-10-03T01:00:00Z");
  const ps::Uuid a1 = ingest("66574", 2, "air", "2026-10-03T02:00:00Z");
  const ps::Uuid b2 = ingest("66602", 1, "blank_air", "2026-10-03T03:00:00Z");
  ingest("77000", 2, "unknown", "2026-10-03T04:00:00Z");
  const ps::Uuid b3 = ingest("66603", 1, "blank_cocktail", "2026-10-03T05:00:00Z");
  ingest("66604", 1, "cocktail", "2026-10-03T06:00:00Z");
  const ps::Uuid b4 = ingest("66601", 2, "blank_unknown", "2026-10-03T07:00:00Z");
  const ps::Uuid a2 = ingest("66574", 3, "air", "2026-10-03T08:00:00Z");
  const ps::Uuid b5 = ingest("66602", 2, "blank_air", "2026-10-03T09:00:00Z");
  // the other spectrometer's, newer than all of jan's
  ingest("66601", 3, "blank_unknown", "2026-10-03T10:00:00Z", "felix");
  ingest("66574", 4, "air", "2026-10-03T10:30:00Z", "felix");

  const auto uuids = [](const BrowsePage& page) {
    std::vector<std::string> out;
    out.reserve(page.rows.size());
    for (const auto& r : page.rows) out.push_back(r.uuid);
    return out;
  };

  BrowseQuery blanks;
  blanks.analysis_types = {"blank_unknown", "blank_air", "blank_cocktail", "blank_extraction_line", "blank_laser"};
  blanks.mass_spectrometers = {"jan"};
  blanks.from = at("2026-10-03T03:00:00Z");  // b2's own time: the edge is in
  blanks.limit = 3;
  auto page = source().browse(blanks);
  ASSERT_TRUE(page) << to_string(page.error());
  EXPECT_EQ(uuids(*page), (std::vector<std::string>{b5.str(), b4.str(), b3.str()}));
  for (const auto& r : page->rows) EXPECT_EQ(r.mass_spectrometer, "jan");
  ASSERT_TRUE(page->next);  // b2 is still to come
  blanks.limit = 50;
  page = source().browse(blanks);
  ASSERT_TRUE(page) << to_string(page.error());
  EXPECT_EQ(uuids(*page), (std::vector<std::string>{b5.str(), b4.str(), b3.str(), b2.str()}));
  EXPECT_FALSE(page->next);
  blanks.from.reset();
  page = source().browse(blanks);
  ASSERT_TRUE(page) << to_string(page.error());
  EXPECT_EQ(uuids(*page), (std::vector<std::string>{b5.str(), b4.str(), b3.str(), b2.str(), b1.str()}));

  BrowseQuery airs;
  airs.analysis_types = {"air"};
  airs.mass_spectrometers = {"jan"};
  airs.from = at("2026-10-03T00:00:00Z");
  airs.limit = 3;
  page = source().browse(airs);
  ASSERT_TRUE(page) << to_string(page.error());
  EXPECT_EQ(uuids(*page), (std::vector<std::string>{a2.str(), a1.str()}));
  EXPECT_FALSE(page->next);
}

TEST_F(StoreSourceTest, LoadAssemblesTheAnalysis) {
  auto loaded = source().load(unknown_.str());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  const Analysis& a = **loaded;
  EXPECT_EQ(a.runid, "77000-01");
  EXPECT_EQ(a.sample, "FC-2");
  EXPECT_EQ(a.position, "3");
  EXPECT_EQ(a.analyst, "jross");
  EXPECT_EQ(a.extraction.value, 4.5);
  EXPECT_EQ(a.extraction.units, "W");
  EXPECT_EQ(a.extraction.duration, 30.0);
  ASSERT_EQ(a.isotopes.size(), 5u);
  EXPECT_EQ(a.isotopes[0].key, "Ar40");
  EXPECT_EQ(a.isotopes[0].detector, "H1");
  EXPECT_EQ(a.isotopes[0].intercept, (Value{1000.0, 0.5}));
  EXPECT_EQ(a.isotopes[0].baseline, (Value{0.01, 0.001}));
  EXPECT_EQ(a.isotopes[0].blank, (Value{0.5, 0.05}));
  EXPECT_EQ(a.isotopes[0].blank_source, "preceding");
  EXPECT_EQ(a.isotopes[0].ic_factor, (Value{1.0, 0.0}));  // no H1 IC factor
  EXPECT_EQ(a.isotopes[0].n, 100);
  ASSERT_TRUE(a.isotopes[0].fit);
  EXPECT_EQ(a.isotopes[0].fit->kind, reduction::FitKind::Linear);
  EXPECT_EQ(a.isotopes[0].fit->error, reduction::ErrorType::Sd);
  EXPECT_TRUE(a.isotopes[0].fit->outliers.enabled);
  EXPECT_EQ(a.isotopes[0].fit->outliers.iterations, 2);
  EXPECT_EQ(a.isotopes[0].fit->outliers.std_devs, 2.5);
  ASSERT_TRUE(a.isotopes[0].baseline_fit);
  EXPECT_EQ(a.isotopes[0].baseline_fit->kind, reduction::FitKind::Average);
  EXPECT_EQ(a.isotopes[1].ic_factor, (Value{1.02, 0.001}));
  EXPECT_EQ(a.isotopes[4].key, "Ar36");
  EXPECT_EQ(a.isotopes[4].intercept.value, 0.4);  // manual
  EXPECT_EQ(a.gains.at("H1"), 1.02);              // used at acquisition
  EXPECT_EQ(a.gains.at("L2"), 0.98);              // from the gains reference
  EXPECT_EQ(a.deflections.at("H1"), 12.0);
  EXPECT_EQ(a.environmentals.at("lab_temperature"), 21.5);
  EXPECT_EQ(a.environmentals.at("lab_humidity"), 33.0);
  EXPECT_FALSE(a.environmentals.count("note"));
  ASSERT_EQ(a.peak_centers.size(), 1u);
  EXPECT_EQ(a.peak_centers[0].center, 5.123);

  // Without a loader the analysis is neutral: no domain data.
  EXPECT_FALSE(a.extension);
  // Report metadata: the sample's catalog row, the flux monitor (the reactor
  // is the Ar-Ar loader's: tests/arar test_store_loader.cpp).
  EXPECT_EQ(a.sample_info.latitude, 37.75);
  EXPECT_EQ(a.sample_info.longitude, -106.9);
  EXPECT_EQ(a.sample_info.elevation, 2850.0);
  EXPECT_EQ(a.sample_info.lithology, "ash-flow tuff");
  EXPECT_EQ(a.sample_info.igsn, "IEFC20001");
  EXPECT_EQ(a.monitor.name, "FC-2");
  EXPECT_EQ(a.monitor.material, "sanidine");
  EXPECT_EQ(a.monitor.age, (Value{28.201, 0.023}));

  // The air has no irradiation position, hence no catalog sample or monitor.
  auto air = source().load(air_.str());
  ASSERT_TRUE(air) << to_string(air.error());
  EXPECT_FALSE((*air)->sample_info.latitude);  // "air" is no catalog sample
  EXPECT_TRUE((*air)->monitor.name.empty());

  // Cached until something changes.
  EXPECT_EQ(source().load(unknown_.str())->get(), loaded->get());
  EXPECT_FALSE(source().load("not-a-uuid"));
  EXPECT_FALSE(source().load(ps::Uuid::v7().str()));
}

// The sample of an analysis is the one of that name, not whichever of the
// samples containing the name a capped search happens to return. A lab's
// catalog has hundreds of names with a monitor's name inside them.
TEST_F(StoreSourceTest, TheSampleIsFoundAmongManyWhoseNamesContainIts) {
  for (int i = 0; i < 510; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "A%03d FC-2", i);  // sorts before "FC-2"
    ASSERT_TRUE(store_->add_sample(acq_, {.name = name, .project = project_, .material = material_, .lat = 1.0, .lon = 2.0}));
  }
  auto loaded = source().load(unknown_.str());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  EXPECT_EQ((*loaded)->sample, "FC-2");
  EXPECT_EQ((*loaded)->sample_info.latitude, 37.75);
  EXPECT_EQ((*loaded)->sample_info.igsn, "IEFC20001");
}

// A name that only differs in case, or that is a longer name, is another sample.
TEST_F(StoreSourceTest, ASampleWithASimilarNameIsNotTaken) {
  ASSERT_TRUE(store_->add_sample(acq_, {.name = "FC-2b", .project = project_, .material = material_, .lat = 9.0, .lon = 9.0}));
  ASSERT_TRUE(store_->add_sample(acq_, {.name = "AFC-2", .project = project_, .material = material_, .lat = 8.0, .lon = 8.0}));
  auto loaded = source().load(unknown_.str());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  EXPECT_EQ((*loaded)->sample_info.latitude, 37.75);
}

// One name, a sample in every project that uses it (a monitor is entered
// under each): the analysis gets the one it was measured on.
TEST_F(StoreSourceTest, TheSampleIsItsOwnAmongNamesakes) {
  const auto pi = *store_->add_principal_investigator(acq_, {"Other", "P", std::nullopt, std::nullopt, std::nullopt});
  for (int i = 0; i < 40; ++i) {
    const auto project = *store_->add_project(acq_, {"Project " + std::to_string(i), pi, std::nullopt});
    ASSERT_TRUE(store_->add_sample(acq_, {.name = "FC-2", .project = project, .material = material_, .lat = 1.0 + i, .lon = 2.0}));
  }
  auto loaded = source().load(unknown_.str());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  EXPECT_EQ((*loaded)->sample_info.latitude, 37.75);
  EXPECT_EQ((*loaded)->sample_info.lithology, "ash-flow tuff");
}

TEST_F(StoreSourceTest, LoadRawDecodesUploadedBlobsAndSlicesWindows) {
  auto raw = source().load_raw(unknown_.str());
  ASSERT_TRUE(raw) << to_string(raw.error());
  // The baseline blob was never uploaded.
  ASSERT_EQ(raw->series.size(), 1u);
  const auto* s = raw->find(SeriesKind::Signal, "Ar40");
  ASSERT_TRUE(s);
  EXPECT_EQ(s->detector, "H1");
  EXPECT_EQ(s->t, (std::vector<double>{1, 2, 3}));  // [1, 4) of 0..4
  EXPECT_EQ(s->v, (std::vector<double>{101, 102, 103}));
  EXPECT_FALSE(source().load_raw(ps::Uuid::v7().str()));
}

TEST_F(StoreSourceTest, LoadCarriesTheInputFingerprint) {
  const std::string version = "arar-1";  // the fixture's StoreSourceOptions::reduction_version
  auto& src = source();
  auto loaded = src.load(unknown_.str());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  const std::string first = (*loaded)->input_fingerprint;
  EXPECT_EQ(first.size(), 64u);
  EXPECT_EQ(first, to_hex(*store_->input_fingerprint(unknown_, version)));

  // A new head is a new input.
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(unknown_, ps::Kind::Tags, ps::TagValue{"invalid", std::nullopt, std::nullopt},
                                *store_->head(unknown_, ps::Kind::Tags)));
  ASSERT_TRUE(uow->commit(ps::ChangesetKind::Reduction, "tag"));
  ASSERT_TRUE(src.refresh());
  auto again = src.load(unknown_.str());
  ASSERT_TRUE(again);
  EXPECT_NE((*again)->input_fingerprint, first);
  EXPECT_EQ((*again)->input_fingerprint, to_hex(*store_->input_fingerprint(unknown_, version)));

  // Another analysis has its own.
  auto air = src.load(air_.str());
  ASSERT_TRUE(air);
  EXPECT_NE((*air)->input_fingerprint, (*again)->input_fingerprint);
}

TEST_F(StoreSourceTest, RefreshSeesNewAnalysesAndNewRevisions) {
  auto& src = source();
  const auto g0 = src.generation();
  ASSERT_TRUE(src.refresh());
  EXPECT_EQ(src.generation(), g0);  // nothing since open

  auto before = src.load(unknown_.str());
  ASSERT_TRUE(before);
  EXPECT_EQ((*before)->tag, "ok");
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(unknown_, ps::Kind::Tags, ps::TagValue{"invalid", std::nullopt, std::nullopt},
                                *store_->head(unknown_, ps::Kind::Tags)));
  ASSERT_TRUE(uow->commit(ps::ChangesetKind::Reduction, "tag"));
  const auto third = ingest("77000", 2, "unknown", "2026-10-02T12:00:00Z");

  ASSERT_TRUE(src.refresh());
  EXPECT_GT(src.generation(), g0);
  auto after = src.load(unknown_.str());
  ASSERT_TRUE(after);
  EXPECT_EQ((*after)->tag, "invalid");
  BrowseQuery q;  // excludes "invalid" by default
  auto page = src.browse(q);
  ASSERT_TRUE(page);
  ASSERT_EQ(page->rows.size(), 2u);
  EXPECT_EQ(page->rows[0].uuid, third.str());
  EXPECT_EQ(page->rows[1].uuid, air_.str());
  const auto g1 = src.generation();
  ASSERT_TRUE(src.refresh());
  EXPECT_EQ(src.generation(), g1);
}

TEST_F(StoreSourceTest, ConcurrentLoadsFromManyThreads) {
  auto& src = source();
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i)
    threads.emplace_back([&, i] {
      for (int k = 0; k < 5; ++k) {
        auto a = src.load((i + k) % 2 ? unknown_.str() : air_.str());
        auto raw = src.load_raw(unknown_.str());
        auto page = src.browse(BrowseQuery{});
        if (a && raw && page && page->rows.size() == 2) ++ok;
      }
    });
  for (auto& t : threads) t.join();
  EXPECT_EQ(ok.load(), 40);
}

// The processing bridge refreshes the source on its worker while a window's
// job may still load on another thread: refresh is safe against both.
TEST_F(StoreSourceTest, ARefreshWhileOtherThreadsReadIsSafe) {
  auto& src = source();
  const auto g0 = src.generation();
  std::atomic<bool> go{false};
  std::atomic<int> failed{0};
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int i = 0; i < 4; ++i)
    readers.emplace_back([&, i] {
      while (!go) std::this_thread::yield();
      for (int k = 0; k < 10; ++k) {
        if (!src.load((i + k) % 2 ? unknown_.str() : air_.str())) ++failed;
        if (!src.browse(BrowseQuery{})) ++failed;
      }
    });
  std::thread refresher([&] {
    while (!go) std::this_thread::yield();
    for (int k = 0; k < 10; ++k)
      if (!src.refresh()) ++failed;
  });
  (void)ingest("77000", 2, "unknown", "2026-10-02T13:00:00Z");  // something for a refresh to find
  go = true;
  for (auto& t : readers) t.join();
  refresher.join();
  EXPECT_EQ(failed.load(), 0);
  ASSERT_TRUE(src.refresh());
  EXPECT_GT(src.generation(), g0);
  EXPECT_EQ(src.browse(BrowseQuery{})->rows.size(), 3u);
}

TEST_F(StoreSourceTest, OpenFailsForABadUrlAndHidesPasswords) {
  EXPECT_FALSE(StoreSource::open(ps::StoreConfig{"mysql://nope", false}));
  EXPECT_EQ(source().name(), "store:" + url_);
  EXPECT_EQ(redact_password("postgresql://me:s3cret@db:5432/pychron?sslmode=require"),
            "postgresql://me:***@db:5432/pychron?sslmode=require");
  EXPECT_EQ(redact_password("postgresql://me@db/pychron"), "postgresql://me@db/pychron");
  EXPECT_EQ(redact_password("postgresql://db:5432/x@y"), "postgresql://db:5432/x@y");
}

TEST_F(StoreSourceTest, SavedFitEditsBecomeTheHeadAndShowInHistory) {
  auto& src = source();
  ASSERT_TRUE(src.revisions());
  auto loaded = src.load(unknown_.str());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  const std::string base = (*loaded)->heads.at("intercepts");
  auto raw = src.load_raw(unknown_.str());
  ASSERT_TRUE(raw);
  reduction::FitSpec avg;
  avg.kind = reduction::FitKind::Average;
  auto edits = apply_fit_edits(**loaded, *raw, {FitEdit{SeriesKind::Signal, "Ar40", avg, {0}}});
  ASSERT_TRUE(edits) << edits.error().what;
  const std::string message = describe_fit_edits(**loaded, edits->fits);
  auto saved = src.revisions()->save_fits(unknown_.str(), (*loaded)->heads, edits->fits, message);
  ASSERT_TRUE(saved) << to_string(saved.error());
  ASSERT_TRUE(saved->saved) << saved->conflict;

  // The window [1, 4) of the blob is 101, 102, 103; leaving out the first
  // point averages 102 and 103.
  auto after = src.load(unknown_.str());
  ASSERT_TRUE(after);
  EXPECT_NE(after->get(), loaded->get());
  const IsotopeData* ar40 = (*after)->find_isotope("Ar40");
  ASSERT_TRUE(ar40);
  EXPECT_DOUBLE_EQ(ar40->intercept.value, 102.5);
  EXPECT_EQ(ar40->fit->kind, reduction::FitKind::Average);
  EXPECT_EQ(ar40->user_excluded, (std::vector<std::size_t>{0}));
  EXPECT_EQ(ar40->n, 2);
  EXPECT_EQ((*after)->heads.at("intercepts"), saved->revisions.at("intercepts"));
  EXPECT_EQ((*after)->find_isotope("Ar39")->intercept.value, 100.0);  // untouched rows stay

  auto history = src.revisions()->history(unknown_.str(), RevisionKind::Intercepts);
  ASSERT_TRUE(history) << to_string(history.error());
  ASSERT_EQ(history->size(), 2u);
  const auto& newest = (*history)[0];
  EXPECT_EQ(newest.id, saved->revisions.at("intercepts"));
  EXPECT_EQ(newest.parent, base);
  EXPECT_TRUE(newest.head);
  EXPECT_FALSE((*history)[1].head);
  EXPECT_EQ(newest.author, "tester");
  EXPECT_EQ(newest.host, "test-host");
  EXPECT_EQ(newest.changeset_kind, "reduction");
  EXPECT_EQ(newest.message, "<ISOEVO> Ar40 linear -> average SEM no outlier filter 1 excluded");
  EXPECT_EQ((*history)[1].changeset_kind, "collection");
  EXPECT_EQ((*history)[1].author, "jross");
  EXPECT_GT(newest.seq, (*history)[1].seq);

  auto before_table = src.revisions()->revision_table(base);
  auto after_table = src.revisions()->revision_table(saved->revisions.at("intercepts"));
  ASSERT_TRUE(before_table && after_table);
  const auto diff = diff_revisions(*before_table, *after_table);
  EXPECT_EQ(diff.changed_rows(), 1);
  const auto row = [&](const std::string& key) -> const DiffRow& {
    return *std::find_if(diff.rows.begin(), diff.rows.end(), [&](const DiffRow& d) { return d.key == key; });
  };
  const auto col = [&](const std::string& c) {
    return static_cast<std::size_t>(std::find(diff.columns.begin(), diff.columns.end(), c) - diff.columns.begin());
  };
  const DiffRow& ar40_row = row("Ar40");
  EXPECT_EQ(ar40_row.state, DiffState::Changed);
  EXPECT_EQ(ar40_row.before[col("fit")], "Linear");
  EXPECT_EQ(ar40_row.after[col("fit")], "average");
  EXPECT_EQ(ar40_row.after[col("user excluded")], "[0]");
  EXPECT_EQ(ar40_row.after[col("n")], "3");
  EXPECT_EQ(ar40_row.after[col("fn")], "2");
  EXPECT_EQ(row("Ar36").before[col("manual value")], "0.4");
  EXPECT_EQ(row("Ar36").state, DiffState::Same);

  // A second save on the old head loses the compare-and-swap.
  auto stale = src.revisions()->save_fits(unknown_.str(), (*loaded)->heads, edits->fits, message);
  ASSERT_TRUE(stale) << to_string(stale.error());
  EXPECT_FALSE(stale->saved);
  EXPECT_NE(stale->conflict.find("<ISOEVO>"), std::string::npos) << stale->conflict;
  EXPECT_EQ(src.revisions()->history(unknown_.str(), RevisionKind::Intercepts)->size(), 2u);

  EXPECT_FALSE(src.revisions()->save_fits(unknown_.str(), (*loaded)->heads, {}, message));
  EXPECT_FALSE(src.revisions()->save_fits(unknown_.str(), {{"intercepts", "nope"}}, edits->fits, message));
  EditedFit unknown_key = edits->fits[0];
  unknown_key.key = "Ar99";
  EXPECT_FALSE(src.revisions()->save_fits(unknown_.str(), (*after)->heads, {unknown_key}, message));
}

TEST_F(StoreSourceTest, BaselineAndInterceptEditsSaveInOneChangeset) {
  // Upload the H1 baseline blob (0, 1, 2, 3, 4 at t = 0..4).
  ASSERT_TRUE(store_->ingest(ps::IngestItem{ps::Uuid::v7(), {}, acq_, ps::BlobIngest{"f32le-tv/1", tv(0), 5}}));
  auto& src = source();
  auto loaded = src.load(unknown_.str());
  ASSERT_TRUE(loaded);
  auto raw = src.load_raw(unknown_.str());
  ASSERT_TRUE(raw);
  ASSERT_TRUE(raw->find(SeriesKind::Baseline, "H1"));
  reduction::FitSpec lin, avg;
  lin.kind = reduction::FitKind::Linear;
  avg.kind = reduction::FitKind::Average;
  auto edits = apply_fit_edits(**loaded, *raw,
                               {FitEdit{SeriesKind::Baseline, "H1", lin, {4}}, FitEdit{SeriesKind::Signal, "Ar40", avg, {}}});
  ASSERT_TRUE(edits) << edits.error().what;
  auto saved = src.revisions()->save_fits(unknown_.str(), (*loaded)->heads, edits->fits,
                                          describe_fit_edits(**loaded, edits->fits));
  ASSERT_TRUE(saved) << to_string(saved.error());
  ASSERT_TRUE(saved->saved) << saved->conflict;
  ASSERT_EQ(saved->revisions.size(), 2u);

  auto after = src.load(unknown_.str());
  ASSERT_TRUE(after);
  const IsotopeData* ar40 = (*after)->find_isotope("Ar40");
  EXPECT_NEAR(ar40->baseline.value, 0.0, 1e-9);  // the line through 0..3 at t = 0
  EXPECT_EQ(ar40->baseline_fit->kind, reduction::FitKind::Linear);
  EXPECT_EQ(ar40->baseline_user_excluded, (std::vector<std::size_t>{4}));
  EXPECT_NEAR(ar40->intercept.value, 102.0, 1e-9);  // average of 101, 102, 103
  EXPECT_EQ((*after)->heads.at("baselines"), saved->revisions.at("baselines"));
  EXPECT_EQ((*after)->heads.at("intercepts"), saved->revisions.at("intercepts"));

  auto bh = src.revisions()->history(unknown_.str(), RevisionKind::Baselines);
  auto ih = src.revisions()->history(unknown_.str(), RevisionKind::Intercepts);
  ASSERT_TRUE(bh && ih);
  ASSERT_EQ(bh->size(), 2u);
  EXPECT_EQ((*bh)[0].seq, (*ih)[0].seq);  // one changeset
  EXPECT_EQ((*bh)[0].message, "<ISOEVO> H1 baseline average -> linear 1 excluded, Ar40 linear -> average SEM no outlier filter");

  // A baseline edit on a stale baselines head conflicts and writes nothing,
  // not even the intercepts revision staged with it.
  auto stale = src.revisions()->save_fits(unknown_.str(), (*loaded)->heads, edits->fits, "stale");
  ASSERT_TRUE(stale);
  EXPECT_FALSE(stale->saved);
  EXPECT_EQ(src.revisions()->history(unknown_.str(), RevisionKind::Intercepts)->size(), 2u);
  auto no_head = (*after)->heads;
  no_head.erase("baselines");
  EXPECT_FALSE(src.revisions()->save_fits(unknown_.str(), no_head, edits->fits, "x"));
}

TEST_F(StoreSourceTest, RestoreMovesTheHeadBackWithoutANewRevision) {
  auto& src = source();
  auto loaded = src.load(unknown_.str());
  auto raw = src.load_raw(unknown_.str());
  ASSERT_TRUE(loaded && raw);
  const std::string root = (*loaded)->heads.at("intercepts");
  reduction::FitSpec avg;
  avg.kind = reduction::FitKind::Average;
  auto edits = apply_fit_edits(**loaded, *raw, {FitEdit{SeriesKind::Signal, "Ar40", avg, {}}});
  ASSERT_TRUE(edits);
  auto saved = src.revisions()->save_fits(unknown_.str(), (*loaded)->heads, edits->fits, "edit");
  ASSERT_TRUE(saved && saved->saved);
  const std::string edited = saved->revisions.at("intercepts");
  EXPECT_NEAR((*src.load(unknown_.str()))->find_isotope("Ar40")->intercept.value, 102.0, 1e-9);

  auto restored = src.revisions()->restore_revision(unknown_.str(), RevisionKind::Intercepts, edited, root,
                                                    "<ROLLBACK> intercepts");
  ASSERT_TRUE(restored) << to_string(restored.error());
  ASSERT_TRUE(restored->saved) << restored->conflict;
  EXPECT_EQ(restored->revisions.at("intercepts"), root);
  auto back = src.load(unknown_.str());
  ASSERT_TRUE(back);
  EXPECT_EQ((*back)->find_isotope("Ar40")->intercept, (Value{1000.0, 0.5}));
  EXPECT_EQ((*back)->find_isotope("Ar40")->fit->kind, reduction::FitKind::Linear);
  EXPECT_EQ((*back)->heads.at("intercepts"), root);

  auto history = src.revisions()->history(unknown_.str(), RevisionKind::Intercepts);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 2u);  // no revision written
  EXPECT_FALSE((*history)[0].head);
  EXPECT_TRUE((*history)[1].head);

  // And forward again; then a stale restore conflicts.
  auto forward = src.revisions()->restore_revision(unknown_.str(), RevisionKind::Intercepts, root, edited, "redo");
  ASSERT_TRUE(forward && forward->saved);
  auto stale = src.revisions()->restore_revision(unknown_.str(), RevisionKind::Intercepts, root, edited, "again");
  ASSERT_TRUE(stale) << to_string(stale.error());
  EXPECT_FALSE(stale->saved);
  EXPECT_FALSE(stale->conflict.empty());
  EXPECT_FALSE(src.revisions()->restore_revision(unknown_.str(), RevisionKind::Intercepts, edited, edited, "same"));
  // Another analysis' revision cannot become this one's head.
  const std::string air_root = (*src.load(air_.str()))->heads.at("intercepts");
  EXPECT_FALSE(src.revisions()->restore_revision(unknown_.str(), RevisionKind::Intercepts, edited, air_root, "x"));
  EXPECT_EQ((*src.load(unknown_.str()))->heads.at("intercepts"), edited);
}

TEST_F(StoreSourceTest, ReferenceFitsSaveForManyAnalysesInOneChangeset) {
  auto& src = source();
  auto unknown = src.load(unknown_.str());
  auto air = src.load(air_.str());
  ASSERT_TRUE(unknown && air);
  ReferenceFitSet blanks;
  blanks.target = ReferenceFitTarget::Blanks;
  for (const auto& a : {*unknown, *air}) {
    AnalysisReferenceFits f;
    f.uuid = a->uuid;
    f.runid = a->runid;
    f.heads = a->heads;
    ReferenceRowFit ar40{"Ar40", {0.7, 0.07}, ReferenceFitKind::Linear, ReferenceErrorKind::Sd, "", std::nullopt, false,
                         {{air_.str(), "66574-01", false}, {"not-a-uuid", "bu-1", true}}};
    ReferenceRowFit ar39 = ar40;  // no Ar39 blank row yet: appended
    ar39.key = "Ar39";
    ar39.value = {0.02, 0.002};
    ar39.fit = ReferenceFitKind::Preceding;
    f.rows = {ar40, ar39};
    blanks.analyses.push_back(f);
  }
  auto saved = src.revisions()->save_reference_fits(blanks);
  ASSERT_TRUE(saved) << to_string(saved.error());
  ASSERT_TRUE(saved->saved) << saved->conflict;
  ASSERT_EQ(saved->revisions.size(), 2u);

  auto after = src.load(unknown_.str());
  ASSERT_TRUE(after);
  EXPECT_EQ((*after)->find_isotope("Ar40")->blank, (Value{0.7, 0.07}));
  EXPECT_EQ((*after)->find_isotope("Ar40")->blank_source, "linear");
  EXPECT_EQ((*after)->find_isotope("Ar39")->blank, (Value{0.02, 0.002}));
  EXPECT_TRUE((*after)->find_isotope("Ar40")->blank_reviewed);
  EXPECT_FALSE((*after)->find_isotope("Ar40")->ic_reviewed);
  EXPECT_EQ((*after)->heads.at("blanks"), saved->revisions.at(unknown_.str()));
  EXPECT_EQ((*src.load(air_.str()))->find_isotope("Ar40")->blank, (Value{0.7, 0.07}));
  auto uh = src.revisions()->history(unknown_.str(), RevisionKind::Blanks);
  auto ah = src.revisions()->history(air_.str(), RevisionKind::Blanks);
  ASSERT_TRUE(uh && ah);
  ASSERT_EQ(uh->size(), 2u);
  EXPECT_EQ((*uh)[0].seq, (*ah)[0].seq);  // one changeset
  EXPECT_EQ((*uh)[0].message, "<BLANKS> fits=Ar40(linear),Ar39(preceding)");
  const auto table = *src.revisions()->revision_table((*uh)[0].id);
  const auto refs_col = static_cast<std::size_t>(
      std::find(table.columns.begin(), table.columns.end(), "references") - table.columns.begin());
  const auto row40 = std::find_if(table.rows.begin(), table.rows.end(), [](const auto& r) { return r.key == "Ar40"; });
  ASSERT_NE(row40, table.rows.end());
  EXPECT_EQ(row40->cells[refs_col], "66574-01, !bu-1");

  // A save on the old heads conflicts for both and writes nothing.
  auto stale = src.revisions()->save_reference_fits(blanks);
  ASSERT_TRUE(stale);
  EXPECT_FALSE(stale->saved);
  EXPECT_EQ(stale->conflict.rfind("2 of 2 analyses changed first", 0), 0u) << stale->conflict;
  EXPECT_EQ(src.revisions()->history(unknown_.str(), RevisionKind::Blanks)->size(), 2u);

  // IC factors: the AX row updated, an H1 row added.
  ReferenceFitSet ic;
  ic.target = ReferenceFitTarget::IcFactors;
  AnalysisReferenceFits f;
  f.uuid = unknown_.str();
  f.runid = (*after)->runid;
  f.heads = (*after)->heads;
  f.rows = {{"AX", {1.03, 0.004}, ReferenceFitKind::Average, ReferenceErrorKind::Sem, "H1", 295.5, false, {}},
            {"H1", {1.0, 0.0}, ReferenceFitKind::Average, ReferenceErrorKind::Sem, "H1", 295.5, false, {}}};
  ic.analyses = {f};
  auto ic_saved = src.revisions()->save_reference_fits(ic);
  ASSERT_TRUE(ic_saved && ic_saved->saved);
  auto with_ic = src.load(unknown_.str());
  EXPECT_EQ((*with_ic)->find_isotope("Ar39")->ic_factor, (Value{1.03, 0.004}));
  EXPECT_TRUE((*with_ic)->find_isotope("Ar39")->ic_reviewed);
  EXPECT_EQ((*with_ic)->find_isotope("Ar40")->ic_factor, (Value{1.0, 0.0}));
  EXPECT_EQ(src.revisions()->history(unknown_.str(), RevisionKind::IcFactors)->front().message,
            "<ICFactor> fits=AX(average),H1(average)");

  EXPECT_FALSE(src.revisions()->save_reference_fits(ReferenceFitSet{}));
  ic.analyses[0].heads.erase("icfactors");
  EXPECT_FALSE(src.revisions()->save_reference_fits(ic));
}

TEST(StoreSourceMapping, ReferenceFitRows) {
  ps::BlankRow old;
  old.isotope = "Ar40";
  old.value = 1;
  old.manual.use_value = true;
  old.extra_json = R"({"keep": 1})";
  const auto rows = apply_blank_fits(
      {old}, {{"Ar40", {2, 0.2}, ReferenceFitKind::BracketingInterpolate, ReferenceErrorKind::Msem, "", std::nullopt, false,
               {{ps::Uuid::v7().str(), "bu-1", false}, {"", "", true}}},
              {"Ar36", {0.1, 0.01}, ReferenceFitKind::Average, ReferenceErrorKind::Sem, "", std::nullopt, false, {}}});
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].value, 2.0);
  EXPECT_EQ(rows[0].fit, "bracketing_interpolate");
  EXPECT_EQ(rows[0].error_type, "MSEM");
  EXPECT_TRUE(rows[0].reviewed);
  EXPECT_FALSE(rows[0].manual.use_value);
  EXPECT_EQ(rows[0].extra_json, old.extra_json);
  ASSERT_EQ(rows[0].references.size(), 2u);
  EXPECT_TRUE(rows[0].references[0].ref_analysis);
  EXPECT_EQ(rows[0].references[0].record_id, "bu-1");
  EXPECT_FALSE(rows[0].references[1].ref_analysis);
  EXPECT_FALSE(rows[0].references[1].record_id);
  EXPECT_TRUE(rows[0].references[1].exclude);
  EXPECT_EQ(rows[0].references[1].ordinal, 1);
  EXPECT_EQ(rows[1].isotope, "Ar36");
  const auto ics = apply_icfactor_fits({}, {{"CDD", {1.01, 0.001}, ReferenceFitKind::Linear, ReferenceErrorKind::Sem,
                                             "H1", 295.5, false, {}}});
  ASSERT_EQ(ics.size(), 1u);
  EXPECT_EQ(ics[0].detector, "CDD");
  EXPECT_EQ(ics[0].reference_detector, "H1");
  EXPECT_EQ(ics[0].standard_ratio, 295.5);
  EXPECT_EQ(ics[0].fit, "linear");
  EXPECT_FALSE(ics[0].source_correction);
  ReferenceRowFit source{"AX", {0.99, 0.001}, ReferenceFitKind::Average, ReferenceErrorKind::Sem, "H1", 295.5, true, {}};
  const auto corrected = apply_icfactor_fits(ics, {source});
  ASSERT_EQ(corrected.size(), 2u);
  EXPECT_TRUE(corrected[1].source_correction);
  EXPECT_FALSE(corrected[1].discrimination);
}

TEST_F(StoreSourceTest, BatchIsotopeRefitsSaveInOneChangeset) {
  auto& src = source();
  Dataset d;
  for (const auto& id : {unknown_, air_})
    d.mutable_items().push_back(DatasetItem{reduce_analysis(*src.load(id.str())), {}, {}});
  Options o(isotope_evolution_fit_schema());
  auto rows = o.rows("isotopes");
  ASSERT_TRUE(rows[0].set("fit", std::string("average")));
  ASSERT_TRUE(o.set_rows("isotopes", rows));
  auto fig = build_isotope_evolution_fits(d, o, [&](const std::string& uuid) { return src.load_raw(uuid); });
  ASSERT_TRUE(fig) << fig.error().what;
  ASSERT_EQ(fig->fits.analyses.size(), 2u);  // only Ar40 has a raw signal
  EXPECT_FALSE(fig->fits.warnings.empty());
  auto saved = src.revisions()->save_isotope_fits(fig->fits);
  ASSERT_TRUE(saved) << to_string(saved.error());
  ASSERT_TRUE(saved->saved) << saved->conflict;
  ASSERT_EQ(saved->revisions.size(), 2u);
  for (const auto& id : {unknown_, air_}) {
    auto a = src.load(id.str());
    ASSERT_TRUE(a);
    EXPECT_NEAR((*a)->find_isotope("Ar40")->intercept.value, 102.0, 1e-9);  // average of the window 101..103
    EXPECT_EQ((*a)->find_isotope("Ar40")->fit->kind, reduction::FitKind::Average);
    EXPECT_TRUE((*a)->find_isotope("Ar40")->intercept_reviewed);
    EXPECT_FALSE((*a)->find_isotope("Ar39")->intercept_reviewed);
    EXPECT_EQ((*a)->heads.at("intercepts"), saved->revisions.at(id.str() + "/intercepts"));
  }
  auto uh = src.revisions()->history(unknown_.str(), RevisionKind::Intercepts);
  auto ah = src.revisions()->history(air_.str(), RevisionKind::Intercepts);
  ASSERT_TRUE(uh && ah);
  EXPECT_EQ((*uh)[0].seq, (*ah)[0].seq);
  EXPECT_EQ((*uh)[0].message, "<ISOEVO> refit Ar40(average)");
  // Saving the same refits again: their heads moved, nothing is written.
  auto stale = src.revisions()->save_isotope_fits(fig->fits);
  ASSERT_TRUE(stale);
  EXPECT_FALSE(stale->saved);
  EXPECT_EQ(stale->conflict.rfind("2 of 2 analyses changed first", 0), 0u) << stale->conflict;
  EXPECT_EQ(src.revisions()->history(unknown_.str(), RevisionKind::Intercepts)->size(), 2u);
  EXPECT_FALSE(src.revisions()->save_isotope_fits(IsotopeFitSet{}));
}

TEST_F(StoreSourceTest, BatchBaselineRefitsSaveBaselinesRevisions) {
  // The H1 baseline blob: 0, 1, 2, 3, 4.
  ASSERT_TRUE(store_->ingest(ps::IngestItem{ps::Uuid::v7(), {}, acq_, ps::BlobIngest{"f32le-tv/1", tv(0), 5}}));
  auto& src = source();
  Dataset d;
  d.mutable_items().push_back(DatasetItem{reduce_analysis(*src.load(unknown_.str())), {}, {}});
  Options o(isotope_evolution_fit_schema());
  auto rows = o.rows("isotopes");
  rows.resize(2);
  ASSERT_TRUE(rows[0].set("fit", std::string("average")));  // Ar40 signal
  ASSERT_TRUE(rows[1].set("series", std::string("baseline")));
  ASSERT_TRUE(rows[1].set("isotope", std::string("H1")));
  ASSERT_TRUE(rows[1].set("fit", std::string("average")));
  ASSERT_TRUE(o.set_rows("isotopes", rows));
  auto fig = build_isotope_evolution_fits(d, o, [&](const std::string& uuid) { return src.load_raw(uuid); });
  ASSERT_TRUE(fig) << fig.error().what;
  ASSERT_EQ(fig->fits.analyses.at(0).isotopes.size(), 2u);
  auto saved = src.revisions()->save_isotope_fits(fig->fits);
  ASSERT_TRUE(saved) << to_string(saved.error());
  ASSERT_TRUE(saved->saved) << saved->conflict;
  const std::string u = unknown_.str();
  ASSERT_EQ(saved->revisions.size(), 2u);
  auto a = src.load(u);
  ASSERT_TRUE(a);
  EXPECT_EQ((*a)->heads.at("baselines"), saved->revisions.at(u + "/baselines"));
  EXPECT_EQ((*a)->heads.at("intercepts"), saved->revisions.at(u + "/intercepts"));
  EXPECT_NEAR((*a)->find_isotope("Ar40")->baseline.value, 2.0, 1e-9);
  EXPECT_TRUE((*a)->find_isotope("Ar40")->baseline_reviewed);
  EXPECT_EQ((*a)->find_isotope("Ar40")->baseline_fit->kind, reduction::FitKind::Average);
  EXPECT_EQ(src.revisions()->history(u, RevisionKind::Baselines)->front().message,
            "<ISOEVO> refit Ar40(average),H1 baseline(average)");
}

TEST_F(StoreSourceTest, RevisionTablesForEveryKind) {
  auto& src = source();
  for (auto kind : kRevisionKinds) {
    auto history = src.revisions()->history(unknown_.str(), kind);
    ASSERT_TRUE(history) << to_string(history.error());
    if (kind == RevisionKind::Annotation) {
      EXPECT_TRUE(history->empty());
      continue;
    }
    ASSERT_EQ(history->size(), 1u) << to_string(kind);
    auto table = src.revisions()->revision_table((*history)[0].id);
    ASSERT_TRUE(table) << to_string(kind);
    EXPECT_FALSE(table->rows.empty()) << to_string(kind);
    for (const auto& row : table->rows) EXPECT_EQ(row.cells.size(), table->columns.size()) << to_string(kind);
  }
  auto tags = src.revisions()->history(unknown_.str(), RevisionKind::Tags);
  EXPECT_EQ(src.revisions()->revision_table((*tags)[0].id)->rows[0].cells[0], "ok");
  auto signals = src.revisions()->history(unknown_.str(), RevisionKind::Signals);
  const auto signal_table = *src.revisions()->revision_table((*signals)[0].id);
  EXPECT_EQ(signal_table.rows[1].key, "signal Ar40");
  EXPECT_EQ(signal_table.rows[1].cells[3], "1");  // start index
  EXPECT_FALSE(src.revisions()->revision_table(ps::Uuid::v7().str()));
  EXPECT_FALSE(src.revisions()->history("bad", RevisionKind::Tags));
}

TEST(StoreSourceMapping, IndexListsAndInterceptEdits) {
  EXPECT_EQ(parse_index_list("[3, 1,  7,3]"), (std::vector<std::size_t>{1, 3, 7}));
  EXPECT_TRUE(parse_index_list("[]").empty());
  EXPECT_TRUE(parse_index_list("").empty());
  EXPECT_EQ(parse_index_list("[1, x, 2]"), (std::vector<std::size_t>{1}));
  EXPECT_EQ(index_list_json({}), "[]");
  EXPECT_EQ(index_list_json({1, 5}), "[1, 5]");

  ps::InterceptRow row;
  row.isotope = "Ar40";
  row.detector = "H1";
  row.value = 1;
  row.manual.use_value = true;
  row.manual.value = 9;
  row.extra_json = R"({"keep": 1})";
  EditedFit e;
  e.key = "Ar40";
  e.value = {2.5, 0.1};
  e.fit.kind = reduction::FitKind::Parabolic;
  e.fit.error = reduction::ErrorType::Sd;
  e.fit.outliers = {true, 2, 2.5};
  e.n_points = 30;
  e.n_used = 27;
  e.user_excluded = {4};
  auto rows = apply_intercept_edits({row}, {e});
  ASSERT_TRUE(rows);
  const auto& r = (*rows)[0];
  EXPECT_EQ(r.value, 2.5);
  EXPECT_EQ(r.error, 0.1);
  EXPECT_EQ(r.fit, "parabolic");
  EXPECT_EQ(r.error_type, "SD");
  EXPECT_EQ(r.n, 30);
  EXPECT_EQ(r.fn, 27);
  EXPECT_EQ(r.user_excluded_json, "[4]");
  EXPECT_FALSE(r.manual.use_value);
  EXPECT_EQ(r.extra_json, row.extra_json);
  const auto o = flat_json_numbers(*r.filter_outliers_json);
  EXPECT_EQ(o.at("filter_outliers"), 1.0);
  EXPECT_EQ(o.at("iterations"), 2.0);
  EXPECT_EQ(o.at("std_devs"), 2.5);
  e.key = "Ar36";
  EXPECT_FALSE(apply_intercept_edits({row}, {e}));
}

// Legacy pychron writes no point count with a run's intercepts until the run
// is refitted, so most imported analyses have neither `n` nor `fn`. The
// series are in the store all the same, each with its length.
TEST(StoreSourceMapping, AnInterceptWithoutACountTakesItsSeriesLength) {
  const auto n_of = [](const StoreAnalysisParts& parts, const std::string& isotope) {
    const auto a = analysis_from_store(parts);
    EXPECT_TRUE(a) << (a ? "" : a.error().what);
    if (!a) return -1;
    for (const auto& iso : a->isotopes)
      if (iso.isotope == isotope) return iso.n;
    return -1;
  };
  const auto signal = [](const char* kind, const char* key, const char* detector, std::optional<int> n) {
    ps::SignalRefRow ref;
    ref.series_kind = kind;
    ref.series_key = key;
    ref.detector = detector;
    ref.n_points = n;
    return ref;
  };
  auto parts = argon_parts();
  EXPECT_EQ(n_of(parts, "Ar40"), 0);  // nothing known: no signals head at all

  parts.heads[ps::Kind::Signals] =
      ps::SignalRefs{signal("sniff", "Ar40", "H1", 12), signal("baseline", "H1", "H1", 90),
                     signal("signal", "Ar40", "H1", 340), signal("signal", "Ar39", "H1", std::nullopt)};
  EXPECT_EQ(n_of(parts, "Ar40"), 340);  // the signal series, not the sniff or the baseline
  EXPECT_EQ(n_of(parts, "Ar39"), 0);    // a series of unknown length
  EXPECT_EQ(n_of(parts, "Ar36"), 0);    // no series

  // A series that is a window of its blob has the window's length.
  auto& refs = std::get<ps::SignalRefs>(parts.heads[ps::Kind::Signals]);
  refs[2].start_index = 40;
  EXPECT_EQ(n_of(parts, "Ar40"), 300);
  refs[2].end_index = 140;
  EXPECT_EQ(n_of(parts, "Ar40"), 100);
  refs[2].end_index = 1000;  // past the end: as series_from_blob clamps it
  EXPECT_EQ(n_of(parts, "Ar40"), 300);

  // A stored count is the fit's own and wins: `fn` (after the outlier
  // filter) before `n`.
  auto& intercepts = std::get<ps::Intercepts>(parts.heads[ps::Kind::Intercepts]);
  intercepts[0].n = 330;
  EXPECT_EQ(n_of(parts, "Ar40"), 330);
  intercepts[0].fn = 323;
  EXPECT_EQ(n_of(parts, "Ar40"), 323);
}

// One isotope on two detectors: each intercept takes its own detector's series.
TEST(StoreSourceMapping, ASeriesLengthIsTakenFromTheInterceptsOwnDetector) {
  auto parts = argon_parts();
  auto& intercepts = std::get<ps::Intercepts>(parts.heads[ps::Kind::Intercepts]);
  intercepts[0].isotope = "H1:Ar40";
  ps::InterceptRow second = intercepts[0];
  second.isotope = "CDD:Ar40";
  second.detector = "CDD";
  intercepts.push_back(second);
  ps::SignalRefRow h1;
  h1.series_kind = "signal";
  h1.series_key = "H1:Ar40";
  h1.detector = "H1";
  h1.n_points = 200;
  ps::SignalRefRow cdd = h1;
  cdd.series_key = "CDD:Ar40";
  cdd.detector = "CDD";
  cdd.n_points = 60;
  parts.heads[ps::Kind::Signals] = ps::SignalRefs{h1, cdd};
  const auto a = analysis_from_store(parts);
  ASSERT_TRUE(a) << a.error().what;
  int seen = 0;
  for (const auto& iso : a->isotopes) {
    if (iso.key == "H1:Ar40") {
      EXPECT_EQ(iso.n, 200);
      ++seen;
    }
    if (iso.key == "CDD:Ar40") {
      EXPECT_EQ(iso.n, 60);
      ++seen;
    }
  }
  EXPECT_EQ(seen, 2);
}

TEST(StoreSourceMapping, NonfinitePointers) {
  using V = std::vector<std::string>;
  EXPECT_EQ(nonfinite_pointers(R"({"nonfinite": {"/value": "NaN", "/error": "Infinity"}})"), (V{"/value", "/error"}));
  EXPECT_EQ(nonfinite_pointers(R"({"a": [1, {"nonfinite": {"/x": "NaN"}}], "s": "}\"{", "n": -1.5e3, "b": true,
                                  "nonfinite" : { "/value" : "-Infinity" } , "z": null})"),
            (V{"/value"}));
  EXPECT_TRUE(nonfinite_pointers(R"({"nested": {"nonfinite": {"/value": "NaN"}}})").empty());  // top level only
  EXPECT_TRUE(nonfinite_pointers(R"({"nonfinite": "/value"})").empty());
  EXPECT_TRUE(nonfinite_pointers(R"({"nonfinite": {}})").empty());
  EXPECT_TRUE(nonfinite_pointers("{}").empty());
  EXPECT_TRUE(nonfinite_pointers("").empty());
  EXPECT_TRUE(nonfinite_pointers(R"({"nonfinite": {"/value": "NaN")").empty());  // cut short
}

TEST(StoreSourceMapping, FlatJsonNumbers) {
  const auto m = flat_json_numbers(R"( {"a": 1.5, "b": -2e3, "s": "x\"y", "o": {"z": [1, {"q": 2}]}, "t": true,
                                       "f": false, "n": null, "c": 3} )");
  EXPECT_EQ(m, (std::map<std::string, double>{{"a", 1.5}, {"b", -2000}, {"t", 1}, {"f", 0}, {"c", 3}}));
  EXPECT_TRUE(flat_json_numbers("[1, 2]").empty());
  EXPECT_EQ(flat_json_numbers(R"({"a": 1, "b": })"), (std::map<std::string, double>{{"a", 1}}));
  EXPECT_TRUE(flat_json_numbers("").empty());
}

TEST(StoreSourceMapping, SeriesFromBlob) {
  ps::SignalRefRow ref{"baseline", "H1", "H1", {}, 5, std::nullopt, 2};
  const auto bytes = ps::encode_tvs(std::vector<ps::TvsPoint>{{0, 1, 0.1f}, {1, 2, 0.1f}, {2, 3, 0.1f}});
  auto s = series_from_blob(ref, ps::BlobData{"f32le-tvs/1", bytes, 3});
  ASSERT_TRUE(s) << to_string(s.error());
  EXPECT_EQ(s->kind, SeriesKind::Baseline);
  EXPECT_EQ(s->v, (std::vector<double>{1, 2}));
  ref.start_index = 7;  // out of range: empty, not a crash
  EXPECT_TRUE(series_from_blob(ref, ps::BlobData{"f32le-tvs/1", bytes, 3})->t.empty());
  EXPECT_FALSE(series_from_blob(ref, ps::BlobData{"zstd/1", bytes, 3}));
  ref.series_kind = "whiff";
  EXPECT_FALSE(series_from_blob(ref, ps::BlobData{"f32le-tvs/1", bytes, 3}));
}

TEST(StoreSourceMapping, QueryAndCursorRoundTrip) {
  BrowseQuery q;
  q.from = 1'700'000'000.25;
  q.samples = {"FC-2"};
  const auto f = to_store_filter(q);
  EXPECT_EQ(f.from->micros, 1'700'000'000'250'000);
  EXPECT_EQ(f.samples, q.samples);
  EXPECT_EQ(f.exclude_tags, (std::vector<std::string>{"invalid"}));
  EXPECT_FALSE(f.age_min);
  EXPECT_FALSE(f.age_max);
  q.age_min = 1.5;
  q.age_max = 28.2;
  const auto aged = to_store_filter(q);
  EXPECT_EQ(aged.age_min, 1.5);
  EXPECT_EQ(aged.age_max, 28.2);
  EXPECT_EQ(to_store_facet(Facet::Repository), ps::BrowseFacet::Repository);
  EXPECT_EQ(to_store_facet(Facet::Load), ps::BrowseFacet::Load);

  ps::BrowseRow row;
  row.summary.uuid = ps::Uuid::v7();
  row.age = 28.2;
  row.age_err = 0.05;
  const auto s = summary_from_row(row);
  EXPECT_EQ(s.age, 28.2);
  EXPECT_EQ(s.age_err, 0.05);
  EXPECT_FALSE(summary_from_row(ps::BrowseRow{}).age);
}

}  // namespace
}  // namespace pychron::processing
