#include "pychron/processing/scene.hpp"

#include <gtest/gtest.h>

#include <cstddef>

namespace pp = pychron::processing;

TEST(Palette, DefaultHasTwelveInOrder) {
  const auto p = pp::default_palette();
  ASSERT_EQ(p.size(), 12u);
  EXPECT_EQ(pp::color_hex(p[0]), "#1f77b4");
  EXPECT_EQ(pp::color_hex(p[6]), "#e377c2");
  EXPECT_EQ(pp::color_hex(p[10]), "#000080");
  EXPECT_EQ(pp::color_hex(p[11]), "#000000");
}

TEST(Palette, PaletteColorCyclesTheDefault) {
  const auto p = pp::default_palette();
  for (int i = 0; i < 12; ++i) {
    EXPECT_EQ(pp::palette_color(i), p[static_cast<std::size_t>(i)]);
  }
  EXPECT_EQ(pp::palette_color(12), p[0]);
  EXPECT_EQ(pp::palette_color(25), p[1]);
}

TEST(Palette, NegativeIndexWrapsFromTheEnd) {
  EXPECT_EQ(pp::palette_color(-1), pp::default_palette()[11]);
  EXPECT_EQ(pp::palette_color(-13), pp::default_palette()[11]);
}
