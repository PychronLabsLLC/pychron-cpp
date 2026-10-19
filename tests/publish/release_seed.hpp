#pragma once

// The store of the release tests as a function: two samples of one project
// and one of another, each with one unknown of five argon isotopes on a level
// that has a flux, a production and a chronology (so the analyses have ages
// and references to pin). No gtest assertions: a failure is a Result, so a Qt
// test can seed a store the same way (tests/ui/test_publish_window.cpp).

#include <string>
#include <utility>
#include <variant>

#include "persistence/store_fixture.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::publish::testing {

namespace ps = persistence;

struct ReleaseSeed {
  ps::testing::Lab lab;
  ps::Uuid project, project2, material, sample1, sample2, sample3, position1, position2, production, analysis1,
      analysis2, analysis3;
  ps::Actor reducer() const { return ps::Actor{lab.reducer, lab.reduction_client}; }
};

namespace detail {

template <class T>
Result<T> need(Result<T> r, const char* what) {
  if (!r) return fail(r.error().kind, std::string(what) + ": " + r.error().what);
  return r;
}

inline Result<void> publish_ref(ps::IStore& store, const ps::Actor& actor, ps::Uuid object, ps::RefPayload payload) {
  auto uow = store.begin(actor);
  if (!uow) return fail(uow.error());
  auto head = store.head(object, ps::Kind::RefValue);
  if (!head) return fail(head.error());
  if (auto r = (*uow)->add_revision(object, ps::Kind::RefValue, ps::RevisionPayload{std::move(payload)}, *head); !r)
    return fail(r.error());
  auto outcome = (*uow)->commit(ps::ChangesetKind::Reference, "<META> update");
  if (!outcome) return fail(outcome.error());
  if (!std::holds_alternative<ps::Committed>(*outcome)) return fail(ErrorKind::Config, "reference not committed");
  return {};
}

inline Result<ps::Uuid> ref_object(ps::IStore& store, const ps::testing::Lab& lab, ps::RefType type,
                                   const std::string& key, ps::RefObjectSpec scope) {
  scope.type = type;
  scope.key = key;
  return store.add_ref_object(lab.reduction_client, scope);
}

}  // namespace detail

// An unknown with five argon isotopes, as the store-source tests ingest one.
inline Result<ps::Uuid> ingest_unknown(ps::IStore& store, const ps::testing::Lab& lab, const std::string& identifier,
                                     int aliquot, const std::string& ts) {
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
  auto ok = store.ingest(ps::IngestItem{id, sha256(std::string_view{"payload-" + id.str()}), lab.acquisition_client, std::move(a)});
  if (!ok) return fail(ok.error());
  for (const auto& bytes : {signal, baseline}) {
    auto blob = store.ingest(ps::IngestItem{ps::Uuid::v7(), {}, lab.acquisition_client, ps::BlobIngest{"f32le-tv/1", bytes, 4}});
    if (!blob) return fail(blob.error());
  }
  return id;
}

