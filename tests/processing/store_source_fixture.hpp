#pragma once

// The store the StoreSource suites run on (tests/processing
// test_store_source.cpp, the neutral cases; tests/arar test_store_loader.cpp,
// the Ar-Ar ones): SQLite in a temp file, so the test's store and the
// source's worker connections share one database, seeded with an irradiated
// unknown (identifier 77000, NM-300 A 3, its flux, production, chronology
// and gains references) and an air.

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "pychron/core/sha256.hpp"
#include "pychron/processing/store_source.hpp"

namespace pychron::processing::test {

namespace ps = pychron::persistence;
namespace fs = std::filesystem;

inline ps::Bytes tv(float offset, int n = 5) {
  std::vector<ps::TvPoint> p;
  p.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) p.push_back({static_cast<float>(i), offset + static_cast<float>(i)});
  return ps::encode_tv(p);
}

class StoreSourceFixture : public ::testing::Test {
 protected:
  // How source() opens the source. tests/arar adds the Ar-Ar loader.
  [[nodiscard]] virtual StoreSourceOptions source_options() const {
    StoreSourceOptions options;
    options.user = "tester";
    options.hostname = "test-host";
    options.reduction_version = "arar-1";  // what the seeded store's fingerprints are compared under
    return options;
  }

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
    ms_ = *store_->add_mass_spectrometer(acq_, {"jan", "argus", "j", std::nullopt});
    irr_ = *store_->add_irradiation(acq_, "NM-300");
    level_ = *store_->add_level(acq_, {irr_, "A", std::nullopt, 0.5, std::nullopt, std::nullopt});
    const auto pi = *store_->add_principal_investigator(acq_, {"Ross", "J", std::nullopt, std::nullopt, std::nullopt});
    const auto project = *store_->add_project(acq_, {"Fish Canyon", pi, std::nullopt});
    const auto material = *store_->add_material(acq_, {"sanidine", "60-80", std::nullopt});
    project_ = project;
    material_ = material;
    const auto sample = *store_->add_sample(acq_, {.name = "FC-2",
                                                   .project = project,
                                                   .material = material,
                                                   .igsn = "IEFC20001",
                                                   .lat = 37.75,
                                                   .lon = -106.9,
                                                   .elevation = 2850.0,
                                                   .lithology = "ash-flow tuff"});
    ASSERT_TRUE(store_->add_extract_device(acq_, "co2"));
    position_ = *store_->add_irradiation_position(acq_, {level_, 3, sample, std::nullopt, {}, {}, std::nullopt});
    ASSERT_TRUE(store_->add_identifier(acq_, {"77000", "unknown", std::nullopt, std::nullopt, position_, std::nullopt, std::nullopt}));
    ASSERT_TRUE(store_->add_identifier(acq_, {"66574", "special", "air", std::nullopt, std::nullopt, std::nullopt, std::nullopt}));

    ps::RefObjectSpec pos, lvl, irr, ms;
    pos.position = position_;
    lvl.level = level_;
    irr.irradiation = irr_;
    ms.mass_spectrometer = ms_;
    ps::FluxValue flux;
    flux.j = 0.001;
    flux.j_err = 1e-6;
    flux.position_jerr = 2e-7;
    flux.monitor_name = "FC-2";
    flux.monitor_material = "sanidine";
    flux.monitor_age = 28.201;
    flux.monitor_age_err = 0.023;
    flux_object_ = ref_object(ps::RefType::FluxPosition, "NM-300/A/3", pos);
    publish(flux_object_, flux);
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
  ps::Uuid ingest(const std::string& identifier, int aliquot, const std::string& type, const std::string& ts,
                  const std::string& spectrometer = "jan") {
    ps::AnalysisIngest a;
    a.analysis = ps::Uuid::v7();
    a.changeset = ps::Uuid::v7();
    a.created = ps::UtcTime::now();
    a.identifier = identifier;
    a.aliquot = aliquot;
    a.analysis_type = type;
    a.timestamp = *ps::UtcTime::parse(ts);
    a.mass_spectrometer = spectrometer;
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
      auto s = StoreSource::open(ps::StoreConfig{url_, false}, source_options());
      EXPECT_TRUE(s) << (s ? "" : to_string(s.error()));
      if (s) source_ = std::move(*s);
    }
    return *source_;
  }

  fs::path path_;
  std::string url_;
  std::unique_ptr<ps::IStore> store_;
  std::unique_ptr<StoreSource> source_;
  ps::Uuid acq_, red_, reducer_, ms_, irr_, level_, position_, unknown_, air_, project_, material_, flux_object_;

  ps::Bytes signal_;
};

// Five argon isotopes on H1 with a baseline, an Ar40 blank and an IC factor.
inline StoreAnalysisParts argon_parts() {
  StoreAnalysisParts parts;
  parts.detail.row.summary.analysis_type = "unknown";
  parts.detail.row.summary.timestamp = *ps::UtcTime::parse("2026-10-02T10:00:00Z");
  ps::Intercepts intercepts;
  for (const char* iso : {"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"}) {
    ps::InterceptRow row;
    row.isotope = iso;
    row.detector = "H1";
    row.value = 100.0;
    row.error = 0.1;
    intercepts.push_back(row);
  }
  ps::BaselineRow baseline;
  baseline.detector = "H1";
  baseline.value = 0.01;
  baseline.error = 0.001;
  ps::BlankRow blank;
  blank.isotope = "Ar40";
  blank.value = 0.5;
  blank.error = 0.05;
  ps::IcFactorRow ic;
  ic.detector = "H1";
  ic.value = 1.02;
  ic.error = 0.001;
  parts.heads[ps::Kind::Intercepts] = std::move(intercepts);
  parts.heads[ps::Kind::Baselines] = ps::Baselines{baseline};
  parts.heads[ps::Kind::Blanks] = ps::Blanks{blank};
  parts.heads[ps::Kind::IcFactors] = ps::IcFactors{ic};
  return parts;
}

template <class Rows>
auto& first_row(StoreAnalysisParts& parts, ps::Kind kind) {
  return std::get<Rows>(parts.heads.at(kind)).front();
}

}  // namespace pychron::processing::test
