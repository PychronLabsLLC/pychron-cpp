// Reference data, interpreted ages and the derived cache (DVC schema spec,
// sections 4.3, 5.7, 6; invariant I14).

#include <algorithm>
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

// A loader that has read an analysis's heads and resolved its references
// has the fingerprint without asking the store again.
TEST_P(ReferenceTest, TheFingerprintOfWhatWasReadIsTheInputFingerprint) {
  RefObjectSpec pos;
  pos.position = lab_.position;
  const Uuid fl = object(RefType::FluxPosition, "NM-300/A/1", pos);
  publish(fl, flux(0.01));
  const Uuid a = ingest(1);
  const auto of_what_was_read = [&] {
    auto heads = store_->heads(a);
    EXPECT_TRUE(heads);
    auto refs = store_->resolve_refs(a, RefPolicy{});
    EXPECT_TRUE(refs);
    return fingerprint_of(*heads, refs->refs, "red-1");
  };
  const Sha256Digest first = of_what_was_read();
  EXPECT_EQ(first, *store_->input_fingerprint(a, "red-1"));
  EXPECT_NE(first, *store_->input_fingerprint(a, "red-2"));

  publish(fl, flux(0.02));
  const Sha256Digest second = of_what_was_read();
  EXPECT_NE(second, first);
  EXPECT_EQ(second, *store_->input_fingerprint(a, "red-1"));

  // The order the heads were read in is no part of it.
  auto heads = *store_->heads(a);
  ASSERT_GT(heads.size(), 1u);
  std::reverse(heads.begin(), heads.end());
  EXPECT_EQ(fingerprint_of(heads, store_->resolve_refs(a, RefPolicy{})->refs, "red-1"), second);
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

  // So does a new blank fit. A put under the new fingerprint takes the old
  // one's rows with it: nothing is left to prune.
  auto fp2 = *store_->input_fingerprint(a, "red-1");
  ASSERT_TRUE(store_->put_derived(a, fp2, "red-1", rows));
  EXPECT_EQ(*store_->prune_derived(a), 0);
  auto uow = *store_->begin(reducer());
  BlankRow b;
  b.isotope = "Ar40";
  b.value = 1;
  ASSERT_TRUE(uow->add_revision(a, Kind::Blanks, Blanks{b}, *store_->head(a, Kind::Blanks)));
  ASSERT_TRUE(std::holds_alternative<Committed>(*uow->commit(ChangesetKind::Reduction, "refit")));
  EXPECT_FALSE(*store_->get_derived(a, "red-1"));
}

// What the age sink asks before it writes: the fingerprint each of a batch of
// analyses is cached under, in one read (age cache spec 4.5).
TEST_P(ReferenceTest, TheCachedFingerprintsOfManyAnalysesAreReadTogether) {
  RefObjectSpec pos;
  pos.position = lab_.position;
  publish(object(RefType::FluxPosition, "NM-300/A/1", pos), flux(0.01));
  const Uuid a = ingest(1);
  const Uuid b = ingest(2);
  const Uuid never = ingest(3);
  const std::vector<Uuid> all{a, b, never, Uuid::v7()};  // the last is no analysis at all

  auto none = store_->derived_fingerprints(all);
  ASSERT_TRUE(none) << to_string(none.error());
  EXPECT_TRUE(none->empty());
  EXPECT_TRUE(store_->derived_fingerprints({})->empty());

  const auto fa = *store_->input_fingerprint(a, "red-1");
  Sha256Digest fb{};
  fb[0] = 7;
  ASSERT_TRUE(store_->put_derived(a, fa, "red-1", {{"age", 28.1, 0.05, "Ma"}, {"kca", 12.0, 0.4, std::nullopt}}));
  // "current, and there is no age" is cached too: a null age
  ASSERT_TRUE(store_->put_derived(b, fb, "red-1", {{"age", std::nullopt, std::nullopt, "Ma"}}));

  auto got = store_->derived_fingerprints(all);
  ASSERT_TRUE(got) << to_string(got.error());
  ASSERT_EQ(got->size(), 2u);
  EXPECT_EQ(got->at(a), fa);
  EXPECT_EQ(got->at(b), fb);
  EXPECT_FALSE(got->contains(never));

  // More analyses than one statement is given: read in parts, the same answer.
  std::vector<Uuid> many;
  many.reserve(1202);
  for (int i = 0; i < 1200; ++i) many.push_back(Uuid::v7());
  many.push_back(a);
  many.insert(many.begin(), b);
  got = store_->derived_fingerprints(many);
  ASSERT_TRUE(got) << to_string(got.error());
  EXPECT_EQ(got->size(), 2u);
  EXPECT_EQ(got->at(a), fa);
}

