#pragma once

#include <cstdint>

#include "pychron/vision/types.hpp"

namespace pychron::vision {

enum class SpiralKind { Hexagon, Square };

// Lazy, deterministic search pattern. Points are offsets from an anchor the
// caller owns. base_mm must be positive and finite; Dragonfly checks that
// before constructing one.
//
// Hexagon: ring k >= 1 has 6k points on the hexagon of circumradius k*base,
// walked edge by edge counter-clockwise from the vertex at angle 0.
// Square: lap n >= 1 visits (+s,0) (0,+s) (-s,0) (0,-s), s = base*growth^(n-1).
class Spiral {
 public:
  Spiral(SpiralKind kind, double base_mm, double square_growth = 1.1);

  Vec2 next();  // never {0, 0}
  void reset();
  // 1-based ring/lap of the point last returned by next(); 0 before the first call and after reset().
  int ring() const { return static_cast<int>(last_ring_); }

 private:
  SpiralKind kind_;
  double base_mm_;
  double growth_;
  std::uint64_t ring_ = 1;  // ring (hexagon) or lap (square), 1-based
  std::uint64_t last_ring_ = 0;
  std::uint64_t index_ = 0;  // point within the ring
  double side_mm_;           // square: current lap's side
};

}  // namespace pychron::vision
