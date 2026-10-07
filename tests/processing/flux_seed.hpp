#pragma once

// A seeded irradiation level for the flux tests: NM-300 level A on a 12-hole
// holder. Holes 1-8 are the ring of the flux golden data and hold FC-2
// (identifiers 66001..66008, three analyses each); holes 9-12 are the golden
// prediction points and hold the sample `unk` (66101..66104, no analyses).
// Free functions over a store, with no test framework in them: the
// processing fixture wraps them and the elctl command tests call them.
//
// The monitor analyses reduce to a known F. With the FC-2 (Kuiper 2008) set,
// the three analyses of ring hole i give J, J(1 + d) and J(1 - d) for the
// golden J of that hole (d = kSeedSpread), so their arithmetic mean is the
// golden J. Ar36, Ar37 and Ar38 are zero, K4039 is zero and the analyses are
// run hours after the irradiation, so F is Ar40 / Ar39 to about a part in a
// million (the Ar39 decay over those hours).

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../reduction/flux_golden.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/flux_store.hpp"

namespace pychron::processing::testing {

inline constexpr double kSeedSpread = 2e-3;
inline constexpr double kSeedAr39 = 100.0;

struct SeededLevel {
  persistence::Uuid acquisition_client, mass_spectrometer, irradiation, level, holder, unknown_sample;
  std::string irradiation_name = "NM-300", level_name = "A", holder_name = "12-hole";
  std::map<int, persistence::Uuid> positions;             // by hole
  std::map<std::string, persistence::Uuid> analyses;      // by record id ("66001-01")
};

// F of a monitor with that J under the FC-2 (Kuiper 2008) set.
inline double seed_f(double j) {
  const auto c = default_monitor_sets().sets[0].constants();
  return (std::exp(c.lambda_k * c.age_a) - 1.0) / j;
}

// The J the seed gives analysis `aliquot` (1..3) of ring hole `hole` (1..8).
inline double seed_j(int hole, int aliquot) {
  const double j = flux_golden::kRing[hole - 1].j;
  return aliquot == 2 ? j * (1 + kSeedSpread) : aliquot == 3 ? j * (1 - kSeedSpread) : j;
}

// The 12 holes: the golden ring, then the golden prediction points. Position
// N of the level is the hole with ordinal N - 1; the id is its label.
inline std::vector<persistence::HolderHole> seed_holes() {
  std::vector<persistence::HolderHole> holes;
  int n = 1;
  for (const auto& m : flux_golden::kRing) {
    holes.push_back({n - 1, std::to_string(n), m.x, m.y, 1.0});
    ++n;
  }
  for (const auto& p : flux_golden::kPoints) {
    holes.push_back({n - 1, std::to_string(n), p.x, p.y, 1.0});
    ++n;
  }
  return holes;
}

// A new revision of a reference object on top of its head.
inline Result<persistence::Uuid> seed_publish(persistence::IStore& store, const persistence::Actor& actor,
                                              persistence::Uuid object, persistence::RefPayload payload,
                                              const std::string& message = "seed") {
  namespace ps = persistence;
  auto head = store.head(object, ps::Kind::RefValue);
  if (!head) return fail(head.error());
  auto uow = store.begin(actor);
  if (!uow) return fail(uow.error());
  auto revision = (*uow)->add_revision(object, ps::Kind::RefValue, ps::RevisionPayload{std::move(payload)}, *head);
  if (!revision) return fail(revision.error());
  auto outcome = (*uow)->commit(ps::ChangesetKind::Reference, message);
  if (!outcome) return fail(outcome.error());
  if (!std::holds_alternative<ps::Committed>(*outcome)) return fail(ErrorKind::Protocol, "seed: the publish conflicted");
  return *revision;
}

// The holder's geometry (a new revision of the seeded holder).
inline Result<persistence::Uuid> seed_publish_holder(persistence::IStore& store, const persistence::Actor& actor,
                                                     const SeededLevel& level,
                                                     std::vector<persistence::HolderHole> holes) {
  persistence::HolderValue holder;
  holder.shape = "circle";
  holder.has_hole_numbers = true;
  holder.holes = std::move(holes);
  return seed_publish(store, actor, level.holder, holder);
}

// One monitor analysis: the five argon isotopes on one detector, with no
// baseline, blank or IC factor, so F is Ar40 / Ar39.
inline Result<persistence::Uuid> seed_ingest_monitor(persistence::IStore& store, const SeededLevel& level,
                                                     const std::string& identifier, int aliquot, double f,
                                                     const std::string& timestamp) {
  namespace ps = persistence;
  ps::AnalysisIngest a;
  a.analysis = ps::Uuid::v7();
  a.changeset = ps::Uuid::v7();
  a.created = ps::UtcTime::now();
  a.identifier = identifier;
  a.aliquot = aliquot;
  a.analysis_type = "unknown";
  a.timestamp = *ps::UtcTime::parse(timestamp);
  a.mass_spectrometer = "jan";
  a.extract_device = "co2";
  a.analyst = "jross";
  for (const char* iso : {"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"})
    a.isotopes.push_back({iso, "AX", "fA", std::nullopt, std::nullopt, std::nullopt});
  a.detectors = {{"AX", std::nullopt, std::nullopt}};
  auto& r = a.roots;
  r.signals = ps::Uuid::v7();
  r.intercepts = ps::Uuid::v7();
  r.baselines = ps::Uuid::v7();
  r.blanks = ps::Uuid::v7();
  r.icfactors = ps::Uuid::v7();
  r.tags = ps::Uuid::v7();
  const auto intercept = [](const char* iso, double v, double e) {
    ps::InterceptRow row;
    row.isotope = iso;
    row.detector = "AX";
    row.value = v;
    row.error = e;
    row.fit = "Linear";
    row.error_type = "SEM";
    row.n = 100;
    return row;
  };
  const double ar40 = f * kSeedAr39;
  r.intercepts_rows = {intercept("Ar40", ar40, ar40 * 5e-4), intercept("Ar39", kSeedAr39, kSeedAr39 * 5e-4),
                       intercept("Ar38", 0.0, 1e-4), intercept("Ar37", 0.0, 1e-4), intercept("Ar36", 0.0, 1e-4)};
  const ps::Uuid id = a.analysis;
  auto ok = store.ingest(
      ps::IngestItem{id, sha256(std::string_view{"payload-" + id.str()}), level.acquisition_client, std::move(a)});
  if (!ok) return fail(ok.error());
  return id;
}

// The three analyses of ring hole `hole` (1..8), run at 1<hole>:01 .. :03.
inline Result<void> seed_monitor_analyses(persistence::IStore& store, SeededLevel& level, int hole) {
  for (int aliquot = 1; aliquot <= 3; ++aliquot) {
    const std::string identifier = std::to_string(66000 + hole);
    const std::string timestamp =
        "2026-01-01T1" + std::to_string(hole) + ":0" + std::to_string(aliquot) + ":00Z";  // 11:01 .. 18:03
    auto analysis = seed_ingest_monitor(store, level, identifier, aliquot, seed_f(seed_j(hole, aliquot)), timestamp);
    if (!analysis) return fail(analysis.error());
    level.analyses[identifier + "-0" + std::to_string(aliquot)] = *analysis;
  }
  return {};
}

// The whole level. `actor` publishes the references (holder, production,
// chronology); the catalog rows and the analyses belong to a new acquisition
// client, "acq-1". `monitor_sample` names the sample of holes 1-8; only the
// first `analysed` of them get their three analyses (seed_monitor_analyses
// gives a hole its own later).
inline Result<SeededLevel> seed_flux_level(persistence::IStore& store, const persistence::Actor& actor,
                                           const std::string& monitor_sample = "FC-2", int analysed = 8) {
  namespace ps = persistence;
#define PYCHRON_SEED_TRY(var, expr) \
  auto var = (expr);                \
  if (!var) return fail(var.error())

  SeededLevel out;
  PYCHRON_SEED_TRY(acq, store.register_client({"acq-1", "acquisition", std::nullopt, "test"}));
  out.acquisition_client = *acq;
  PYCHRON_SEED_TRY(ms, store.add_mass_spectrometer(*acq, {"jan", "argus", "j", std::nullopt}));
  out.mass_spectrometer = *ms;
  PYCHRON_SEED_TRY(device, store.add_extract_device(*acq, "co2"));
  PYCHRON_SEED_TRY(irradiation, store.add_irradiation(*acq, out.irradiation_name));
  out.irradiation = *irradiation;

  ps::RefObjectSpec holder_spec;
  holder_spec.type = ps::RefType::IrradiationHolder;
  holder_spec.key = out.holder_name;
  PYCHRON_SEED_TRY(holder, store.add_ref_object(actor.client, holder_spec));
  out.holder = *holder;
  PYCHRON_SEED_TRY(holder_revision, seed_publish_holder(store, actor, out, seed_holes()));

  PYCHRON_SEED_TRY(level, store.add_level(*acq, {*irradiation, out.level_name, *holder, 0.5, std::nullopt, std::nullopt}));
  out.level = *level;

  PYCHRON_SEED_TRY(pi, store.add_principal_investigator(*acq, {"Ross", "J", std::nullopt, std::nullopt, std::nullopt}));
  PYCHRON_SEED_TRY(project, store.add_project(*acq, {"Fish Canyon", *pi, std::nullopt}));
  PYCHRON_SEED_TRY(material, store.add_material(*acq, {"sanidine", "60-80", std::nullopt}));
  PYCHRON_SEED_TRY(monitor, store.add_sample(*acq, {.name = monitor_sample, .project = *project, .material = *material}));
  PYCHRON_SEED_TRY(unknown, store.add_sample(*acq, {.name = "unk", .project = *project, .material = *material}));

  out.unknown_sample = *unknown;

  for (int hole = 1; hole <= 12; ++hole) {
    const bool ring = hole <= 8;
    PYCHRON_SEED_TRY(position, store.add_irradiation_position(
                                   *acq, {*level, hole, ring ? *monitor : *unknown, std::nullopt, {}, {}, std::nullopt}));
    out.positions[hole] = *position;
    const std::string identifier = std::to_string(ring ? 66000 + hole : 66100 + hole - 8);
    PYCHRON_SEED_TRY(added, store.add_identifier(*acq, {identifier, "unknown", std::nullopt, std::nullopt, *position,
                                                        std::nullopt, std::nullopt}));
  }

  // What the reduction needs for F: production ratios and the chronology.
  ps::RefObjectSpec production_spec, level_scope, irradiation_scope;
  production_spec.type = ps::RefType::Production;
  production_spec.key = out.irradiation_name + "/Triga";
  PYCHRON_SEED_TRY(production, store.add_ref_object(actor.client, production_spec));
  PYCHRON_SEED_TRY(production_revision,
                   seed_publish(store, actor, *production,
                                ps::ProductionValue{"Triga", std::nullopt, {{"K4039", 0.0, 0.0}, {"Ca3937", 0.0007, 1e-5}}}));
  level_scope.type = ps::RefType::LevelProduction;
  level_scope.key = out.irradiation_name + "/" + out.level_name;
  level_scope.level = *level;
  PYCHRON_SEED_TRY(level_production, store.add_ref_object(actor.client, level_scope));
  PYCHRON_SEED_TRY(level_production_revision,
                   seed_publish(store, actor, *level_production, ps::LevelProductionValue{*production, std::nullopt}));
  irradiation_scope.type = ps::RefType::Chronology;
  irradiation_scope.key = out.irradiation_name;
  irradiation_scope.irradiation = *irradiation;
  PYCHRON_SEED_TRY(chronology, store.add_ref_object(actor.client, irradiation_scope));
  PYCHRON_SEED_TRY(chronology_revision,
                   seed_publish(store, actor, *chronology,
                                ps::ChronologyValue{{{0, 1.0, *ps::UtcTime::parse("2026-01-01T00:00:00Z"),
                                                      *ps::UtcTime::parse("2026-01-01T10:00:00Z")}}}));

  for (int hole = 1; hole <= analysed; ++hole) {
    PYCHRON_SEED_TRY(ingested, seed_monitor_analyses(store, out, hole));
  }
#undef PYCHRON_SEED_TRY
  return out;
}

// A second level of the seeded irradiation, on the same holder, with one
// unknown (identifier 66201) and no monitor: a level that cannot be fitted.
inline Result<void> seed_level_without_monitors(persistence::IStore& store, const SeededLevel& seeded,
                                                const std::string& name) {
  namespace ps = persistence;
  auto level = store.add_level(seeded.acquisition_client, {seeded.irradiation, name, seeded.holder, 0.5, std::nullopt, std::nullopt});
  if (!level) return fail(level.error());
  auto position = store.add_irradiation_position(seeded.acquisition_client,
                                                 {*level, 1, seeded.unknown_sample, std::nullopt, {}, {}, std::nullopt});
  if (!position) return fail(position.error());
  auto added = store.add_identifier(seeded.acquisition_client,
                                    {"66201", "unknown", std::nullopt, std::nullopt, *position, std::nullopt, std::nullopt});
  if (!added) return fail(added.error());
  return {};
}

// A flux_position revision of a hole, shaped as the importer and save_level
// shape the reference object (key `<irrad>/<level>/<pos>`, scoped to the
// position). Returns the revision.
inline Result<persistence::Uuid> seed_save_flux(persistence::IStore& store, const persistence::Actor& actor,
                                                const SeededLevel& level, int hole, persistence::FluxValue value) {
  namespace ps = persistence;
  const auto position = level.positions.find(hole);
  if (position == level.positions.end())
    return fail(ErrorKind::Config, "seed: no hole " + std::to_string(hole));
  const std::string key = level.irradiation_name + "/" + level.level_name + "/" + std::to_string(hole);
  auto found = store.find_catalog_row(ps::CatalogTable::RefObject, {std::string("flux_position"), key});
  if (!found) return fail(found.error());
  ps::Uuid object;
  if (*found) {
    object = **found;
  } else {
    ps::RefObjectSpec spec;
    spec.type = ps::RefType::FluxPosition;
    spec.key = key;
    spec.irradiation = level.irradiation;
    spec.level = level.level;
    spec.position = position->second;
    auto made = store.add_ref_object(actor.client, spec);
    if (!made) return fail(made.error());
    object = *made;
  }
  return seed_publish(store, actor, object, std::move(value), "fit flux for " + level.irradiation_name + level.level_name);
}

// Tags an analysis of the level, named by record id ("66001-02").
inline Result<void> seed_tag(persistence::IStore& store, const persistence::Actor& actor, const SeededLevel& level,
                             const std::string& record_id, const std::string& name) {
  namespace ps = persistence;
  const auto analysis = level.analyses.find(record_id);
  if (analysis == level.analyses.end()) return fail(ErrorKind::Config, "seed: no analysis " + record_id);
  auto head = store.head(analysis->second, ps::Kind::Tags);
  if (!head) return fail(head.error());
  auto uow = store.begin(actor);
  if (!uow) return fail(uow.error());
  auto revision = (*uow)->add_revision(analysis->second, ps::Kind::Tags,
                                       ps::RevisionPayload{ps::TagValue{name, std::nullopt, std::nullopt}}, *head);
  if (!revision) return fail(revision.error());
  auto outcome = (*uow)->commit(ps::ChangesetKind::Reduction, "tag");
  if (!outcome) return fail(outcome.error());
  if (!std::holds_alternative<ps::Committed>(*outcome)) return fail(ErrorKind::Protocol, "seed: the tag conflicted");
  return {};
}

}  // namespace pychron::processing::testing
