#pragma once

// A catalog for the entry tests (sample and package entry spec, section 5),
// on top of seed_lab: PIs, projects, materials, samples, and a second package
// "NM-301" whose levels are inserted B before A.

#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "store_fixture.hpp"

namespace pychron::persistence::testing {

struct EntryCatalog {
  Uuid ross, smith;
  Uuid p_ross1, p_ross2, p_smith;
  Uuid sanidine, biotite;
  Uuid fc2, s1, s2, s3, s4;  // fc2: monitor, in p_ross1
  Uuid nm301, level_a, level_b;
  Uuid pos_a1, pos_a2, pos_b1;
};

inline EntryCatalog seed_entry(IStore& store, const Lab& lab) {
  const Uuid c = lab.reduction_client;
  EntryCatalog e;
  e.ross = *store.add_principal_investigator(c, {"Ross", "J", std::nullopt, std::nullopt, std::nullopt});
  e.smith = *store.add_principal_investigator(c, {"Smith", "", std::nullopt, std::nullopt, std::nullopt});
  e.p_ross1 = *store.add_project(c, {"Alpha", e.ross});
  e.p_ross2 = *store.add_project(c, {"Beta", e.ross});
  e.p_smith = *store.add_project(c, {"Gamma", e.smith});
  e.sanidine = *store.add_material(c, {"sanidine", "", std::nullopt});
  e.biotite = *store.add_material(c, {"biotite", "20-40", std::nullopt});
  e.fc2 = *store.add_sample(c, {"FC-2", e.p_ross1, e.sanidine});
  e.s1 = *store.add_sample(c, {"bt-1", e.p_ross1, e.biotite});
  e.s2 = *store.add_sample(c, {"bt-2", e.p_ross2, e.biotite});
  e.s3 = *store.add_sample(c, {"san_10%", e.p_smith, e.sanidine});
  e.s4 = *store.add_sample(c, {"Other", e.p_smith, e.sanidine});
  e.nm301 = *store.add_irradiation(c, IrradiationSpec{"NM-301", std::nullopt, std::nullopt});
  e.level_b = *store.add_level(c, {e.nm301, "B", std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  e.level_a = *store.add_level(c, {e.nm301, "A", std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  e.pos_a2 = *store.add_irradiation_position(c, {e.level_a, 2, e.s1, 1.5, std::string("P2"), std::nullopt, std::nullopt});
  e.pos_a1 = *store.add_irradiation_position(c, {e.level_a, 1, e.fc2, std::nullopt, std::string("P1"), std::nullopt, std::nullopt});
  e.pos_b1 = *store.add_irradiation_position(c, {e.level_b, 1, e.s2, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  return e;
}

class EntryTest : public StoreTest {
 protected:
  void SetUp() override {
    StoreTest::SetUp();
    if (HasFatalFailure()) return;
    cat_ = seed_entry(*store_, lab_);
  }
  Uuid client() const { return lab_.reduction_client; }
  Actor reducer() const { return Actor{lab_.reducer, lab_.reduction_client}; }

  // Ingests one analysis of `identifier` (which must exist).
  Uuid analyze(const std::string& identifier, int aliquot = 1) {
    const auto item = analysis_item(lab_, aliquot, series(1), series(0), identifier);
    auto ack = store_->ingest(item);
    EXPECT_TRUE(ack) << (ack ? "" : to_string(ack.error()));
    return std::get<AnalysisIngest>(item.body).analysis;
  }

  CatalogOutcome apply(CatalogEditBatch batch) {
    auto r = store_->apply_catalog_edits(client(), batch);
    EXPECT_TRUE(r) << (r ? "" : to_string(r.error()));
    return r ? *r : CatalogOutcome{std::vector<Refusal>{}};
  }

  EntryCatalog cat_;
};

inline bool applied(const CatalogOutcome& o) { return std::holds_alternative<CatalogApplied>(o); }

inline std::string describe(const CatalogOutcome& o) {
  if (std::holds_alternative<CatalogApplied>(o)) return "applied";
  if (const auto* s = std::get_if<std::vector<StaleRow>>(&o)) return "stale x" + std::to_string(s->size());
  if (const auto* r = std::get_if<std::vector<Refusal>>(&o)) {
    std::string out = "refused:";
    for (const auto& x : *r) out += " [" + x.rule + ": " + x.what + "]";
    return out;
  }
  return "ref conflict";
}

}  // namespace pychron::persistence::testing
