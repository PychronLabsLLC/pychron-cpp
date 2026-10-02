// Reference data, interpreted ages and the derived cache (DVC schema spec,
// sections 4.3, 5.7, 6; invariant I14).

#include <gtest/gtest.h>

#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

class ReferenceTest : public StoreTest {
 protected:
  Actor reducer() const { return Actor{lab_.reducer, lab_.reduction_client}; }

  Uuid object(RefType type, const std::string& key, RefObjectSpec scope = {}) {
    scope.type = type;
    scope.key = key;
    auto id = store_->add_ref_object(lab_.reduction_client, scope);
    EXPECT_TRUE(id) << (id ? "" : to_string(id.error()));
    return id ? *id : Uuid{};
  }

  // Commits one value revision on top of the current head.
  Uuid publish(Uuid object, RefPayload payload) {
    auto uow = *store_->begin(reducer());
    auto rev = uow->add_revision(object, Kind::RefValue, RevisionPayload{std::move(payload)},
                                 *store_->head(object, Kind::RefValue));
    EXPECT_TRUE(rev) << (rev ? "" : to_string(rev.error()));
    auto outcome = uow->commit(ChangesetKind::Reference, "<META> update");
    EXPECT_TRUE(outcome && std::holds_alternative<Committed>(*outcome))
        << (outcome ? "conflict" : to_string(outcome.error()));
    return rev ? *rev : Uuid{};
  }

  Uuid ingest(int aliquot) {
    const auto item = analysis_item(lab_, aliquot, series(1), series(0));
    EXPECT_TRUE(store_->ingest(item));
    return std::get<AnalysisIngest>(item.body).analysis;
  }

  static FluxValue flux(double j) {
    FluxValue f;
    f.j = j;
    f.j_err = j * 1e-3;
    f.monitor_name = "FC-2";
    f.monitor_age = 28.201;
    f.options_json = R"({"model": "plane"})";
    f.analyses = {{std::nullopt, "66573-01", false}, {std::nullopt, "66573-02", true}};
    return f;
  }
};

}  // namespace

TEST_P(ReferenceTest, EveryReferenceTypeRoundTrips) {
  const Uuid prod = object(RefType::Production, "NM-300/Triga");
  const std::vector<std::pair<RefType, RefPayload>> cases = {
      {RefType::FluxPosition, flux(0.0123)},
      {RefType::LevelGeometry, LevelZValue{0.75}},
      {RefType::Production, ProductionValue{"Triga", "2024 values", {{"Ca3637", 0.000254, 1e-6}, {"K4039", 0.0008, 5e-5}}}},
      {RefType::LevelProduction, LevelProductionValue{prod, "default"}},
      {RefType::Chronology,
       ChronologyValue{{{0, 1.0, *UtcTime::parse("2024-01-02T03:04:05.123456Z"), *UtcTime::parse("2024-01-02T07:04:05Z")},
                        {1, 0.5, *UtcTime::parse("2024-01-03T00:00:00Z"), *UtcTime::parse("2024-01-03T01:30:00Z")}}}},
      {RefType::Gains, GainsValue{{{"AX", 1.0}, {"H1", 1.002}}}},
      {RefType::Sensitivity, SensitivityValue{4.1e-17, *UtcTime::parse("2023-05-06T07:08:09.5Z"), R"({"note": "x"})"}},
      {RefType::IrradiationHolder, HolderValue{"circle", 0.1, true, {{0, "1", 0.0, 1.0, 0.05}, {1, "2", 1.0, 0.0, std::nullopt}}}},
      {RefType::LoadHolder, HolderValue{std::nullopt, std::nullopt, false, {}}},
      {RefType::Script, ScriptValue{"def main():\n    sleep(1)\n"}},
      {RefType::Document, DocumentValue{std::nullopt, R"({"reactors": [1, 2]})"}},
  };
  int i = 0;
  for (const auto& [type, payload] : cases) {
    const Uuid obj = type == RefType::Production ? prod : object(type, "key-" + std::to_string(i++));
    const Uuid rev = publish(obj, payload);
    auto loaded = store_->load_payload(rev);
    ASSERT_TRUE(loaded && *loaded) << to_string(type) << (loaded ? "" : to_string(loaded.error()));
    EXPECT_EQ(std::get<RefPayload>(**loaded), payload) << to_string(type);
  }
}

