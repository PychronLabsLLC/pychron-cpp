// Revisions, compare-and-swap heads, rollback (DVC schema spec, sections 5.4,
// 5.5; invariants I2, I4, I5).

#include <gtest/gtest.h>

#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

class RevisionTest : public StoreTest {
 protected:
  Uuid ingest(int aliquot) {
    const auto item = analysis_item(lab_, aliquot, series(static_cast<float>(aliquot)), series(0));
    auto ack = store_->ingest(item);
    EXPECT_TRUE(ack) << (ack ? "" : to_string(ack.error()));
    return std::get<AnalysisIngest>(item.body).analysis;
  }

  Uuid head_of(Uuid subject, Kind kind) { return **store_->head(subject, kind); }

  static Blanks blanks(double value) {
    BlankRow b;
    b.isotope = "Ar40";
    b.value = value;
    b.error = value / 10;
    b.fit = "average";
    b.references = {{0, std::nullopt, "66573-01", false}, {1, std::nullopt, "66573-02", true}};
    return {b};
  }

  Actor reducer() const { return Actor{lab_.reducer, lab_.reduction_client}; }
};

}  // namespace

TEST_P(RevisionTest, CommitMovesTheHeadAndKeepsHistory) {
  const Uuid a = ingest(1);
  const Uuid root = head_of(a, Kind::Blanks);
  auto uow = *store_->begin(reducer());
  auto rev = uow->add_revision(a, Kind::Blanks, blanks(0.7), root);
  ASSERT_TRUE(rev);
  auto outcome = uow->commit(ChangesetKind::Reduction, "<BLANKS> fit average");
  ASSERT_TRUE(outcome) << to_string(outcome.error());
  ASSERT_TRUE(std::holds_alternative<Committed>(*outcome));

  EXPECT_EQ(head_of(a, Kind::Blanks), *rev);
  auto history = *store_->history(a, Kind::Blanks);
  ASSERT_EQ(history.size(), 2u);
  EXPECT_EQ(history[0].uuid, root);
  EXPECT_EQ(history[1].uuid, *rev);
  EXPECT_EQ(history[1].parent, root);
  EXPECT_EQ(history[1].changeset.message, "<BLANKS> fit average");
  EXPECT_EQ(history[1].changeset.author_user, lab_.reducer);
  EXPECT_EQ(history[1].author_name, "jsmith");
  EXPECT_EQ(history[1].client_hostname, "red-1");
  EXPECT_EQ(history[0].author_name, "jross");  // the collection root, by the analyst
  EXPECT_EQ(history[1].change_seq, std::get<Committed>(*outcome).seq);
  EXPECT_EQ(std::get<Blanks>(**store_->load_payload(*rev)), blanks(0.7));
  // Other kinds are untouched: per-kind heads (5.2).
  EXPECT_EQ(store_->history(a, Kind::IcFactors)->size(), 1u);
}

TEST_P(RevisionTest, LosingWriterGetsTheWinnersIdentityAndLeavesNoRows) {
  const Uuid a = ingest(1);
  const Uuid root = head_of(a, Kind::Blanks);
  auto winner = *store_->begin(reducer());
  auto loser = *store_->begin(Actor{lab_.analyst, lab_.acquisition_client});
  auto win_rev = winner->add_revision(a, Kind::Blanks, blanks(1.0), root);
  ASSERT_TRUE(loser->add_revision(a, Kind::Blanks, blanks(2.0), root));
  const auto changes_before = store_->changes_since(0, 1000)->entries.size();

  auto won = winner->commit(ChangesetKind::Reduction, "winner");
  ASSERT_TRUE(won);
  ASSERT_TRUE(std::holds_alternative<Committed>(*won));
  auto lost = loser->commit(ChangesetKind::Reduction, "loser");
  ASSERT_TRUE(lost) << to_string(lost.error());
  ASSERT_TRUE(std::holds_alternative<std::vector<Conflict>>(*lost));
  const auto& conflicts = std::get<std::vector<Conflict>>(*lost);
  ASSERT_EQ(conflicts.size(), 1u);
  EXPECT_EQ(conflicts[0].subject, a);
  EXPECT_EQ(conflicts[0].kind, Kind::Blanks);
  EXPECT_EQ(conflicts[0].expected, root);
  EXPECT_EQ(conflicts[0].actual, *win_rev);
  ASSERT_TRUE(conflicts[0].actual_by);
  EXPECT_EQ(conflicts[0].actual_by->message, "winner");
  EXPECT_EQ(conflicts[0].actual_by->author_user, lab_.reducer);
  EXPECT_EQ(conflicts[0].actual_by->client, lab_.reduction_client);

  // I5: nothing of the losing changeset persists.
  EXPECT_EQ(store_->history(a, Kind::Blanks)->size(), 2u);
  EXPECT_EQ(store_->changes_since(0, 1000)->entries.size(), changes_before + 1);
}

