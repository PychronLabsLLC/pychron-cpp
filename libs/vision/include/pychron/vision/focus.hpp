#pragma once

#include "pychron/vision/frame.hpp"

namespace pychron::vision::focus {

// Sharpness scores for a Z sweep: higher is sharper. Each divides by
// pixel_depth so 8-, 12- and 16-bit frames compare. All return 0 for frames
// smaller than 3x3, null data or pixel_depth == 0; borders are skipped.

// 99th percentile (nearest rank) of |4c - n - s - e - w| over interior pixels.
double laplace_p99(const FrameView&);
// Population variance of the pixel values, over pixel_depth squared.
double variance(const FrameView&);
// Mean 3x3 Sobel gradient magnitude over interior pixels.
double sobel_sum(const FrameView&);

}  // namespace pychron::vision::focus
