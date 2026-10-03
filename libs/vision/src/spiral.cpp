#include "pychron/vision/spiral.hpp"

#include <cmath>
#include <numbers>

namespace pychron::vision {

Spiral::Spiral(SpiralKind kind, double base_mm, double square_growth)
    : kind_(kind), base_mm_(base_mm), growth_(square_growth), side_mm_(base_mm) {}

void Spiral::reset() {
  ring_ = 1;
  index_ = 0;
  side_mm_ = base_mm_;
}

Vec2 Spiral::next() {
  if (kind_ == SpiralKind::Square) {
    static constexpr double kDir[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    const Vec2 p{kDir[index_][0] * side_mm_, kDir[index_][1] * side_mm_};
    if (++index_ == 4) {
      index_ = 0;
      ++ring_;
      side_mm_ *= growth_;
    }
    return p;
  }

  const double r = static_cast<double>(ring_) * base_mm_;
  const std::uint64_t edge = index_ / ring_;
  const double t = static_cast<double>(index_ % ring_) / static_cast<double>(ring_);
  const double a0 = static_cast<double>(edge) * std::numbers::pi / 3.0;
  const double a1 = static_cast<double>(edge + 1) * std::numbers::pi / 3.0;
  const Vec2 v0{r * std::cos(a0), r * std::sin(a0)};
  const Vec2 v1{r * std::cos(a1), r * std::sin(a1)};
  const Vec2 p{v0.x + (v1.x - v0.x) * t, v0.y + (v1.y - v0.y) * t};
  if (++index_ == 6 * ring_) {
    index_ = 0;
    ++ring_;
  }
  return p;
}

}  // namespace pychron::vision
