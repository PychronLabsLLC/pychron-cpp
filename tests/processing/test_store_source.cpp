// StoreSource (data browsing and visualization design, section 9.2): browse,
// facets and loads over a real store, reference data in the reduction
// context, raw series from blobs, change-log refresh, and the pure mappings.
// SQLite in a temp file, so the test's store and the source's worker
// connections share one database.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <random>
#include <thread>

#include "pychron/processing/reduced.hpp"
#include "pychron/processing/store_source.hpp"

namespace pychron::processing {
namespace {

namespace ps = pychron::persistence;
namespace fs = std::filesystem;

ps::Bytes tv(float offset, int n = 5) {
  std::vector<ps::TvPoint> p;
  for (int i = 0; i < n; ++i) p.push_back({static_cast<float>(i), offset + static_cast<float>(i)});
  return ps::encode_tv(p);
}

class StoreSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<int> counter{0};
    path_ = fs::temp_directory_path() /
            ("pychron_store_source_" + std::to_string(std::random_device{}() % 1000000) + "_" +
             std::to_string(counter.fetch_add(1)) + ".sqlite");
    fs::remove(path_);
    url_ = "sqlite:" + path_.string();
    auto store = ps::open_store(ps::StoreConfig{url_, true});
    ASSERT_TRUE(store) << to_string(store.error());
    store_ = std::move(*store);
    seed();
  }

  void TearDown() override {
    source_.reset();
    store_.reset();
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm"}) fs::remove(path_.string() + suffix, ec);
  }

  ps::Actor reducer() const { return ps::Actor{reducer_, red_}; }

  void publish(ps::Uuid object, ps::RefPayload payload) {
    auto uow = *store_->begin(reducer());
    ASSERT_TRUE(uow->add_revision(object, ps::Kind::RefValue, ps::RevisionPayload{std::move(payload)},
                                  *store_->head(object, ps::Kind::RefValue)));
    auto outcome = uow->commit(ps::ChangesetKind::Reference, "<META> update");
    ASSERT_TRUE(outcome && std::holds_alternative<ps::Committed>(*outcome));
  }

  ps::Uuid ref_object(ps::RefType type, const std::string& key, ps::RefObjectSpec scope) {
    scope.type = type;
    scope.key = key;
    return *store_->add_ref_object(red_, scope);
  }