TEST_P(RevisionTest, MultiSubjectChangesetIsAllOrNothing) {
  const Uuid a = ingest(1), b = ingest(2);
  const Uuid a_root = head_of(a, Kind::Blanks), b_root = head_of(b, Kind::Blanks);

  // Someone else moves b first.
  auto other = *store_->begin(reducer());
  ASSERT_TRUE(other->add_revision(b, Kind::Blanks, blanks(5), b_root));
  ASSERT_TRUE(std::holds_alternative<Committed>(*other->commit(ChangesetKind::Reduction, "b only")));

  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(a, Kind::Blanks, blanks(1), a_root));
  ASSERT_TRUE(uow->add_revision(b, Kind::Blanks, blanks(1), b_root));  // stale
  auto outcome = uow->commit(ChangesetKind::Reduction, "fit both");
  ASSERT_TRUE(outcome);
  ASSERT_TRUE(std::holds_alternative<std::vector<Conflict>>(*outcome));
  EXPECT_EQ(std::get<std::vector<Conflict>>(*outcome).size(), 1u);
  EXPECT_EQ(head_of(a, Kind::Blanks), a_root) << "a must not move when b conflicts";
  EXPECT_EQ(store_->history(a, Kind::Blanks)->size(), 1u);
}

TEST_P(RevisionTest, FirstHeadUsesInsertAndConflictsIfItAlreadyExists) {
  const Uuid a = ingest(1);
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(a, Kind::Annotation, AnnotationValue{"first comment"}, std::nullopt));
  auto first = uow->commit(ChangesetKind::Reduction, "<MANUAL> comment");
  ASSERT_TRUE(first) << to_string(first.error());
  ASSERT_TRUE(std::holds_alternative<Committed>(*first));

  auto again = *store_->begin(reducer());
  ASSERT_TRUE(again->add_revision(a, Kind::Annotation, AnnotationValue{"second"}, std::nullopt));
  auto second = again->commit(ChangesetKind::Reduction, "<MANUAL> comment");
  ASSERT_TRUE(second);
  ASSERT_TRUE(std::holds_alternative<std::vector<Conflict>>(*second));
  EXPECT_FALSE(std::get<std::vector<Conflict>>(*second)[0].expected);
  EXPECT_TRUE(std::get<std::vector<Conflict>>(*second)[0].actual);
}

TEST_P(RevisionTest, RollbackToCollectionRestoresTheRootValues) {
  const Uuid a = ingest(1);
  const Uuid root = head_of(a, Kind::Blanks);
  const auto root_payload = **store_->load_payload(root);
  Uuid head = root;
  for (double v : {1.0, 2.0, 3.0}) {
    auto uow = *store_->begin(reducer());
    head = *uow->add_revision(a, Kind::Blanks, blanks(v), head);
    ASSERT_TRUE(std::holds_alternative<Committed>(*uow->commit(ChangesetKind::Reduction, "refit")));
  }
  auto history = *store_->history(a, Kind::Blanks);
  ASSERT_EQ(history.size(), 4u);
  ASSERT_FALSE(history.front().parent);  // the collection root

  auto rollback = *store_->begin(reducer());
  ASSERT_TRUE(rollback->move_head(a, Kind::Blanks, head, history.front().uuid, MoveReason::CollectionRestore));
  auto outcome = rollback->commit(ChangesetKind::Rollback, "rollback to collection");
  ASSERT_TRUE(outcome) << to_string(outcome.error());
  ASSERT_TRUE(std::holds_alternative<Committed>(*outcome));

  // I4: values at head are exactly those shown when the root was head.
  auto view = *store_->load_analysis(a);
  EXPECT_EQ(view->payloads.at(Kind::Blanks), root_payload);
  EXPECT_EQ(store_->history(a, Kind::Blanks)->size(), 4u) << "rollback adds no revision";
}

TEST_P(RevisionTest, HeadCannotPointAtAnotherSubjectsRevision) {
  const Uuid a = ingest(1), b = ingest(2);
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->move_head(a, Kind::Blanks, head_of(a, Kind::Blanks), head_of(b, Kind::Blanks), MoveReason::Rollback));
  auto outcome = uow->commit(ChangesetKind::Rollback, "bad");
  ASSERT_FALSE(outcome);  // composite FK head -> revision(uuid, subject, kind), I2
  EXPECT_EQ(outcome.error().kind, ErrorKind::Protocol);
}

