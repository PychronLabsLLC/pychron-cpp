#pragma once

#include <filesystem>

#include "pychron/core/error.hpp"
#include "pychron/vision/frame.hpp"

namespace pychron::vision {

// Binary P5 only. maxval 1..65535; above 255 each pixel is two bytes,
// big-endian. Frame::pixel_depth is the maxval. '#' comments are allowed
// anywhere in the header.
Result<Frame> read_pgm(const std::filesystem::path&);
// One byte per pixel when pixel_depth <= 255, else two.
Result<void> write_pgm(const std::filesystem::path&, const FrameView&);

}  // namespace pychron::vision
