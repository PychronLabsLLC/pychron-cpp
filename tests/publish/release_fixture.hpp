#pragma once

// A store with releasable analyses, for the publish tests: two samples of
// one project and one of another, each with one unknown of five argon
// isotopes on a level that has a flux, a production and a chronology (so
// the analyses have ages and references to pin). Engine-parameterised as the
// persistence tests are: SQLite always, PostgreSQL when PYCHRON_TEST_PG_URL
// is set.

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "persistence/store_fixture.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::publish::testing {

namespace ps = persistence;

class ReleaseFixture : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    db_ = std::make_unique<ps::testing::TestDatabase>(GetParam(), true);
    store_ = ps::testing::open_or_die(db_->url());
    ASSERT_TRUE(store_);
    lab_ = ps::testing::seed_lab(*store_);
    const ps::Uuid c = lab_.reduction_client;
    project_ = *store_->add_project(c, {"Alpha"});
    project2_ = *store_->add_project(c, {"Beta"});
    material_ = *store_->add_material(c, {"sanidine", "60-80"});
    sample1_ = *store_->add_sample(c, {.name = "FC-1",
                                       .project = project_,
                                       .material = material_,
                                       .igsn = "IEFC10001",
                                       .lat = 37.75,
                                       .lon = -106.9,
                                       .elevation = 2850.0,
                                       .lithology = "ash-flow tuff"});
    sample2_ = *store_->add_sample(c, {"FC-2", project_, material_});  // no location
    sample3_ = *store_->add_sample(c, {.name = "BT-1", .project = project2_, .material = material_, .lat = 34.0, .lon = -106.9});
    // Positions 2 and 3 of the seeded level hold samples 1 and 2; sample 3's
    // identifier has no position (and so no J).
    position1_ = *store_->add_irradiation_position(lab_.acquisition_client, {lab_.level, 2, sample1_});
    position2_ = *store_->add_irradiation_position(lab_.acquisition_client, {lab_.level, 3, sample2_});
    ASSERT_TRUE(store_->add_identifier(lab_.acquisition_client, {.identifier = "70001", .position = position1_}));
    ASSERT_TRUE(store_->add_identifier(lab_.acquisition_client, {.identifier = "70002", .position = position2_}));
    ASSERT_TRUE(store_->add_identifier(lab_.acquisition_client, {.identifier = "70003", .sample = sample3_}));
    seed_references();
    analysis1_ = ingest("70001", 1, "2026-10-02T10:00:00Z");
    analysis2_ = ingest("70002", 1, "2026-10-02T11:00:00Z");
    analysis3_ = ingest("70003", 1, "2026-10-02T12:00:00Z");
  }

  ps::Actor reducer() const { return ps::Actor{lab_.reducer, lab_.reduction_client}; }
  std::vector<ps::Uuid> analyses() const { return {analysis1_, analysis2_, analysis3_}; }
  std::vector<std::string> analysis_texts() const {
    return {analysis1_.str(), analysis2_.str(), analysis3_.str()};
  }

  // A tags revision on top of the analysis's head.
  void retag(ps::Uuid analysis, const std::string& tag) {
    auto uow = *store_->begin(reducer());
    ASSERT_TRUE(uow->add_revision(analysis, ps::Kind::Tags, ps::TagValue{tag, std::nullopt, std::nullopt},
                                  *store_->head(analysis, ps::Kind::Tags)));
    auto outcome = uow->commit(ps::ChangesetKind::Reduction, "tag " + tag);
    ASSERT_TRUE(outcome && std::holds_alternative<ps::Committed>(*outcome));
  }

  void publish_ref(ps::Uuid object, ps::RefPayload payload) {
    auto uow = *store_->begin(reducer());
    ASSERT_TRUE(uow->add_revision(object, ps::Kind::RefValue, ps::RevisionPayload{std::move(payload)},
                                  *store_->head(object, ps::Kind::RefValue)));
    auto outcome = uow->commit(ps::ChangesetKind::Reference, "<META> update");
    ASSERT_TRUE(outcome && std::holds_alternative<ps::Committed>(*outcome));
  }

  ps::Uuid ref_object(ps::RefType type, const std::string& key, ps::RefObjectSpec scope) {
    scope.type = type;
    scope.key = key;
    return *store_->add_ref_object(lab_.reduction_client, scope);
  }

  void seed_references() {
    ps::RefObjectSpec pos1, pos2, lvl, irr;
    pos1.position = position1_;
    pos2.position = position2_;
    lvl.level = lab_.level;
    irr.irradiation = lab_.irradiation;
    for (const auto& [key, pos] : {std::pair{"NM-300/A/2", pos1}, std::pair{"NM-300/A/3", pos2}}) {
      ps::FluxValue flux;
      flux.j = 0.001;
      flux.j_err = 1e-6;
      flux.position_jerr = 2e-7;
      flux.monitor_name = "FC-2";
      flux.monitor_material = "sanidine";
      flux.monitor_age = 28.201;
      flux.monitor_age_err = 0.023;
      publish_ref(ref_object(ps::RefType::FluxPosition, key, pos), flux);
    }
    production_ = ref_object(ps::RefType::Production, "NM-300/Triga", {});
    publish_ref(production_, ps::ProductionValue{"Triga", std::nullopt, {{"K4039", 0.0008, 5e-5}, {"Ca3937", 0.0007, 1e-5}}});
    publish_ref(ref_object(ps::RefType::LevelProduction, "NM-300/A", lvl), ps::LevelProductionValue{production_, std::nullopt});
    publish_ref(ref_object(ps::RefType::Chronology, "NM-300", irr),
                ps::ChronologyValue{{{0, 1.0, *ps::UtcTime::parse("2026-01-01T00:00:00Z"),
                                      *ps::UtcTime::parse("2026-01-01T10:00:00Z")}}});
  }

  // An unknown with five argon isotopes, as the store-source tests ingest one.
  ps::Uuid ingest(const std::string& identifier, int aliquot, const std::string& ts) {
    ps::AnalysisIngest a;
    a.analysis = ps::Uuid::v7();
    a.changeset = ps::Uuid::v7();
    a.created = ps::UtcTime::now();
    a.identifier = identifier;
    a.aliquot = aliquot;
    a.analysis_type = "unknown";
    a.timestamp = *ps::UtcTime::parse(ts);
    a.mass_spectrometer = "jan";
    a.extraction.extract_value = 4.5;
    a.extraction.extract_units = "W";
    a.analyst = "jross";
    a.load_name = "load-1";
    a.record_sha256 = sha256(std::string_view{"record-" + identifier});
    for (const char* iso : {"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"})
      a.isotopes.push_back({iso, std::string(iso) == "Ar40" ? "H1" : "AX", "fA", std::nullopt, std::nullopt, std::nullopt});
    a.detectors = {{"H1", 12.0, 1.02}, {"AX", 0.0, std::nullopt}};
    auto& r = a.roots;
    r.signals = ps::Uuid::v7();
    r.intercepts = ps::Uuid::v7();
    r.baselines = ps::Uuid::v7();
    r.blanks = ps::Uuid::v7();
    r.icfactors = ps::Uuid::v7();
    r.tags = ps::Uuid::v7();
    const ps::Bytes signal = ps::testing::series(100), baseline = ps::testing::series(0);
    r.signal_refs = {{"baseline", "H1", "H1", ps::blob_sha256(ps::kCodecTv, baseline), 4, std::nullopt, std::nullopt},
                     {"signal", "Ar40", "H1", ps::blob_sha256(ps::kCodecTv, signal), 4, std::nullopt, std::nullopt}};
    auto intercept = [](const char* iso, const char* det, double v, double e) {
      ps::InterceptRow row;
      row.isotope = iso;
      row.detector = det;
      row.value = v;
      row.error = e;
      row.fit = "Linear";
      row.error_type = "SD";
      row.n = 100;
      return row;
    };
    r.intercepts_rows = {intercept("Ar40", "H1", 1000.0, 0.5), intercept("Ar39", "AX", 100.0, 0.1),
                         intercept("Ar38", "AX", 2.0, 0.01), intercept("Ar37", "AX", 0.5, 0.01),
                         intercept("Ar36", "AX", 0.5, 0.01)};
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
    auto ok = store_->ingest(ps::IngestItem{id, sha256(std::string_view{"payload-" + id.str()}), lab_.acquisition_client, std::move(a)});
    EXPECT_TRUE(ok) << (ok ? "" : to_string(ok.error()));
    for (const auto& bytes : {signal, baseline}) {
      auto blob = store_->ingest(ps::IngestItem{ps::Uuid::v7(), {}, lab_.acquisition_client, ps::BlobIngest{"f32le-tv/1", bytes, 4}});
      EXPECT_TRUE(blob) << (blob ? "" : to_string(blob.error()));
    }
    return id;
  }

  std::unique_ptr<ps::testing::TestDatabase> db_;  // first: the store holds a connection to it
  std::unique_ptr<ps::IStore> store_;
  ps::testing::Lab lab_;
  ps::Uuid project_, project2_, material_, sample1_, sample2_, sample3_, position1_, position2_, production_,
      analysis1_, analysis2_, analysis3_;
};

}  // namespace pychron::publish::testing
