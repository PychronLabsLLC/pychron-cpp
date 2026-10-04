// Sequential identifier allocation (sample and package entry spec, section 5.4).

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "catalog_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

class AllocationTest : public EntryTest {
 protected:
  Result<AllocationOutcome> allocate(std::int64_t last, std::vector<IdentifierAssignment> a) {
    return store_->allocate_identifiers(client(), IdentifierAllocation{last, std::move(a)});
  }
  std::optional<std::string> identifier_of(Uuid level, int position) {
    auto sheet = store_->level_sheet(level);
    for (const auto& p : (**sheet).positions)
      if (p.position == position) return p.identifier;
    return std::nullopt;
  }
  std::int64_t counter() { return store_->identifier_counter(std::string(kIdentifierScope))->value_or(-1); }
};

bool applied(const AllocationOutcome& o) { return std::holds_alternative<CatalogApplied>(o); }

}  // namespace

TEST_P(AllocationTest, SeedRules) {
  for (const char* id : {"01234", "bu-FD-J", "12a", "1234567890123456789"})
    ASSERT_TRUE(store_->add_identifier(client(), {id, "unknown", std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt}));
  // seed_lab holds 66573 and 66574: the next is 66575.
  auto out = allocate(66574, {{cat_.pos_a1, 66575, std::nullopt}});
  ASSERT_TRUE(out) << to_string(out.error());
  ASSERT_TRUE(applied(*out));
  EXPECT_EQ(identifier_of(cat_.level_a, 1), "66575");
  EXPECT_EQ(counter(), 66575);
}

TEST_P(AllocationTest, EmptyStoreSeedsAtZero) {
  TestDatabase empty(GetParam(), false);
  auto store = open_or_die(empty.url());
  ASSERT_TRUE(store);
  const Uuid c = *store->register_client({"h", "reduction", std::nullopt, "t"});
  const Uuid pkg = *store->add_irradiation(c, "NM-1");
  const Uuid level = *store->add_level(c, {pkg, "A", std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  const Uuid pos = *store->add_irradiation_position(c, {level, 1, std::nullopt, std::nullopt, {}, {}, std::nullopt});
  auto out = store->allocate_identifiers(c, IdentifierAllocation{0, {{pos, 1, std::nullopt}}});
  ASSERT_TRUE(out && applied(*out));
  EXPECT_EQ(*store->find_identifier("1") != std::nullopt, true);
}

TEST_P(AllocationTest, StaleCounter) {
  ASSERT_TRUE(applied(*allocate(66574, {{cat_.pos_a1, 66575, std::nullopt}})));
  const ChangeSeq before = *store_->latest_change_seq();
  auto out = allocate(66574, {{cat_.pos_a2, 66575, std::nullopt}});
  ASSERT_TRUE(out);
  const auto* stale = std::get_if<AllocationStale>(&*out);
  ASSERT_TRUE(stale);
  EXPECT_EQ(stale->actual_last, 66575);
  EXPECT_FALSE(identifier_of(cat_.level_a, 2));
  EXPECT_EQ(*store_->latest_change_seq(), before);
}

TEST_P(AllocationTest, RaceOneWins) {
  TestDatabase shared(GetParam(), true);
  auto setup = open_or_die(shared.url());
  ASSERT_TRUE(setup);
  const Lab lab = seed_lab(*setup);
  const EntryCatalog cat = seed_entry(*setup, lab);
  std::atomic<int> wins{0}, stale{0};
  std::atomic<bool> errors{false};
  auto writer = [&](Uuid position) {
    auto store = open_store(StoreConfig{shared.url(), false});
    if (!store) {
      errors = true;
      return;
    }
    auto out = (*store)->allocate_identifiers(lab.reduction_client, IdentifierAllocation{66574, {{position, 66575, std::nullopt}}});
    if (!out) {
      errors = true;
      return;
    }
    if (applied(*out)) ++wins;
    if (std::holds_alternative<AllocationStale>(*out)) ++stale;
  };
  std::thread a(writer, cat.pos_a1), b(writer, cat.pos_a2);
  a.join();
  b.join();
  ASSERT_FALSE(errors);
  EXPECT_EQ(wins.load(), 1);
  EXPECT_EQ(stale.load(), 1);
}

TEST_P(AllocationTest, NonSequentialIsError) {
  EXPECT_FALSE(allocate(66574, {{cat_.pos_a1, 66576, std::nullopt}}));  // a gap
  EXPECT_FALSE(allocate(66574, {{cat_.pos_a1, 66575, std::nullopt}, {cat_.pos_a2, 66575, std::nullopt}}));  // a repeat
  EXPECT_FALSE(allocate(66574, {{cat_.pos_a1, 66574, std::nullopt}}));  // at last
  EXPECT_FALSE(allocate(66574, {{cat_.pos_a1, 66575, std::nullopt}, {cat_.pos_a1, 66576, std::nullopt}}));  // one position twice
  EXPECT_EQ(counter(), -1);
}

TEST_P(AllocationTest, DuplicateTextRefused) {
  // A hand-entered numeric identifier sits where the counter goes next.
  ASSERT_TRUE(store_->add_identifier(client(), {"66576", "special", std::string("blank_unknown"), std::nullopt, std::nullopt, std::nullopt, std::nullopt}));
  // The seed is now 66576; a plan made from 66574 is stale.
  auto stale = allocate(66574, {{cat_.pos_a1, 66575, std::nullopt}});
  ASSERT_TRUE(stale && std::holds_alternative<AllocationStale>(*stale));
  EXPECT_EQ(std::get<AllocationStale>(*stale).actual_last, 66576);
  // Once the counter exists, a later hand-entered number in the way is refused.
  ASSERT_TRUE(applied(*allocate(66576, {{cat_.pos_a1, 66577, std::nullopt}})));
  ASSERT_TRUE(store_->add_identifier(client(), {"66578", "special", std::string("air"), std::nullopt, std::nullopt, std::nullopt, std::nullopt}));
  auto out = allocate(66577, {{cat_.pos_a2, 66578, std::nullopt}});
  ASSERT_TRUE(out);
  const auto* refused = std::get_if<std::vector<Refusal>>(&*out);
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->front().rule, "unique");
  EXPECT_EQ(counter(), 66577);
}

TEST_P(AllocationTest, ReplaceInPlaceKeepsUuid) {
  const Uuid old = *store_->add_identifier(client(), {"70001", "unknown", std::nullopt, std::nullopt, cat_.pos_a1, std::nullopt, std::nullopt});
  const std::int64_t last = *store_->max_numeric_identifier();
  ASSERT_EQ(last, 70001);
  auto out = allocate(last, {{cat_.pos_a1, 70002, old}});
  ASSERT_TRUE(out && applied(*out));
  EXPECT_EQ(*store_->find_identifier("70002"), old);
  EXPECT_FALSE(*store_->find_identifier("70001"));
  // A replace that does not name the current identifier is refused.
  auto wrong = allocate(70002, {{cat_.pos_a1, 70003, std::nullopt}});
  ASSERT_TRUE(wrong);
  ASSERT_TRUE(std::holds_alternative<std::vector<Refusal>>(*wrong));
  EXPECT_EQ(std::get<std::vector<Refusal>>(*wrong).front().rule, "stale_identifier");
}

TEST_P(AllocationTest, ReplaceAnalyzedRefused) {
  const Uuid old = *store_->add_identifier(client(), {"70001", "unknown", std::nullopt, std::nullopt, cat_.pos_a1, std::nullopt, std::nullopt});
  analyze("70001");
  auto out = allocate(70001, {{cat_.pos_a1, 70002, old}});
  ASSERT_TRUE(out);
  const auto* refused = std::get_if<std::vector<Refusal>>(&*out);
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->front().rule, "analyzed_identifier");
  EXPECT_EQ(*store_->find_identifier("70001"), old);
}