  void seed() {
    acq_ = *store_->register_client({"acq-1", "acquisition", std::nullopt, "test"});
    red_ = *store_->register_client({"red-1", "reduction", std::nullopt, "test"});
    reducer_ = *store_->ensure_user(red_, "jsmith");
    ms_ = *store_->add_mass_spectrometer(acq_, {"jan", "argus", "j"});
    irr_ = *store_->add_irradiation(acq_, "NM-300");
    level_ = *store_->add_level(acq_, {irr_, "A", std::nullopt, 0.5, std::nullopt});
    const auto pi = *store_->add_principal_investigator(acq_, {"Ross", "J", std::nullopt, std::nullopt});
    const auto project = *store_->add_project(acq_, {"Fish Canyon", pi});
    const auto material = *store_->add_material(acq_, {"sanidine", "60-80"});
    const auto sample =
        *store_->add_sample(acq_, {"FC-2", project, material, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    ASSERT_TRUE(store_->add_extract_device(acq_, "co2"));
    position_ = *store_->add_irradiation_position(acq_, {level_, 3, sample, std::nullopt, {}, {}});
    ASSERT_TRUE(store_->add_identifier(acq_, {"77000", "unknown", std::nullopt, std::nullopt, position_, std::nullopt}));
    ASSERT_TRUE(store_->add_identifier(acq_, {"66574", "special", "air", std::nullopt, std::nullopt, std::nullopt}));

    ps::RefObjectSpec pos, lvl, irr, ms;
    pos.position = position_;
    lvl.level = level_;
    irr.irradiation = irr_;
    ms.mass_spectrometer = ms_;
    ps::FluxValue flux;
    flux.j = 0.001;
    flux.j_err = 1e-6;
    flux.position_jerr = 2e-7;
    publish(ref_object(ps::RefType::FluxPosition, "NM-300/A/3", pos), flux);
    const auto prod = ref_object(ps::RefType::Production, "NM-300/Triga", {});
    publish(prod, ps::ProductionValue{"Triga", std::nullopt, {{"K4039", 0.0008, 5e-5}, {"Ca3937", 0.0007, 1e-5}}});
    publish(ref_object(ps::RefType::LevelProduction, "NM-300/A", lvl), ps::LevelProductionValue{prod, std::nullopt});
    publish(ref_object(ps::RefType::Chronology, "NM-300", irr),
            ps::ChronologyValue{{{0, 1.0, *ps::UtcTime::parse("2026-01-01T00:00:00Z"),
                                  *ps::UtcTime::parse("2026-01-01T10:00:00Z")}}});
    publish(ref_object(ps::RefType::Gains, "jan", ms), ps::GainsValue{{{"AX", 1.0}, {"H1", 1.5}, {"L2", 0.98}}});

    unknown_ = ingest("77000", 1, "unknown", "2026-10-02T10:00:00Z");
    air_ = ingest("66574", 1, "air", "2026-10-02T11:00:00Z");
  }

  // An analysis with five argon isotopes on two detectors. Only the Ar40
  // signal and the H1 baseline have blobs; the Ar40 one is uploaded.
  ps::Uuid ingest(const std::string& identifier, int aliquot, const std::string& type, const std::string& ts) {
    ps::AnalysisIngest a;
    a.analysis = ps::Uuid::v7();
    a.changeset = ps::Uuid::v7();
    a.created = ps::UtcTime::now();
    a.identifier = identifier;
    a.aliquot = aliquot;
    a.analysis_type = type;
    a.timestamp = *ps::UtcTime::parse(ts);
    a.mass_spectrometer = "jan";
    a.extract_device = "co2";
    a.extraction.extract_value = 4.5;
    a.extraction.extract_units = "W";
    a.extraction.extract_duration = 30;
    a.analyst = "jross";
    a.load_name = "load-1";
    for (const char* iso : {"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"})
      a.isotopes.push_back({iso, std::string(iso) == "Ar40" ? "H1" : "AX", "fA", std::nullopt, std::nullopt, std::nullopt});
    a.detectors = {{"H1", 12.0, 1.02}, {"AX", 0.0, std::nullopt}};
    a.meta = ps::AnalysisMetaRow{};
    a.meta->environmental_json = R"({"lab_temperature": 21.5, "lab_humidity": 33, "note": "x", "nested": {"a": 1}})";
    ps::PeakCenterRow pc;
    pc.detector = "H1";
    pc.center_dac = 5.123;
    pc.resolution = 450;
    a.peak_centers = {pc};
    auto& r = a.roots;
    r.signals = ps::Uuid::v7();
    r.intercepts = ps::Uuid::v7();
    r.baselines = ps::Uuid::v7();
    r.blanks = ps::Uuid::v7();
    r.icfactors = ps::Uuid::v7();
    r.tags = ps::Uuid::v7();
    signal_ = tv(100);
    r.signal_refs = {{"baseline", "H1", "H1", ps::blob_sha256(ps::kCodecTv, tv(0)), 5, std::nullopt, std::nullopt},
                     {"signal", "Ar40", "H1", ps::blob_sha256(ps::kCodecTv, signal_), 5, 1, 4}};
    auto intercept = [](const char* iso, const char* det, double v, double e) {
      ps::InterceptRow row;
      row.isotope = iso;
      row.detector = det;
      row.value = v;
      row.error = e;
      row.fit = "Linear";
      row.error_type = "SD";
      row.n = 100;
      row.filter_outliers_json = R"({"filter_outliers": true, "iterations": 2, "std_devs": 2.5})";
      return row;
    };
    r.intercepts_rows = {intercept("Ar40", "H1", 1000.0, 0.5), intercept("Ar39", "AX", 100.0, 0.1),
                         intercept("Ar38", "AX", 2.0, 0.01), intercept("Ar37", "AX", 0.5, 0.01),
                         intercept("Ar36", "AX", 0.5, 0.01)};
    r.intercepts_rows[4].manual.use_value = true;  // a manual Ar36 value wins
    r.intercepts_rows[4].manual.value = 0.4;
    ps::BaselineRow h1;
    h1.detector = "H1";
    h1.value = 0.01;
    h1.error = 0.001;
    h1.fit = "average";
    r.baselines_rows = {h1};
    ps::BlankRow blank;
    blank.isotope = "Ar40";
    blank.value = 0.5;
    blank.error = 0.05;
    blank.fit = "preceding";
    r.blanks_rows = {blank};
    ps::IcFactorRow ic;
    ic.detector = "AX";
    ic.value = 1.02;
    ic.error = 0.001;
    r.icfactors_rows = {ic};
    const ps::Uuid id = a.analysis;
    auto ok = store_->ingest(ps::IngestItem{id, sha256(std::string_view{"payload-" + id.str()}), acq_, std::move(a)});
    EXPECT_TRUE(ok) << (ok ? "" : to_string(ok.error()));
    auto blob = store_->ingest(ps::IngestItem{ps::Uuid::v7(), {}, acq_, ps::BlobIngest{"f32le-tv/1", signal_, 5}});
    EXPECT_TRUE(blob) << (blob ? "" : to_string(blob.error()));
    return id;
  }

  StoreSource& source() {
    if (!source_) {
      auto s = StoreSource::open(ps::StoreConfig{url_, false}, StoreSourceOptions{2, "tester", "test-host"});
      EXPECT_TRUE(s) << (s ? "" : to_string(s.error()));
      if (s) source_ = std::move(*s);
    }
    return *source_;
  }

  fs::path path_;
  std::string url_;
  std::unique_ptr<ps::IStore> store_;
  std::unique_ptr<StoreSource> source_;
  ps::Uuid acq_, red_, reducer_, ms_, irr_, level_, position_, unknown_, air_;
  ps::Bytes signal_;
};

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
  EXPECT_EQ(s.principal_investigator, "Ross, J");
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

TEST_F(StoreSourceTest, LoadAssemblesTheAnalysisAndItsReductionContext) {
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

  ASSERT_TRUE(a.context.flux);
  EXPECT_EQ(a.context.flux->j.value, 0.001);
  EXPECT_EQ(a.context.flux->j.error, 1e-6);
  EXPECT_EQ(a.context.flux->position_jerr, 2e-7);
  ASSERT_TRUE(a.context.production);
  EXPECT_EQ(a.context.production->k4039.value, 0.0008);
  EXPECT_EQ(a.context.production->ca3937.value, 0.0007);
  ASSERT_EQ(a.context.chronology.size(), 1u);
  EXPECT_EQ(a.context.chronology[0].end_utc_s - a.context.chronology[0].start_utc_s, 36000);

  const auto reduced = reduce_analysis(*loaded, ReductionSettings{});
  ASSERT_TRUE(reduced->arar) << reduced->reduction_error;
  ASSERT_TRUE(reduced->arar->ages);

  // The air has no irradiation position, hence no flux and no age.
  auto air = source().load(air_.str());
  ASSERT_TRUE(air) << to_string(air.error());
  EXPECT_FALSE((*air)->context.flux);
  EXPECT_FALSE((*air)->context.production);

  // Cached until something changes.
  EXPECT_EQ(source().load(unknown_.str())->get(), loaded->get());
  EXPECT_FALSE(source().load("not-a-uuid"));
  EXPECT_FALSE(source().load(ps::Uuid::v7().str()));
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
  EXPECT_EQ(to_store_facet(Facet::Repository), ps::BrowseFacet::Repository);
  EXPECT_EQ(to_store_facet(Facet::Load), ps::BrowseFacet::Load);
}

}  // namespace
}  // namespace pychron::processing