TEST_P(RevisionTest, StagingRules) {
  const Uuid a = ingest(1);
  const Uuid root = head_of(a, Kind::Blanks);
  auto uow = *store_->begin(reducer());
  EXPECT_FALSE(uow->add_revision(a, Kind::Blanks, AnnotationValue{"x"}, root)) << "payload/kind mismatch";
  ASSERT_TRUE(uow->add_revision(a, Kind::Blanks, blanks(1), root));
  EXPECT_FALSE(uow->add_revision(a, Kind::Blanks, blanks(2), root)) << "same subject/kind twice";
  EXPECT_FALSE(uow->move_head(a, Kind::Signals, root, root, MoveReason::Ingest));
  EXPECT_FALSE(store_->begin(Actor{}));
  auto empty = *store_->begin(reducer());
  EXPECT_FALSE(empty->commit(ChangesetKind::Reduction, "nothing"));
  auto collection = *store_->begin(reducer());
  ASSERT_TRUE(collection->add_revision(a, Kind::Tags, TagValue{"invalid", std::nullopt, std::nullopt},
                                       head_of(a, Kind::Tags)));
  EXPECT_FALSE(collection->commit(ChangesetKind::Collection, "x"));
}

TEST_P(RevisionTest, EveryPayloadKindRoundTrips) {
  const Uuid a = ingest(1);
  InterceptRow i;
  i.isotope = "H1:Ar40";  // peak-hop keys survive
  i.detector = "H1";
  i.value = 1.25e-12;
  i.error = std::nullopt;
  i.fit = "parabolic";
  i.error_type = "SEM";
  i.n = 120;
  i.fn = 118;
  i.include_baseline_error = true;
  i.filter_outliers_json = R"({"filter_outliers": true, "iterations": 1, "std_devs": 2})";
  i.user_excluded_json = "[1, 5]";
  i.outlier_excluded_json = "[]";
  i.reviewed = true;
  i.manual = {true, 1.3e-12, false, std::nullopt};
  i.extra_json = R"({"legacy": "x"})";
  BaselineRow bl;
  bl.detector = "H1";
  bl.value = 0.002;
  bl.modifier_value = 0.5;
  bl.modifier_error = 0.01;
  IcFactorRow ic;
  ic.detector = "CDD";
  ic.value = 1.02;
  ic.error = 0.003;
  ic.reference_detector = "H1";
  ic.standard_ratio = 295.5;
  ic.discrimination = true;
  ic.reference_data_json = R"({"a": 1})";
  ic.references = {{0, std::nullopt, "air-01", false}};

  const std::pair<Kind, RevisionPayload> cases[] = {
      {Kind::Intercepts, Intercepts{i}},
      {Kind::Baselines, Baselines{bl}},
      {Kind::IcFactors, IcFactors{ic}},
      {Kind::Signals,
       SignalRefs{{"sniff", "Ar40", "H1", blob_sha256(kCodecTv, series(9)), 4, 1, 3}}},
      {Kind::Tags, TagValue{"invalid", "bad pressure", R"({"name": "g1"})"}},
      {Kind::Annotation, AnnotationValue{"re-run: läuft"}},
      {Kind::Cosmogenic, CosmogenicValue{R"({"c": [1, 2]})"}},
  };
  for (const auto& [kind, payload] : cases) {
    auto uow = *store_->begin(reducer());
    auto expected = *store_->head(a, kind);
    auto rev = uow->add_revision(a, kind, payload, expected);
    ASSERT_TRUE(rev) << to_string(kind);
    auto outcome = uow->commit(ChangesetKind::Reduction, "round trip");
    ASSERT_TRUE(outcome) << to_string(kind) << ": " << to_string(outcome.error());
    ASSERT_TRUE(std::holds_alternative<Committed>(*outcome));
    auto loaded = store_->load_payload(*rev);
    ASSERT_TRUE(loaded && *loaded) << to_string(kind);
    if (store_->dialect() == Dialect::PostgreSql && kind == Kind::Intercepts) {
      // jsonb normalises; compare everything but the JSON text.
      auto got = std::get<Intercepts>(**loaded).at(0);
      EXPECT_EQ(got.value, i.value);
      EXPECT_EQ(got.manual, i.manual);
      EXPECT_EQ(got.isotope, i.isotope);
      EXPECT_TRUE(got.filter_outliers_json);
      continue;
    }
    EXPECT_EQ(**loaded, payload) << to_string(kind);
  }
}

INSTANTIATE_TEST_SUITE_P(Engines, RevisionTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
