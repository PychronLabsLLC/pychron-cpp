#pragma once

// Shared fixture of the commands that read the DVC store (`export`,
// `earthbank`): a file-backed SQLite store with one irradiated unknown (two
// heating steps) and one air, as the store-source tests seed it, with the
// catalog metadata a report lists. Only with persistence.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <gtest/gtest.h>

#include "elctl_fixture.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/persistence/store.hpp"

namespace elctl::testing {

namespace ps = pychron::persistence;

class StoreTest : public ElctlTest {
 protected:
  void SetUp() override {
    ElctlTest::SetUp();
    db_ = "sqlite:" + path("store.db").string();
    auto store = ps::open_store(ps::StoreConfig{db_, true});
    ASSERT_TRUE(store) << to_string(store.error());
    store_ = std::move(*store);
    seed();
  }

  void TearDown() override {
    store_.reset();
    ElctlTest::TearDown();
  }

  void publish(ps::Uuid object, ps::RefPayload payload) {
    auto uow = *store_->begin(ps::Actor{reducer_, red_});
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
    const auto pi = *store_->add_principal_investigator(acq_, {"Doe", "A", std::nullopt, std::nullopt, std::nullopt});
    project_ = *store_->add_project(acq_, {"Fish Canyon", pi, std::nullopt});
    material_ = *store_->add_material(acq_, {"sanidine", "60-80", std::nullopt});
    sample_ = *store_->add_sample(acq_, {.name = "FC-2",
                                                   .project = project_,
                                                   .material = material_,
                                                   .igsn = "IEFC20001",
                                                   .lat = 37.75,
                                                   .lon = -106.9,
                                                   .elevation = 2850.0,
                                                   .lithology = "ash-flow tuff"});
    ASSERT_TRUE(store_->add_extract_device(acq_, "co2"));
    position_ = *store_->add_irradiation_position(acq_, {level_, 3, sample_, std::nullopt, {}, {}, std::nullopt});
    ASSERT_TRUE(store_->add_identifier(acq_, {"77000", "unknown", std::nullopt, std::nullopt, position_, std::nullopt, std::nullopt}));
    ASSERT_TRUE(store_->add_identifier(acq_, {"66574", "special", "air", std::nullopt, std::nullopt, std::nullopt, std::nullopt}));

    ps::RefObjectSpec pos, lvl, irr;
    pos.position = position_;
    lvl.level = level_;
    irr.irradiation = irr_;
    ps::FluxValue flux;
    flux.j = 0.001;
    flux.j_err = 1e-6;
    flux.monitor_name = "FC-2";
    flux.monitor_material = "sanidine";
    flux.monitor_age = 28.201;
    flux.monitor_age_err = 0.023;
    publish(ref_object(ps::RefType::FluxPosition, "NM-300/A/3", pos), flux);
    const auto prod = ref_object(ps::RefType::Production, "NM-300/Triga", {});
    publish(prod, ps::ProductionValue{"Triga", std::nullopt, {{"K4039", 0.0008, 5e-5}, {"Ca3937", 0.0007, 1e-5}}});
    publish(ref_object(ps::RefType::LevelProduction, "NM-300/A", lvl), ps::LevelProductionValue{prod, std::nullopt});
    publish(ref_object(ps::RefType::Chronology, "NM-300", irr),
            ps::ChronologyValue{{{0, 1.0, *ps::UtcTime::parse("2026-01-01T00:00:00Z"),
                                  *ps::UtcTime::parse("2026-01-01T10:00:00Z")}}});

    ingest("77000", 1, 0, "unknown", "2026-10-02T10:00:00Z", 1000.0);
    ingest("77000", 1, 1, "unknown", "2026-10-02T10:30:00Z", 1500.0);
    ingest("66574", 1, -1, "air", "2026-10-02T11:00:00Z", 2955.0);
  }

  // Another unknown of the seeded irradiation level: `identifier` in `hole`,
  // of the sample `sample`, with the flux of a monitor of that age there.
  void unknown_at(int hole, ps::Uuid sample, const std::string& identifier, double monitor_age = 28.201) {
    ps::RefObjectSpec pos;
    pos.position = *store_->add_irradiation_position(acq_, {level_, hole, sample, std::nullopt, {}, {}, std::nullopt});
    ASSERT_TRUE(store_->add_identifier(acq_, {identifier, "unknown", std::nullopt, std::nullopt, pos.position, std::nullopt, std::nullopt}));
    ps::FluxValue flux;
    flux.j = 0.001;
    flux.j_err = 1e-6;
    flux.monitor_name = "FC-2";
    flux.monitor_material = "sanidine";
    flux.monitor_age = monitor_age;
    flux.monitor_age_err = 0.023;
    publish(ref_object(ps::RefType::FluxPosition, "NM-300/A/" + std::to_string(hole), pos), flux);
  }

  void ingest(const std::string& identifier, int aliquot, int increment, const std::string& type,
              const std::string& ts, double ar40) {
    ps::AnalysisIngest a;
    a.analysis = ps::Uuid::v7();
    a.changeset = ps::Uuid::v7();
    a.created = ps::UtcTime::now();
    a.identifier = identifier;
    a.aliquot = aliquot;
    a.increment = increment;
    a.analysis_type = type;
    a.timestamp = *ps::UtcTime::parse(ts);
    a.mass_spectrometer = "jan";
    a.extract_device = "co2";
    a.extraction.extract_value = 4.5;
    a.extraction.extract_units = "W";
    a.analyst = "adoe";
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
    r.intercepts_rows = {intercept("Ar40", "H1", ar40, 0.5), intercept("Ar39", "AX", 100.0, 0.1),
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
    const ps::Uuid id = a.analysis;
    auto ok = store_->ingest(ps::IngestItem{id, pychron::sha256(std::string_view{"payload-" + id.str()}), acq_, std::move(a)});
    ASSERT_TRUE(ok) << (ok ? "" : to_string(ok.error()));
  }

  std::string db_;
  std::unique_ptr<ps::IStore> store_;
  ps::Uuid acq_, red_, reducer_, ms_, irr_, level_, position_, project_, material_, sample_;
};

}  // namespace elctl::testing