TEST_P(AllocationTest, OneRefusalWritesNothing) {
  const Uuid analyzed = *store_->add_identifier(client(), {"70001", "unknown", std::nullopt, std::nullopt, cat_.pos_b1, std::nullopt, std::nullopt});
  analyze("70001");
  auto out = allocate(70001, {{cat_.pos_a1, 70002, std::nullopt}, {cat_.pos_b1, 70003, analyzed}, {cat_.pos_a2, 70004, std::nullopt}});
  ASSERT_TRUE(out);
  EXPECT_TRUE(std::holds_alternative<std::vector<Refusal>>(*out));
  EXPECT_FALSE(identifier_of(cat_.level_a, 1));
  EXPECT_FALSE(identifier_of(cat_.level_a, 2));
  EXPECT_EQ(counter(), -1);
}

TEST_P(AllocationTest, OverwrittenNumbersNotReused) {
  ASSERT_TRUE(applied(*allocate(66574, {{cat_.pos_a1, 66575, std::nullopt}})));
  const Uuid id = **store_->find_identifier("66575");
  ASSERT_TRUE(applied(*allocate(66575, {{cat_.pos_a1, 66576, id}})));
  EXPECT_EQ(counter(), 66576);
  EXPECT_FALSE(*store_->find_identifier("66575"));
  // 66575 is free text now, but the counter only moves up.
  EXPECT_FALSE(allocate(66576, {{cat_.pos_a2, 66575, std::nullopt}}));
}

TEST_P(AllocationTest, AfterImportContinuesAboveMax) {
  // An import writes identifiers through add_identifier and never touches the
  // counter; the first allocation seeds above them.
  for (const char* id : {"12000", "61234", "bu-FD-J"})
    ASSERT_TRUE(store_->add_identifier(client(), {id, "unknown", std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt}));
  EXPECT_FALSE(*store_->identifier_counter(std::string(kIdentifierScope)));
  const std::int64_t last = *store_->max_numeric_identifier();
  EXPECT_EQ(last, 66574);
  ASSERT_TRUE(applied(*allocate(last, {{cat_.pos_a1, last + 1, std::nullopt}, {cat_.pos_a2, last + 2, std::nullopt}})));
  EXPECT_EQ(identifier_of(cat_.level_a, 2), "66576");
}

INSTANTIATE_TEST_SUITE_P(Engines, AllocationTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
