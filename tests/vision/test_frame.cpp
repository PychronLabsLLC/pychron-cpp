#include <gtest/gtest.h>

#include "pychron/vision/frame.hpp"

using namespace pychron::vision;

TEST(Frame, MakeFillsAndAddressesRowMajor) {
  auto f = Frame::make(4, 3, 4095, 9);
  EXPECT_EQ(f.data.size(), 12U);
  EXPECT_EQ(f.pixel_depth, 4095);
  f.at(3, 2) = 100;
  EXPECT_EQ(f.data[2 * 4 + 3], 100);
  EXPECT_EQ(f.view().at(3, 2), 100);
  EXPECT_EQ(f.view().stride, 4);
  EXPECT_EQ(f.view().at(0, 0), 9);
}

TEST(Frame, CropClampsToFrame) {
  auto f = Frame::make(10, 8, 255, 7);
  auto c = crop(f.view(), Rect{6, 5, 10, 10});
  EXPECT_EQ(c.width, 4);
  EXPECT_EQ(c.height, 3);
  EXPECT_EQ(c.view().at(3, 2), 7);
}

TEST(Frame, CropFullyOutsideIsEmpty) {
  auto f = Frame::make(10, 8, 255);
  auto c = crop(f.view(), Rect{20, 20, 5, 5});
  EXPECT_EQ(c.width, 0);
  EXPECT_EQ(c.height, 0);
  EXPECT_TRUE(c.data.empty());
}

TEST(Frame, CropCopiesTheRightPixels) {
  auto f = Frame::make(10, 8, 255);
  f.at(4, 3) = 200;
  auto c = crop(f.view(), Rect{3, 2, 4, 4});
  EXPECT_EQ(c.view().at(1, 1), 200);
  EXPECT_EQ(c.view().at(0, 0), 0);
}

TEST(Frame, CenteredRectIsCentredAndOffsettable) {
  auto f = Frame::make(100, 80, 255);
  auto r = centered_rect(f.view(), 20);
  EXPECT_EQ(r.x, 40);
  EXPECT_EQ(r.y, 30);
  EXPECT_EQ(r.w, 20);
  EXPECT_EQ(r.h, 20);
  auto o = centered_rect(f.view(), 20, Vec2{5, -3});
  EXPECT_EQ(o.x, 45);
  EXPECT_EQ(o.y, 27);
}
