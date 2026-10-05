#include "pychron/vision/opencv_source.hpp"

#include "pychron/vision/camera_backend.hpp"

#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <exception>
#include <filesystem>
#include <utility>

#ifdef PYCHRON_VISION_OPENCV_ENABLED
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#endif

namespace pychron::vision {

#ifdef PYCHRON_VISION_OPENCV_ENABLED

namespace {

class OpenCvSource final : public IFrameSource {
 public:
  OpenCvSource(cv::VideoCapture cap, SourceConfig cfg, ClockFn clock)
      : cap_(std::move(cap)), cfg_(cfg), clock_(std::move(clock)) {}

  // No exception may leave the library: OpenCV reports bad input by throwing cv::Exception.
  Result<Frame> grab() override {
    try {
      return grab_impl();
    } catch (const std::exception& e) {
      return fail(ErrorKind::Io, e.what());
    }
  }

  Result<Frame> grab_impl() {
    cv::Mat raw;
    if (!cap_.read(raw) || raw.empty()) return fail(ErrorKind::Io, "end of stream");

    cv::Mat m = raw;
    if (cfg_.roi.w > 0 && cfg_.roi.h > 0) {
      const cv::Rect r = cv::Rect(cfg_.roi.x, cfg_.roi.y, cfg_.roi.w, cfg_.roi.h) & cv::Rect(0, 0, m.cols, m.rows);
      if (r.empty()) return fail(ErrorKind::Config, "roi lies outside the frame");
      m = m(r);
    }

    cv::Mat grey;
    if (m.channels() == 1) {
      grey = m;
    } else if (cfg_.channel == SourceConfig::Channel::Luma) {
      cv::cvtColor(m, grey, m.channels() == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
    } else {
      // OpenCV stores B, G, R.
      const int c = cfg_.channel == SourceConfig::Channel::R ? 2 : cfg_.channel == SourceConfig::Channel::G ? 1 : 0;
      cv::extractChannel(m, grey, c);
    }
    if (grey.depth() == CV_16U) {
      grey.convertTo(grey, CV_8U, 255.0 / 65535.0);
    } else if (grey.depth() != CV_8U) {
      return fail(ErrorKind::Io, "unsupported pixel type");
    }

    if (cfg_.flip_x) cv::flip(grey, grey, 1);
    if (cfg_.flip_y) cv::flip(grey, grey, 0);
    if (cfg_.rotate == 90) cv::rotate(grey, grey, cv::ROTATE_90_CLOCKWISE);
    if (cfg_.rotate == 180) cv::rotate(grey, grey, cv::ROTATE_180);
    if (cfg_.rotate == 270) cv::rotate(grey, grey, cv::ROTATE_90_COUNTERCLOCKWISE);

    Frame f = Frame::make(grey.cols, grey.rows, 255);
    for (int y = 0; y < grey.rows; ++y) {
      const std::uint8_t* row = grey.ptr<std::uint8_t>(y);
      for (int x = 0; x < grey.cols; ++x) f.data[static_cast<std::size_t>(y) * static_cast<std::size_t>(grey.cols) + static_cast<std::size_t>(x)] = row[x];
    }
    f.seq = ++seq_;
    // Stamped from the caller's clock, not the video position: controllers compare
    // this with their own `now`.
    f.timestamp = clock_();
    return f;
  }

  FrameInfo info() const override {
    try {
      return info_impl();
    } catch (const std::exception&) {
      return FrameInfo{0, 0, 255, 0.0};
    }
  }

  FrameInfo info_impl() const {
    int w = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
    int h = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
    if (cfg_.roi.w > 0 && cfg_.roi.h > 0) {
      const cv::Rect r = cv::Rect(cfg_.roi.x, cfg_.roi.y, cfg_.roi.w, cfg_.roi.h) & cv::Rect(0, 0, w, h);
      w = r.width;
      h = r.height;
    }
    if (cfg_.rotate == 90 || cfg_.rotate == 270) std::swap(w, h);
    const double fps = cap_.get(cv::CAP_PROP_FPS);
    return FrameInfo{w, h, 255, std::isfinite(fps) && fps > 0 ? fps : 0.0};
  }

 private:
  mutable cv::VideoCapture cap_;  // get() is not const in older OpenCV
  SourceConfig cfg_;
  ClockFn clock_;
  std::uint64_t seq_ = 0;
};

bool is_index(const std::string& s) {
  if (s.empty() || s.size() > 6) return false;  // longer cannot be a camera index (and would overflow stoi)
  for (const char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

}  // namespace

Result<std::unique_ptr<IFrameSource>> open_opencv_source(const std::string& uri, SourceConfig cfg, ClockFn clock) {
  if (!clock) clock = &std::chrono::steady_clock::now;
  if (cfg.rotate != 0 && cfg.rotate != 90 && cfg.rotate != 180 && cfg.rotate != 270)
    return fail(ErrorKind::Config, "rotate must be 0, 90, 180 or 270");
  try {
    cv::VideoCapture cap;
    if (is_index(uri)) {
      cap.open(std::stoi(uri));
    } else {
      // Check first so a missing path is a clear error rather than a backend log line.
      std::error_code ec;
      if (!std::filesystem::exists(uri, ec)) return fail(ErrorKind::Io, "no such video file: " + uri);
      cap.open(uri);
    }
    if (!cap.isOpened()) return fail(ErrorKind::Io, "cannot open " + uri);
    return std::unique_ptr<IFrameSource>(std::make_unique<OpenCvSource>(std::move(cap), cfg, std::move(clock)));
  } catch (const std::exception& e) {
    return fail(ErrorKind::Io, e.what());
  }
}

bool opencv_built() noexcept { return true; }

Result<std::unique_ptr<IFrameSource>> open_opencv_camera(const CameraRequest& request, ClockFn clock) {
  if (!clock) clock = &std::chrono::steady_clock::now;
  const SourceConfig& cfg = request.shape;
  if (cfg.rotate != 0 && cfg.rotate != 90 && cfg.rotate != 180 && cfg.rotate != 270)
    return fail(ErrorKind::Config, "rotate must be 0, 90, 180 or 270");
  const std::string uri = request.device.empty() ? "0" : request.device;
  try {
    cv::VideoCapture cap;
    if (is_index(uri)) {
      cap.open(std::stoi(uri));
      if (!cap.isOpened()) {
        return fail(ErrorKind::Io, "cannot open camera " + uri +
                                       " (is it there, in use by another program, or not allowed to this one?)");
      }
      // Asked for, not demanded: a camera gives the nearest it has.
      if (request.width > 0) cap.set(cv::CAP_PROP_FRAME_WIDTH, request.width);
      if (request.height > 0) cap.set(cv::CAP_PROP_FRAME_HEIGHT, request.height);
      if (request.fps > 0) cap.set(cv::CAP_PROP_FPS, request.fps);
    } else {
      std::error_code ec;
      if (!std::filesystem::exists(uri, ec)) return fail(ErrorKind::Io, "no such video file: " + uri);
      cap.open(uri);
      if (!cap.isOpened()) return fail(ErrorKind::Io, "cannot open " + uri);
    }
    return std::unique_ptr<IFrameSource>(std::make_unique<OpenCvSource>(std::move(cap), cfg, std::move(clock)));
  } catch (const std::exception& e) {
    return fail(ErrorKind::Io, e.what());
  }
}

std::vector<CameraFound> list_opencv_cameras() {
  std::vector<CameraFound> found;
  for (int index = 0; index < 8; ++index) {
    try {
      cv::VideoCapture cap(index);
      if (!cap.isOpened()) continue;
      CameraFound camera;
      camera.backend = "opencv";
      camera.device = std::to_string(index);
      camera.description = "camera " + std::to_string(index) + " (" + cap.getBackendName() + ")";
      camera.width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
      camera.height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
      const double fps = cap.get(cv::CAP_PROP_FPS);
      camera.fps = std::isfinite(fps) && fps > 0 ? fps : 0.0;
      found.push_back(std::move(camera));
    } catch (const std::exception&) {
      // not a camera this build can open
    }
  }
  return found;
}

#else

Result<std::unique_ptr<IFrameSource>> open_opencv_source(const std::string&, SourceConfig, ClockFn) {
  return fail(ErrorKind::Config, "built without OpenCV");
}

bool opencv_built() noexcept { return false; }

Result<std::unique_ptr<IFrameSource>> open_opencv_camera(const CameraRequest&, ClockFn) {
  return fail(ErrorKind::Config, "built without OpenCV");
}

std::vector<CameraFound> list_opencv_cameras() { return {}; }

#endif

}  // namespace pychron::vision
