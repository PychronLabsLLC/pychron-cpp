// Repositories, groups, bookmarks, rollback to collection and identity
// revisions (DVC schema spec, sections 3.6, 5.5, 5.6).

#include <gtest/gtest.h>

#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

class CollectionTest : public StoreTest {
 protected:
  Actor reducer() const { return Actor{lab_.reducer, lab_.reduction_client}; }

  Uuid ingest(int aliquot, const std::string& identifier = "66573") {
    const auto item = analysis_item(lab_, aliquot, series(static_cast<float>(aliquot)), series(0), identifier);
    auto ack = store_->ingest(item);
    EXPECT_TRUE(ack) << (ack ? "" : to_string(ack.error()));
    return std::get<AnalysisIngest>(item.body).analysis;
  }

  Uuid refit(Uuid a, Kind kind, double value) {
    auto uow = *store_->begin(reducer());
    Result<Uuid> rev = fail(ErrorKind::Protocol, "kind");
    if (kind == Kind::Blanks) {
      BlankRow b;
      b.isotope = "Ar40";
      b.value = value;
      rev = uow->add_revision(a, kind, Blanks{b}, *store_->head(a, kind));
    } else {
      IcFactorRow f;
      f.detector = "H1";
      f.value = value;
      rev = uow->add_revision(a, kind, IcFactors{f}, *store_->head(a, kind));
    }
    EXPECT_TRUE(rev);
    auto outcome = uow->commit(ChangesetKind::Reduction, "refit");
    EXPECT_TRUE(outcome && std::holds_alternative<Committed>(*outcome));
    return *rev;
  }

  std::map<std::pair<Uuid, Kind>, Uuid> all_heads(const std::vector<Uuid>& analyses) {
    std::map<std::pair<Uuid, Kind>, Uuid> out;
    for (const auto& a : analyses) {
      const std::vector<HeadInfo> heads = *store_->heads(a);
      for (const auto& h : heads) out[{a, h.kind}] = h.revision;
    }
    return out;
  }
};

}  // namespace

TEST_P(CollectionTest, RepositoryBookmarkRestoresEveryHead) {
  const Uuid a = ingest(1), b = ingest(2);
  const Uuid repo = *store_->add_repository(lab_.reduction_client, "Project-X");
  ASSERT_TRUE(store_->add_repository_members(reducer(), repo, {a, b}));
  refit(a, Kind::Blanks, 1);
  const auto before = all_heads({a, b});
  auto bm = store_->create_bookmark(reducer(), {"submitted", "as submitted", repo, std::nullopt});
  ASSERT_TRUE(bm) << to_string(bm.error());
  EXPECT_EQ(store_->bookmark_heads(*bm)->size(), before.size());

  refit(a, Kind::Blanks, 2);
  refit(b, Kind::IcFactors, 1.1);
  ASSERT_NE(all_heads({a, b}), before);
  const auto blanks_history = store_->history(a, Kind::Blanks)->size();

  auto restored = store_->restore_bookmark(reducer(), *bm, "restore submitted");
  ASSERT_TRUE(restored) << to_string(restored.error());
  ASSERT_TRUE(std::holds_alternative<Committed>(*restored));
  EXPECT_FALSE(std::get<Committed>(*restored).changeset.is_nil());
  EXPECT_EQ(all_heads({a, b}), before);
  EXPECT_EQ(store_->history(a, Kind::Blanks)->size(), blanks_history) << "restore adds no revisions";

  auto again = store_->restore_bookmark(reducer(), *bm, "no-op");
  ASSERT_TRUE(again);
  EXPECT_TRUE(std::get<Committed>(*again).changeset.is_nil()) << "nothing differs";
}

TEST_P(CollectionTest, GroupBookmarkAndScopeRules) {
  const Uuid a = ingest(1);
  const Uuid group = *store_->create_group(reducer(), "plateau", {a, a});
  auto bm = store_->create_bookmark(reducer(), {"g", std::nullopt, std::nullopt, group});
  ASSERT_TRUE(bm) << to_string(bm.error());
  EXPECT_EQ(store_->bookmark_heads(*bm)->size(), std::size(kCollectionKinds));
  EXPECT_FALSE(store_->create_bookmark(reducer(), {"none", std::nullopt, std::nullopt, std::nullopt}));
  EXPECT_FALSE(store_->create_bookmark(reducer(), {"both", std::nullopt, group, group}));
  EXPECT_FALSE(store_->restore_bookmark(reducer(), Uuid::v7(), "unknown"));
}

