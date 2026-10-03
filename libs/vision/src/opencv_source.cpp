#include "pychron/vision/opencv_source.hpp"

#include <cctype>
#include <chrono>
#include <cmath>
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
  OpenCvSource(cv::VideoCapture cap, SourceConfig cfg) : cap_(std::move(cap)), cfg_(cfg) {}

  Result<Frame> grab() override {
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
    // Video position when the backend reports one; cameras report none.
    const double ms = cap_.get(cv::CAP_PROP_POS_MSEC);
    if (std::isfinite(ms) && ms > 0)
      f.timestamp = TimePoint(std::chrono::duration_cast<Duration>(std::chrono::duration<double, std::milli>(ms)));
    return f;
  }

  FrameInfo info() const override {
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
  std::uint64_t seq_ = 0;
};

bool is_index(const std::string& s) {
  if (s.empty() || s.size() > 6) return false;  // longer cannot be a camera index (and would overflow stoi)
  for (const char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

}  // namespace

Result<std::unique_ptr<IFrameSource>> open_opencv_source(const std::string& uri, SourceConfig cfg) {
  if (cfg.rotate != 0 && cfg.rotate != 90 && cfg.rotate != 180 && cfg.rotate != 270)
    return fail(ErrorKind::Config, "rotate must be 0, 90, 180 or 270");
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
  return std::unique_ptr<IFrameSource>(std::make_unique<OpenCvSource>(std::move(cap), cfg));
}

#else

Result<std::unique_ptr<IFrameSource>> open_opencv_source(const std::string&, SourceConfig) {
  return fail(ErrorKind::Config, "built without OpenCV");
}

#endif

}  // namespace pychron::vision
