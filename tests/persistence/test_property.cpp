// Seeded random sequences of commits, stale commits, rollbacks, collection
// restores, bookmarks and bookmark restores, checked step by step against an
// in-memory model (DVC schema spec, section 12.6 "property/"):
//   I2  every head is a revision of its own subject and kind
//   I4  the payload at a head is exactly the payload that revision was made with
//   I5  a commit with a stale expected head conflicts and leaves no rows
//   I6  change_seq is gap-free and increasing

#include <gtest/gtest.h>

#include <map>
#include <random>

#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

class PropertyTest : public StoreTest {};

struct Model {
  std::map<std::pair<Uuid, Kind>, Uuid> heads;
  std::map<std::pair<Uuid, Kind>, std::vector<Uuid>> history;
  std::map<Uuid, RevisionPayload> payloads;
  std::vector<std::pair<Uuid, std::map<std::pair<Uuid, Kind>, Uuid>>> bookmarks;
  std::size_t changes = 0;
};

RevisionPayload random_payload(Kind kind, std::mt19937& rng) {
  std::uniform_real_distribution<double> value(0.0, 10.0);
  if (kind == Kind::Blanks) {
    BlankRow b;
    b.isotope = "Ar40";
    b.value = value(rng);
    b.error = value(rng) / 100;
    return Blanks{b};
  }
  IcFactorRow f;
  f.detector = "H1";
  f.value = 1 + value(rng) / 100;
  return IcFactors{f};
}

}  // namespace

TEST_P(PropertyTest, RandomOperationSequencesKeepTheInvariants) {
  constexpr int kSteps = 120;
  std::mt19937 rng(20261002);
  const Actor actor{lab_.reducer, lab_.reduction_client};

  Model model;
  model.changes = static_cast<std::size_t>(kSeedLabChanges);
  std::vector<Uuid> analyses;
  for (int i = 1; i <= 3; ++i) {
    const auto item = analysis_item(lab_, i, series(static_cast<float>(i)), series(0));
    ASSERT_TRUE(store_->ingest(item));
    ++model.changes;
    const Uuid a = std::get<AnalysisIngest>(item.body).analysis;
    analyses.push_back(a);
    for (Kind k : kCollectionKinds) {
      const Uuid root = **store_->head(a, k);
      model.heads[{a, k}] = root;
      model.history[{a, k}] = {root};
      model.payloads.emplace(root, **store_->load_payload(root));
    }
  }
  const Uuid group = *store_->create_group(actor, "all", analyses);
  ++model.changes;

  const Kind edited[] = {Kind::Blanks, Kind::IcFactors};
  std::map<std::string, int> seen;  // coverage: every operation must actually happen
  auto pick = [&](auto n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };

  for (int step = 0; step < kSteps; ++step) {
    const Uuid a = analyses[pick(analyses.size())];
    const Kind k = edited[pick(std::size(edited))];
    const auto key = std::make_pair(a, k);
    const int op = static_cast<int>(pick(10));
    SCOPED_TRACE("step " + std::to_string(step) + " op " + std::to_string(op));

    if (op < 5) {  // commit, sometimes against a stale head
      const bool stale = model.history[key].size() > 1 && pick(4) == 0;
      const Uuid expected = stale ? model.history[key].front() : model.heads[key];
      RevisionPayload payload = random_payload(k, rng);
      auto uow = *store_->begin(actor);
      auto rev = uow->add_revision(a, k, payload, expected);
      ASSERT_TRUE(rev);
      auto outcome = uow->commit(ChangesetKind::Reduction, "step");
      ASSERT_TRUE(outcome) << to_string(outcome.error());
      if (stale && expected != model.heads[key]) {
        ASSERT_TRUE(std::holds_alternative<std::vector<Conflict>>(*outcome));
        EXPECT_FALSE(*store_->load_payload(*rev)) << "a conflicted revision must not persist (I5)";
        ++seen["conflict"];
      } else {
        ASSERT_TRUE(std::holds_alternative<Committed>(*outcome));
        model.heads[key] = *rev;
        model.history[key].push_back(*rev);
        model.payloads.emplace(*rev, payload);
        ++model.changes;
        ++seen["commit"];
      }
    } else if (op < 7) {  // rollback to a random earlier revision
      const auto& h = model.history[key];
      const Uuid to = h[pick(h.size())];
      if (to == model.heads[key]) continue;
      auto uow = *store_->begin(actor);
      ASSERT_TRUE(uow->move_head(a, k, model.heads[key], to, MoveReason::Rollback));
      auto outcome = uow->commit(ChangesetKind::Rollback, "rollback");
      ASSERT_TRUE(outcome && std::holds_alternative<Committed>(*outcome));
      model.heads[key] = to;
      ++model.changes;
      ++seen["rollback"];
    } else if (op == 7) {  // rollback to collection
      auto outcome = store_->rollback_to_collection(actor, a, "to collection");
      ASSERT_TRUE(outcome && std::holds_alternative<Committed>(*outcome));
      bool moved = false;
      for (Kind c : kCollectionKinds) {
        moved |= model.heads[{a, c}] != model.history[{a, c}].front();
        model.heads[{a, c}] = model.history[{a, c}].front();
      }
      EXPECT_EQ(moved, !std::get<Committed>(*outcome).changeset.is_nil());
      if (moved) ++model.changes;
      seen["collection"] += moved;
    } else if (op == 8) {  // bookmark
      auto bm = store_->create_bookmark(actor, {"bm" + std::to_string(step), std::nullopt, std::nullopt, group, std::nullopt});
      ASSERT_TRUE(bm);
      model.bookmarks.emplace_back(*bm, model.heads);
      ++model.changes;
      ++seen["bookmark"];
    } else if (!model.bookmarks.empty()) {  // restore a bookmark
      const auto& [bm, heads] = model.bookmarks[pick(model.bookmarks.size())];
      auto outcome = store_->restore_bookmark(actor, bm, "restore");
      ASSERT_TRUE(outcome && std::holds_alternative<Committed>(*outcome));
      if (model.heads != heads) ++model.changes;
      EXPECT_EQ(model.heads != heads, !std::get<Committed>(*outcome).changeset.is_nil());
      seen["restore"] += model.heads != heads;
      model.heads = heads;
    }

    // Invariants after every step.
    for (const auto& [hk, rev] : model.heads) {
      auto head = store_->head(hk.first, hk.second);
      ASSERT_TRUE(head && *head);
      ASSERT_EQ(**head, rev) << to_string(hk.second);
      const auto& h = model.history[hk];
      ASSERT_NE(std::find(h.begin(), h.end(), **head), h.end()) << "I2";
      ASSERT_EQ(store_->history(hk.first, hk.second)->size(), h.size());
    }
    const Uuid probe = analyses[pick(analyses.size())];
    auto view = *store_->load_analysis(probe);
    for (Kind e : edited) ASSERT_EQ(view->payloads.at(e), model.payloads.at(model.heads[{probe, e}])) << "I4";
  }

  for (const char* op : {"commit", "conflict", "rollback", "collection", "bookmark", "restore"})
    EXPECT_GT(seen[op], 0) << op << " never exercised; change the seed or step count";

  auto page = store_->changes_since(0, 100000);
  ASSERT_EQ(page->entries.size(), model.changes);
  for (std::size_t i = 0; i < page->entries.size(); ++i)
    ASSERT_EQ(page->entries[i].seq, static_cast<ChangeSeq>(i + 1)) << "I6";
}

INSTANTIATE_TEST_SUITE_P(Engines, PropertyTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
