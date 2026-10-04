#include <gtest/gtest.h>

#include "pychron/entry/settings.hpp"
#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::entry;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

TEST(Settings, DefaultsUnknownKeysAndTypes) {
  auto s = parse_settings("{}");
  ASSERT_TRUE(s);
  EXPECT_EQ(*s, EntrySettings{});
  s = parse_settings(R"({"package_prefix": "P-", "j_multiplier": 2e-4, "future": {"x": 1}, "pi_names_allowed": ["NMGRL"]})");
  ASSERT_TRUE(s);
  EXPECT_EQ(s->package_prefix, "P-");
  EXPECT_EQ(s->j_multiplier, 2e-4);
  EXPECT_EQ(s->pi_names_allowed, (std::vector<std::string>{"NMGRL"}));
  auto again = parse_settings(to_json(*s));
  ASSERT_TRUE(again);
  EXPECT_EQ(*again, *s);  // the unknown key survives a round trip
  EXPECT_NE(to_json(*s).find("future"), std::string::npos);
  EXPECT_FALSE(parse_settings(R"({"package_prefix": 3})"));
  EXPECT_FALSE(parse_settings(R"({"default_package_kind": "argon"})"));
  EXPECT_FALSE(parse_settings("[]"));
}

namespace {
class SettingsStoreTest : public StoreTest {};
}  // namespace

TEST_P(SettingsStoreTest, SaveLoadAndConflict) {
  const Actor actor{lab_.reducer, lab_.reduction_client};
  auto first = load_settings(*store_);
  ASSERT_TRUE(first);
  EXPECT_FALSE(first->ref_object);
  EXPECT_EQ(first->settings, EntrySettings{});
  EntrySettings s;
  s.package_prefix = "X-";
  auto saved = save_settings(*store_, actor, s, *first);
  ASSERT_TRUE(saved && std::holds_alternative<Committed>(*saved));
  auto loaded = load_settings(*store_);
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->settings.package_prefix, "X-");
  // A save made from the stale first load conflicts.
  LoadedSettings stale = *loaded;
  stale.head = std::nullopt;
  auto conflict = save_settings(*store_, actor, s, stale);
  ASSERT_TRUE(conflict);
  EXPECT_FALSE(std::holds_alternative<Committed>(*conflict));
}

INSTANTIATE_TEST_SUITE_P(Engines, SettingsStoreTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