// After any write the cache holds one fingerprint's rows for an analysis
// (age cache spec 4.2): a join from an analysis to its age is one row or none.
TEST_P(ReferenceTest, APutReplacesAnotherFingerprintsRows) {
  RefObjectSpec pos;
  pos.position = lab_.position;
  const Uuid fl = object(RefType::FluxPosition, "NM-300/A/1", pos);
  publish(fl, flux(0.01));
  const Uuid a = ingest(1);
  const Uuid other = ingest(2);
  const auto fp1 = *store_->input_fingerprint(a, "red-1");
  const auto other_fp = *store_->input_fingerprint(other, "red-1");
  ASSERT_TRUE(store_->put_derived(a, fp1, "red-1", {{"age", 28.1, 0.05, "Ma"}, {"kca", 12.0, 0.4, std::nullopt}}));
  ASSERT_TRUE(store_->put_derived(other, other_fp, "red-1", {{"age", 1.0, 0.1, "Ma"}}));

  publish(fl, flux(0.02));
  const auto fp2 = *store_->input_fingerprint(a, "red-1");
  ASSERT_NE(fp2, fp1);
  const std::vector<DerivedRow> second = {{"age", 56.0, 0.1, "Ma"}};
  ASSERT_TRUE(store_->put_derived(a, fp2, "red-1", second));
  auto got = store_->get_derived(a, "red-1");
  ASSERT_TRUE(got && *got);
  EXPECT_EQ(**got, second) << "the first fingerprint's kca went with its age";
  EXPECT_EQ(*store_->prune_derived(a), 0) << "nothing of the first fingerprint was left";
  // Another analysis's rows are not touched.
  const auto other_fp2 = *store_->input_fingerprint(other, "red-1");
  EXPECT_EQ(*store_->prune_derived(other), other_fp2 == other_fp ? 0 : 1);
}

TEST_P(ReferenceTest, ManyPutsAreOneTransaction) {
  const Uuid a = ingest(1), b = ingest(2), c = ingest(3);
  const auto fp = [&](Uuid u) { return *store_->input_fingerprint(u, "red-1"); };
  const std::vector<DerivedRow> rows = {{"age", 28.1, 0.05, "Ma"}};
  const std::vector<DerivedPut> three = {{a, fp(a), rows}, {b, fp(b), rows}, {c, fp(c), {{"age", 3.0, 0.3, "Ma"}}}};
  ASSERT_TRUE(store_->put_derived_many(three, "red-1"));
  for (const Uuid u : {a, b, c}) {
    auto got = store_->get_derived(u, "red-1");
    ASSERT_TRUE(got && *got);
    EXPECT_EQ((*got)->size(), 1u);
  }
  EXPECT_EQ((**store_->get_derived(c, "red-1"))[0].value, std::optional<double>(3.0));

  // One put that cannot be made fails the call and leaves none of it.
  const Uuid d = ingest(4), e = ingest(5);
  const Uuid nobody = Uuid::v7();
  const std::vector<DerivedPut> bad = {{d, fp(d), rows}, {nobody, fp(d), rows}, {e, fp(e), rows}};
  EXPECT_FALSE(store_->put_derived_many(bad, "red-1"));
  EXPECT_FALSE(*store_->get_derived(d, "red-1"));
  EXPECT_FALSE(*store_->get_derived(e, "red-1"));
  ASSERT_TRUE(store_->put_derived_many({}, "red-1")) << "nothing to put is not an error";
}

TEST_P(ReferenceTest, InterpretedAgeRevisions) {
  const Uuid a = ingest(1), b = ingest(2);
  auto ia = store_->add_interpreted_age(lab_.reduction_client, {"66573 plateau", lab_.identifier, std::nullopt, std::nullopt});
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

TEST_P(ReferenceTest, InterpretedAgeWithCallerIdIsEnsured) {
  const Uuid id = *Uuid::parse("5a5a5a5a-1111-5111-8111-5a5a5a5a5a5a");
  auto first = store_->add_interpreted_age(lab_.reduction_client, {"66573 plateau", lab_.identifier, std::nullopt, id});
  ASSERT_TRUE(first) << to_string(first.error());
  EXPECT_EQ(*first, id);
  const auto seq = *store_->latest_change_seq();

  // The same id again: the stored row is returned and nothing is written,
  // whatever else the spec says.
  auto again = store_->add_interpreted_age(lab_.reduction_client, {"renamed", std::nullopt, std::nullopt, id});
  ASSERT_TRUE(again) << to_string(again.error());
  EXPECT_EQ(*again, id);
  EXPECT_EQ(*store_->latest_change_seq(), seq);

  // Without an id every call still makes a new interpreted age.
  auto other = store_->add_interpreted_age(lab_.reduction_client, {"66573 plateau", lab_.identifier, std::nullopt, std::nullopt});
  ASSERT_TRUE(other);
  EXPECT_NE(*other, id);
  EXPECT_GT(*store_->latest_change_seq(), seq);
}

INSTANTIATE_TEST_SUITE_P(Engines, ReferenceTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