TEST_P(ReferenceTest, PayloadMustMatchTheObjectsType) {
  const Uuid gains = object(RefType::Gains, "jan");
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(gains, Kind::RefValue, RevisionPayload{RefPayload{flux(1)}}, std::nullopt));
  auto outcome = uow->commit(ChangesetKind::Reference, "wrong type");
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().kind, ErrorKind::Protocol);
  EXPECT_FALSE(*store_->head(gains, Kind::RefValue));
}

TEST_P(ReferenceTest, ResolveFindsScopedHeadsAndTheLevelsProduction) {
  RefObjectSpec pos, lvl, irr, ms;
  pos.position = lab_.position;
  lvl.level = lab_.level;
  irr.irradiation = lab_.irradiation;
  ms.mass_spectrometer = lab_.mass_spectrometer;
  const Uuid fl = object(RefType::FluxPosition, "NM-300/A/1", pos);
  const Uuid lz = object(RefType::LevelGeometry, "NM-300/A", lvl);
  const Uuid lp = object(RefType::LevelProduction, "NM-300/A", lvl);
  const Uuid pr = object(RefType::Production, "NM-300/Triga");
  const Uuid ch = object(RefType::Chronology, "NM-300", irr);
  const Uuid gn = object(RefType::Gains, "jan", ms);
  const Uuid se = object(RefType::Sensitivity, "jan", ms);
  object(RefType::FluxPosition, "NM-300/A/2");  // unrelated, and never published
  const Uuid fl_rev = publish(fl, flux(0.01));
  publish(lz, LevelZValue{0.5});
  publish(lp, LevelProductionValue{pr, std::nullopt});
  const Uuid pr_rev = publish(pr, ProductionValue{"Triga", std::nullopt, {{"K4039", 0.0008, 5e-5}}});
  publish(ch, ChronologyValue{});
  publish(gn, GainsValue{{{"H1", 1.0}}});
  publish(se, SensitivityValue{4e-17, std::nullopt, std::nullopt});
  const Uuid a = ingest(1);

  auto res = store_->resolve_refs(a, RefPolicy{});
  ASSERT_TRUE(res) << to_string(res.error());
  ASSERT_EQ(res->refs.size(), 7u);
  std::map<RefType, ResolvedRef> by_type;
  for (const auto& r : res->refs) {
    EXPECT_FALSE(r.pinned);
    by_type[r.type] = r;
  }
  EXPECT_EQ(by_type.at(RefType::FluxPosition).revision, fl_rev);
  EXPECT_EQ(by_type.at(RefType::FluxPosition).key, "NM-300/A/1");
  EXPECT_EQ(by_type.at(RefType::Production).revision, pr_rev);
  EXPECT_EQ(by_type.at(RefType::Production).ref_object, pr);

  // An analysis whose identifier has no position sees only spectrometer refs.
  const auto other = analysis_item(lab_, 1, series(5), series(0), "66574");
  ASSERT_TRUE(store_->ingest(other));
  auto res2 = store_->resolve_refs(std::get<AnalysisIngest>(other.body).analysis, RefPolicy{});
  ASSERT_TRUE(res2);
  EXPECT_EQ(res2->refs.size(), 2u);
}

TEST_P(ReferenceTest, PinsFreezeAReferenceRevision) {
  RefObjectSpec pos;
  pos.position = lab_.position;
  const Uuid fl = object(RefType::FluxPosition, "NM-300/A/1", pos);
  const Uuid v1 = publish(fl, flux(0.01));
  const Uuid a = ingest(1);
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(a, Kind::RefPins, RefPins{{fl, v1}}, std::nullopt));
  ASSERT_TRUE(std::holds_alternative<Committed>(*uow->commit(ChangesetKind::Reduction, "<FLUX_FREEZE>")));
  const Uuid v2 = publish(fl, flux(0.02));

  auto pinned = *store_->resolve_refs(a, RefPolicy{true});
  ASSERT_EQ(pinned.refs.size(), 1u);
  EXPECT_EQ(pinned.refs[0].revision, v1);
  EXPECT_TRUE(pinned.refs[0].pinned);
  auto latest = *store_->resolve_refs(a, RefPolicy{false});
  EXPECT_EQ(latest.refs[0].revision, v2);
  EXPECT_FALSE(latest.refs[0].pinned);
}

