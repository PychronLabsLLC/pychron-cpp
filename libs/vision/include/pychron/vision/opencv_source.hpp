#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
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

// Same shape as SyntheticSource::ClockFn; empty means steady_clock::now.
using ClockFn = std::function<TimePoint()>;

// uri: a video file path or a decimal camera index. Frames are 8-bit
// (pixel_depth 255), numbered from 1 and stamped from `clock` at grab() time.
// ErrorKind::Config when built without OpenCV.
Result<std::unique_ptr<IFrameSource>> open_opencv_source(const std::string& uri, SourceConfig, ClockFn clock = {});

struct CameraRequest;
struct CameraFound;

// Whether this build has OpenCV.
bool opencv_built() noexcept;
// A camera or file by a request: its device is the uri above (empty: camera
// 0); a size and rate are asked of the camera, which may give its nearest.
Result<std::unique_ptr<IFrameSource>> open_opencv_camera(const CameraRequest& request, ClockFn clock = {});
// OpenCV cannot list cameras: indexes 0 to 7 are opened in turn, and each
// that opens is listed with its size and rate. Empty without OpenCV.
std::vector<CameraFound> list_opencv_cameras();

}  // namespace pychron::vision
