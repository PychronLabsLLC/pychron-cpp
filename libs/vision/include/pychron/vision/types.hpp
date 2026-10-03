#pragma once

namespace pychron::vision {

struct Vec2 {
  double x = 0, y = 0;
};
using Point2 = Vec2;

// Integer pixel rectangle; x,y is the top-left pixel.
struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

}  // namespace pychron::vision