TEST_P(ReferenceTest, DerivedValuesAreServedOnlyForCurrentInputs) {
  RefObjectSpec pos;
  pos.position = lab_.position;
  const Uuid fl = object(RefType::FluxPosition, "NM-300/A/1", pos);
  publish(fl, flux(0.01));
  const Uuid a = ingest(1);
  EXPECT_FALSE(*store_->get_derived(a, "red-1"));

  auto fp = store_->input_fingerprint(a, "red-1");
  ASSERT_TRUE(fp) << to_string(fp.error());
  EXPECT_EQ(*store_->input_fingerprint(a, "red-1"), *fp) << "deterministic";
  EXPECT_NE(*store_->input_fingerprint(a, "red-2"), *fp) << "reduction version is an input";
  const std::vector<DerivedRow> rows = {{"age", 28.1, 0.05, "Ma"}, {"kca", 12.0, 0.4, std::nullopt}};
  ASSERT_TRUE(store_->put_derived(a, *fp, "red-1", rows));
  ASSERT_TRUE(store_->put_derived(a, *fp, "red-1", rows)) << "re-insert is a no-op";
  auto got = store_->get_derived(a, "red-1");
  ASSERT_TRUE(got && *got);
  EXPECT_EQ(**got, rows);

  // A new flux head changes the inputs: the cached age is no longer served (I14).
  publish(fl, flux(0.02));
  EXPECT_FALSE(*store_->get_derived(a, "red-1"));
  EXPECT_EQ(*store_->prune_derived(a), 2);
  EXPECT_EQ(*store_->prune_derived(a), 0);

  // So does a new blank fit.
  auto fp2 = *store_->input_fingerprint(a, "red-1");
  ASSERT_TRUE(store_->put_derived(a, fp2, "red-1", rows));
  auto uow = *store_->begin(reducer());
  BlankRow b;
  b.isotope = "Ar40";
  b.value = 1;
  ASSERT_TRUE(uow->add_revision(a, Kind::Blanks, Blanks{b}, *store_->head(a, Kind::Blanks)));
  ASSERT_TRUE(std::holds_alternative<Committed>(*uow->commit(ChangesetKind::Reduction, "refit")));
  EXPECT_FALSE(*store_->get_derived(a, "red-1"));
}

TEST_P(ReferenceTest, InterpretedAgeRevisions) {
  const Uuid a = ingest(1), b = ingest(2);
  auto ia = store_->add_interpreted_age(lab_.reduction_client, {"66573 plateau", lab_.identifier, std::nullopt});
  ASSERT_TRUE(ia) << to_string(ia.error());
  InterpretedAgeValue v;
  v.age = 28.2;
  v.age_err = 0.03;
  v.age_kind = "Plateau";
  v.mswd = 1.1;
  v.nanalyses = 2;
  v.doc_json = R"({"name": "66573 plateau"})";
  v.members = {{a, "66573-01", true, "ok"}, {b, "66573-02", false, "invalid"}};
  std::sort(v.members.begin(), v.members.end(),
            [](const auto& x, const auto& y) { return x.analysis.str() < y.analysis.str(); });
  auto uow = *store_->begin(reducer());
  auto rev = uow->add_revision(*ia, Kind::InterpretedAge, v, std::nullopt);
  ASSERT_TRUE(rev);
  ASSERT_TRUE(std::holds_alternative<Committed>(*uow->commit(ChangesetKind::Reduction, "<IA>")));
  EXPECT_EQ(std::get<InterpretedAgeValue>(**store_->load_payload(*rev)), v);

  // A re-save is a new revision of the same interpreted age.
  v.age = 28.25;
  auto again = *store_->begin(reducer());
  ASSERT_TRUE(again->add_revision(*ia, Kind::InterpretedAge, v, *rev));
  ASSERT_TRUE(std::holds_alternative<Committed>(*again->commit(ChangesetKind::Reduction, "<IA>")));
  EXPECT_EQ(store_->history(*ia, Kind::InterpretedAge)->size(), 2u);
}

INSTANTIATE_TEST_SUITE_P(Engines, ReferenceTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