TEST_P(CollectionTest, RollbackToCollectionMovesEveryKindToItsRoot) {
  const Uuid a = ingest(1);
  const auto roots = all_heads({a});
  refit(a, Kind::Blanks, 1);
  refit(a, Kind::IcFactors, 1.01);
  refit(a, Kind::Blanks, 2);
  auto outcome = store_->rollback_to_collection(reducer(), a, "rollback to collection");
  ASSERT_TRUE(outcome) << to_string(outcome.error());
  ASSERT_TRUE(std::holds_alternative<Committed>(*outcome));
  EXPECT_EQ(all_heads({a}), roots);
  auto again = store_->rollback_to_collection(reducer(), a, "again");
  EXPECT_TRUE(std::get<Committed>(*again).changeset.is_nil());

  refit(a, Kind::Blanks, 3);
  ASSERT_TRUE(store_->rollback_to_collection(reducer(), a, "ic factors only", {Kind::IcFactors}));
  EXPECT_NE(**store_->head(a, Kind::Blanks), roots.at({a, Kind::Blanks}));
  EXPECT_FALSE(store_->rollback_to_collection(reducer(), a, "x", {Kind::Annotation}))
      << "annotation has no collection revision";
}

TEST_P(CollectionTest, IdentityRevisionRenumbersTheAnalysisRow) {
  const Uuid a = ingest(1);
  auto uow = *store_->begin(Actor{lab_.reducer, lab_.reduction_client});
  auto first = uow->add_revision(a, Kind::Identity, IdentityValue{lab_.identifier2, 5, 0, "admin_repair"}, std::nullopt);
  ASSERT_TRUE(first);
  auto outcome = uow->commit(ChangesetKind::Admin, "fix identifier");
  ASSERT_TRUE(outcome) << to_string(outcome.error());
  ASSERT_TRUE(std::holds_alternative<Committed>(*outcome));
  auto view = **store_->load_analysis(a);
  EXPECT_EQ(view.summary.runid, "66574-05A");
  EXPECT_EQ(view.summary.identifier, "66574");
  EXPECT_EQ(std::get<IdentityValue>(view.payloads.at(Kind::Identity)).reason, "admin_repair");
  AnalysisQuery by_identifier;
  by_identifier.identifier = "66574";
  EXPECT_EQ(store_->find_analyses(by_identifier)->size(), 1u);

  auto second = *store_->begin(reducer());
  ASSERT_TRUE(second->add_revision(a, Kind::Identity, IdentityValue{lab_.identifier, 9, -1, "admin_repair"}, *first));
  ASSERT_TRUE(std::holds_alternative<Committed>(*second->commit(ChangesetKind::Admin, "again")));
  EXPECT_EQ((*store_->load_analysis(a))->summary.runid, "66573-09");

  // Rolling the identity head back re-applies that revision's identity.
  auto back = *store_->begin(reducer());
  ASSERT_TRUE(back->move_head(a, Kind::Identity, *store_->head(a, Kind::Identity), *first, MoveReason::Rollback));
  ASSERT_TRUE(std::holds_alternative<Committed>(*back->commit(ChangesetKind::Rollback, "undo")));
  EXPECT_EQ((*store_->load_analysis(a))->summary.runid, "66574-05A");
}

TEST_P(CollectionTest, IdentityClashIsRejectedAndChangesNothing) {
  const Uuid a = ingest(1);
  ingest(2);
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(a, Kind::Identity, IdentityValue{lab_.identifier, 2, -1, "admin_repair"}, std::nullopt));
  auto outcome = uow->commit(ChangesetKind::Admin, "collide");
  ASSERT_FALSE(outcome);  // UNIQUE (identifier, aliquot, increment), I9
  EXPECT_EQ(outcome.error().kind, ErrorKind::Protocol);
  EXPECT_EQ((*store_->load_analysis(a))->summary.runid, "66573-01");
  EXPECT_FALSE(*store_->head(a, Kind::Identity));
}

INSTANTIATE_TEST_SUITE_P(Engines, CollectionTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
