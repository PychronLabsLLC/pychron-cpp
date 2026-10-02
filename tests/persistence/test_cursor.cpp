// Change cursor (DVC schema spec, section 9; invariant I6) and concurrent
// compare-and-swap through separate connections (I5).

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;
namespace pd = pychron::persistence::detail;

namespace {

class CursorTest : public StoreTest {};

}  // namespace

TEST_P(CursorTest, EveryWriteGetsTheNextSeqAndPagesInOrder) {
  auto all = store_->changes_since(0, 1000);
  ASSERT_TRUE(all) << to_string(all.error());
  ASSERT_EQ(all->entries.size(), static_cast<std::size_t>(kSeedLabChanges));
  for (std::size_t i = 0; i < all->entries.size(); ++i) {
    EXPECT_EQ(all->entries[i].seq, static_cast<ChangeSeq>(i + 1));
    EXPECT_EQ(all->entries[i].kind, "catalog");
    EXPECT_EQ(all->entries[i].entities.size(), 1u);
  }
  EXPECT_EQ(all->entries[0].entities[0].entity_type, "client");
  EXPECT_EQ(all->entries[0].client, lab_.acquisition_client);
  EXPECT_FALSE(all->more);

  ASSERT_TRUE(store_->ingest(analysis_item(lab_, 1, series(1), series(0))));
  ChangeSeq cursor = 0;
  std::vector<ChangeSeq> seen;
  for (;;) {
    auto page = store_->changes_since(cursor, 2);
    ASSERT_TRUE(page);
    for (const auto& e : page->entries) seen.push_back(e.seq);
    cursor = page->cursor;
    if (!page->more) break;
  }
  std::vector<ChangeSeq> expected;
  for (ChangeSeq i = 1; i <= kSeedLabChanges + 1; ++i) expected.push_back(i);
  EXPECT_EQ(seen, expected);
  EXPECT_EQ(cursor, kSeedLabChanges + 1);
  auto empty = store_->changes_since(cursor, 10);
  EXPECT_TRUE(empty->entries.empty());
  EXPECT_EQ(empty->cursor, cursor);
}

TEST_P(CursorTest, CatalogChangesCarryAFieldDiff) {
  // D6: catalog rows are audited by change_entity.detail; read it raw, so the
  // database must be reachable from a second connection.
  TestDatabase shared(GetParam(), true);
  auto store = open_or_die(shared.url());
  ASSERT_TRUE(store);
  const Lab lab = seed_lab(*store);
  auto db = pd::Db::open(StoreConfig{shared.url(), false});
  ASSERT_TRUE(db);
  auto row = (*db)->select_one(
      "SELECT detail FROM change_entity WHERE entity_type = 'mass_spectrometer' AND entity_uuid = ?",
      {pd::qv(lab.mass_spectrometer)});
  ASSERT_TRUE(row && *row);
  const std::string diff = pd::to_std((*row)->value("detail"));
  EXPECT_NE(diff.find("\"name\": [null, \"jan\"]"), std::string::npos) << diff;
}

TEST_P(CursorTest, ConcurrentWritersRacingOnOneHeadProduceExactlyOneCommit) {
  TestDatabase shared(GetParam(), true);
  auto setup = open_or_die(shared.url());
  ASSERT_TRUE(setup);
  const Lab lab = seed_lab(*setup);
  const auto item = analysis_item(lab, 1, series(1), series(0));
  ASSERT_TRUE(setup->ingest(item));
  const Uuid a = std::get<AnalysisIngest>(item.body).analysis;
  const Uuid root = **setup->head(a, Kind::Blanks);

  constexpr int kWriters = 6;
  std::atomic<int> committed{0}, conflicted{0}, failed{0};
  std::atomic<int> ready{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      auto store = open_store(StoreConfig{shared.url(), false});
      if (!store) {
        ++failed;
        return;
      }
      auto uow = *(*store)->begin(Actor{lab.reducer, lab.reduction_client});
      BlankRow b;
      b.isotope = "Ar40";
      b.value = w;
      (void)uow->add_revision(a, Kind::Blanks, Blanks{b}, root);
      ++ready;
      while (ready.load() < kWriters) std::this_thread::yield();
      auto outcome = uow->commit(ChangesetKind::Reduction, "writer " + std::to_string(w));
      if (!outcome)
        ++failed;
      else if (std::holds_alternative<Committed>(*outcome))
        ++committed;
      else
        ++conflicted;
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(failed.load(), 0);
  EXPECT_EQ(committed.load(), 1);
  EXPECT_EQ(conflicted.load(), kWriters - 1);
  EXPECT_EQ(setup->history(a, Kind::Blanks)->size(), 2u);

  // change_seq is gap-free and strictly increasing across all writers (I6).
  auto page = setup->changes_since(0, 1000);
  for (std::size_t i = 0; i < page->entries.size(); ++i)
    EXPECT_EQ(page->entries[i].seq, static_cast<ChangeSeq>(i + 1));
}

INSTANTIATE_TEST_SUITE_P(Engines, CursorTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });

TEST(RunId, MatchesLegacyMakeRunid) {
  EXPECT_EQ(make_runid("66573", 7, -1), "66573-07");
  EXPECT_EQ(make_runid("66573", 7, 0), "66573-07A");
  EXPECT_EQ(make_runid("66573", 12, 25), "66573-12Z");
  EXPECT_EQ(make_runid("66573", 112, 26), "66573-112AA");
  EXPECT_EQ(make_runid("bu-FD-J", 3, 27), "bu-FD-J-03AB");
}
