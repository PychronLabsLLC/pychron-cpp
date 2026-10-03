#include "pychron/vision/focus.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pychron::vision::focus {

namespace {
// The 3x3 stencils need one pixel of margin on every side.
bool usable(const FrameView& v) { return v.data != nullptr && v.width >= 3 && v.height >= 3 && v.pixel_depth > 0; }
}  // namespace

double laplace_p99(const FrameView& v) {
  if (!usable(v)) return 0;
  std::vector<double> mags;
  mags.reserve(static_cast<std::size_t>(v.width - 2) * static_cast<std::size_t>(v.height - 2));
  for (int y = 1; y < v.height - 1; ++y)
    for (int x = 1; x < v.width - 1; ++x) {
      const int l = 4 * v.at(x, y) - v.at(x, y - 1) - v.at(x, y + 1) - v.at(x - 1, y) - v.at(x + 1, y);
      mags.push_back(static_cast<double>(std::abs(l)));
    }
  // Nearest rank: the ceil(0.99 n)-th smallest value.
  const auto n = mags.size();
  const auto rank = static_cast<std::size_t>(std::ceil(0.99 * static_cast<double>(n)));
  const auto idx = std::min(n - 1, rank == 0 ? std::size_t{0} : rank - 1);
  std::nth_element(mags.begin(), mags.begin() + static_cast<std::ptrdiff_t>(idx), mags.end());
  return mags[idx] / v.pixel_depth;
}

double variance(const FrameView& v) {
  if (!usable(v)) return 0;
  double sum = 0, sum2 = 0;
  for (int y = 0; y < v.height; ++y)
    for (int x = 0; x < v.width; ++x) {
      const double p = v.at(x, y);
      sum += p;
      sum2 += p * p;
    }
  const double n = static_cast<double>(v.width) * v.height;
  const double mean = sum / n;
  // Rounding can push a constant frame a hair below zero.
  const double var = std::max(0.0, sum2 / n - mean * mean);
  return var / (static_cast<double>(v.pixel_depth) * v.pixel_depth);
}

double sobel_sum(const FrameView& v) {
  if (!usable(v)) return 0;
  double total = 0;
  for (int y = 1; y < v.height - 1; ++y)
    for (int x = 1; x < v.width - 1; ++x) {
      const int gx = (v.at(x + 1, y - 1) + 2 * v.at(x + 1, y) + v.at(x + 1, y + 1)) -
                     (v.at(x - 1, y - 1) + 2 * v.at(x - 1, y) + v.at(x - 1, y + 1));
      const int gy = (v.at(x - 1, y + 1) + 2 * v.at(x, y + 1) + v.at(x + 1, y + 1)) -
                     (v.at(x - 1, y - 1) + 2 * v.at(x, y - 1) + v.at(x + 1, y - 1));
      total += std::hypot(static_cast<double>(gx), static_cast<double>(gy));
    }
  const double interior = static_cast<double>(v.width - 2) * (v.height - 2);
  return total / v.pixel_depth / interior;
}

}  // namespace pychron::vision::focus
