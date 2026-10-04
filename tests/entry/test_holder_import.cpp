#include <gtest/gtest.h>

#include "pychron/entry/holder_import.hpp"
#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::entry;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

TEST(HolderImport, LegacyFiles) {
  auto plain = read_holder("circle,0.0175\n0,0\n0.1,0\n# a comment\n0.2,0,0.02\n");
  ASSERT_TRUE(plain) << to_string(plain.error());
  ASSERT_EQ(plain->holes.size(), 3u);
  EXPECT_EQ(plain->holes[0].hole_id, "1");
  EXPECT_EQ(plain->holes[2].hole_id, "4");  // the comment line still counts (legacy)
  EXPECT_EQ(plain->holes[2].radius, 0.02);
  auto numbered = read_holder("circle,0.02,True\n7,0,0\n9,1,0\n");
  ASSERT_TRUE(numbered);
  EXPECT_TRUE(numbered->has_hole_numbers);
  EXPECT_EQ(numbered->holes[1].hole_id, "9");
  EXPECT_FALSE(read_holder("circle,0.02,True\n7,0,0\n7,1,0\n"));  // a hole twice
  EXPECT_FALSE(read_holder("circle,0.02\n"));                  // no holes
  EXPECT_FALSE(read_holder(""));
}

namespace {
class HolderStoreTest : public StoreTest {};
}  // namespace

TEST_P(HolderStoreTest, SaveAndLoad) {
  const Actor actor{lab_.reducer, lab_.reduction_client};
  auto h = read_holder("circle,0.0175\n0,0\n0.1,0\n");
  ASSERT_TRUE(h);
  auto id = save_holder(*store_, actor, "24-hole", *h);
  ASSERT_TRUE(id) << to_string(id.error());
  auto loaded = load_holder(*store_, *id);
  ASSERT_TRUE(loaded && *loaded);
  EXPECT_EQ((*loaded)->holes.size(), 2u);
  // A second save is the next revision of the same reference.
  EXPECT_EQ(*save_holder(*store_, actor, "24-hole", *h), *id);
  EXPECT_EQ(store_->history(*id, Kind::RefValue)->size(), 2u);
}

INSTANTIATE_TEST_SUITE_P(Engines, HolderStoreTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
