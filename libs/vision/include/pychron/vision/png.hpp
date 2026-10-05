#pragma once

#include <filesystem>

#include "pychron/core/error.hpp"
#include "pychron/vision/frame.hpp"

namespace pychron::vision {

// A grey PNG: 8 bits a pixel when pixel_depth <= 255, else 16 (scaled so that
// pixel_depth is white). Not compressed (stored deflate blocks): a picture
// any viewer opens, written with no library. Config error for an empty
// view; Io error, and no file, when it cannot be written.
Result<void> write_png(const std::filesystem::path& file, const FrameView& view);

}  // namespace pychron::vision
