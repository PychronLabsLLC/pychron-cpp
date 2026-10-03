#pragma once

#include <memory>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/vision/source.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::vision {

// Applied in this order: roi, channel, flip, rotate.
struct SourceConfig {
  Rect roi{};  // zero size: the whole frame
  enum class Channel { Luma, R, G, B } channel = Channel::Luma;
  bool flip_x = false, flip_y = false;
  int rotate = 0;  // 0, 90, 180, 270 (clockwise); anything else is ErrorKind::Config
};

// uri: a video file path or a decimal camera index. Frames are 8-bit
// (pixel_depth 255), numbered from 1. ErrorKind::Config when built without OpenCV.
Result<std::unique_ptr<IFrameSource>> open_opencv_source(const std::string& uri, SourceConfig);

}  // namespace pychron::vision