inline Result<ReleaseSeed> seed_release_store(ps::IStore& store) {
  ReleaseSeed s;
  s.lab = ps::testing::seed_lab(store);
  const ps::Uuid c = s.lab.reduction_client;
  const ps::Uuid acq = s.lab.acquisition_client;
  auto project = detail::need(store.add_project(c, {"Alpha"}), "project Alpha");
  if (!project) return fail(project.error());
  s.project = *project;
  auto project2 = detail::need(store.add_project(c, {"Beta"}), "project Beta");
  if (!project2) return fail(project2.error());
  s.project2 = *project2;
  auto material = detail::need(store.add_material(c, {"sanidine", "60-80"}), "material");
  if (!material) return fail(material.error());
  s.material = *material;
  auto sample1 = detail::need(store.add_sample(c, {.name = "FC-1",
                                                   .project = s.project,
                                                   .material = s.material,
                                                   .igsn = "IEFC10001",
                                                   .lat = 37.75,
                                                   .lon = -106.9,
                                                   .elevation = 2850.0,
                                                   .lithology = "ash-flow tuff"}),
                              "sample FC-1");
  if (!sample1) return fail(sample1.error());
  s.sample1 = *sample1;
  auto sample2 = detail::need(store.add_sample(c, {"FC-2", s.project, s.material}), "sample FC-2");  // no location
  if (!sample2) return fail(sample2.error());
  s.sample2 = *sample2;
  auto sample3 = detail::need(
      store.add_sample(c, {.name = "BT-1", .project = s.project2, .material = s.material, .lat = 34.0, .lon = -106.9}),
      "sample BT-1");
  if (!sample3) return fail(sample3.error());
  s.sample3 = *sample3;
  // Positions 2 and 3 of the seeded level hold samples 1 and 2; sample 3's
  // identifier has no position (and so no J).
  auto position1 = detail::need(store.add_irradiation_position(acq, {s.lab.level, 2, s.sample1}), "position 2");
  if (!position1) return fail(position1.error());
  s.position1 = *position1;
  auto position2 = detail::need(store.add_irradiation_position(acq, {s.lab.level, 3, s.sample2}), "position 3");
  if (!position2) return fail(position2.error());
  s.position2 = *position2;
  for (auto r : {store.add_identifier(acq, {.identifier = "70001", .position = s.position1}),
                 store.add_identifier(acq, {.identifier = "70002", .position = s.position2}),
                 store.add_identifier(acq, {.identifier = "70003", .sample = s.sample3})})
    if (!r) return fail(r.error());

  // References: a flux per position, a production for the level, a chronology.
  const ps::Actor reducer = s.reducer();
  ps::RefObjectSpec pos1, pos2, lvl, irr;
  pos1.position = s.position1;
  pos2.position = s.position2;
  lvl.level = s.lab.level;
  irr.irradiation = s.lab.irradiation;
  for (const auto& [key, pos] : {std::pair{"NM-300/A/2", pos1}, std::pair{"NM-300/A/3", pos2}}) {
    ps::FluxValue flux;
    flux.j = 0.001;
    flux.j_err = 1e-6;
    flux.position_jerr = 2e-7;
    flux.monitor_name = "FC-2";
    flux.monitor_material = "sanidine";
    flux.monitor_age = 28.201;
    flux.monitor_age_err = 0.023;
    auto object = detail::ref_object(store, s.lab, ps::RefType::FluxPosition, key, pos);
    if (!object) return fail(object.error());
    if (auto r = detail::publish_ref(store, reducer, *object, flux); !r) return fail(r.error());
  }
  auto production = detail::ref_object(store, s.lab, ps::RefType::Production, "NM-300/Triga", {});
  if (!production) return fail(production.error());
  s.production = *production;
  if (auto r = detail::publish_ref(store, reducer, s.production,
                                   ps::ProductionValue{"Triga", std::nullopt, {{"K4039", 0.0008, 5e-5}, {"Ca3937", 0.0007, 1e-5}}});
      !r)
    return fail(r.error());
  auto level_production = detail::ref_object(store, s.lab, ps::RefType::LevelProduction, "NM-300/A", lvl);
  if (!level_production) return fail(level_production.error());
  if (auto r = detail::publish_ref(store, reducer, *level_production, ps::LevelProductionValue{s.production, std::nullopt}); !r)
    return fail(r.error());
  auto chronology = detail::ref_object(store, s.lab, ps::RefType::Chronology, "NM-300", irr);
  if (!chronology) return fail(chronology.error());
  if (auto r = detail::publish_ref(store, reducer, *chronology,
                                   ps::ChronologyValue{{{0, 1.0, *ps::UtcTime::parse("2026-01-01T00:00:00Z"),
                                                         *ps::UtcTime::parse("2026-01-01T10:00:00Z")}}});
      !r)
    return fail(r.error());

  for (auto [target, identifier, ts] : {std::tuple{&s.analysis1, "70001", "2026-10-02T10:00:00Z"},
                                        std::tuple{&s.analysis2, "70002", "2026-10-02T11:00:00Z"},
                                        std::tuple{&s.analysis3, "70003", "2026-10-02T12:00:00Z"}}) {
    auto id = ingest_unknown(store, s.lab, identifier, 1, ts);
    if (!id) return fail(id.error());
    *target = *id;
  }
  return s;
}

}  // namespace pychron::publish::testing
